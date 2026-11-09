// Minimal .zip extraction for "Add mod", so it works without an external tool. tar.exe (bsdtar) is only on Windows 10+,
// and not in Wine / Proton, where adding a .zip used to fail. Stored and deflated entries (libdeflate), CRC-checked;
// encrypted entries, ZIP64 archives and names that would land outside the target folder ("..", absolute, drive
// letters) are refused.

#include "zip.h"

#include <windows.h>
#include <libdeflate.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <set>

namespace {

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

bool read_all(const std::wstring& path, std::vector<uint8_t>& out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart <= (256ll << 20);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

bool write_all(const std::wstring& path, const uint8_t* data, size_t n)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = n == 0 || (WriteFile(f, data, DWORD(n), &put, nullptr) && put == n);
    CloseHandle(f);
    return ok;
}

// "folder/sub/file.txt" -> L"folder\\sub\\file.txt"; empty if it would leave the target folder.
std::wstring safe_name(const char* name, size_t len, bool utf8)
{
    int n = MultiByteToWideChar(utf8 ? CP_UTF8 : 437, utf8 ? MB_ERR_INVALID_CHARS : 0, name, int(len), nullptr, 0);
    if (!n || n >= 200) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(utf8 ? CP_UTF8 : 437, 0, name, int(len), w.data(), n);
    for (auto& c : w)
        if (c == L'/') c = L'\\';
    if (w.empty() || w[0] == L'\\' || w.find(L':') != std::wstring::npos) return L"";
    size_t start = 0;
    while (start <= w.size()) {
        size_t end = w.find(L'\\', start);
        std::wstring part = w.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty()) { if (start == w.size()) break; return {}; }
        if (part == L".." || part == L"." || part.back() == L'.' || part.back() == L' ') return {};
        for (wchar_t c : part) if (c < 32 || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') return {};
        std::wstring base=part.substr(0,part.find(L'.')); CharLowerBuffW(base.data(),DWORD(base.size()));
        if (base==L"con"||base==L"prn"||base==L"aux"||base==L"nul"||
            (base.size()==4&&(base.starts_with(L"com")||base.starts_with(L"lpt"))&&base[3]>=L'1'&&base[3]<=L'9')) return {};
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return w;
}

void make_dirs(const std::wstring& root, const std::wstring& rel)
{
    size_t pos = 0;
    while ((pos = rel.find(L'\\', pos)) != std::wstring::npos) {
        CreateDirectoryW((root + L"\\" + rel.substr(0, pos)).c_str(), nullptr);
        pos++;
    }
}

} // namespace

bool zip_extract(const std::wstring& zip, const std::wstring& dest, std::wstring& error)
{
    std::vector<uint8_t> z;
    if (!read_all(zip, z)) {
        error = L"could not read the file";
        return false;
    }
    // End of central directory: the last "PK\5\6" within the final 64 KB + 22 bytes.
    size_t eocd = std::string::npos;
    for (size_t i = z.size() >= 22 ? z.size() - 22 : 0; z.size() >= 22; i--) {
        if (u32(&z[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0 || z.size() - i > 65557) break;
    }
    if (eocd == std::string::npos) {
        error = L"not a .zip file";
        return false;
    }
    uint16_t count = u16(&z[eocd + 10]);
    uint32_t cd_size = u32(&z[eocd + 12]), cd_off = u32(&z[eocd + 16]);
    if (count > 4096 || cd_off == 0xffffffff || size_t(cd_off) + cd_size > z.size()) {
        error = L"unsupported .zip (ZIP64 or damaged)";
        return false;
    }
    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    if (!dec) { error=L"could not allocate decompressor"; return false; }
    size_t p = cd_off;
    bool ok = true;
    std::vector<uint8_t> out;
    std::set<std::wstring> names;
    uint64_t expanded=0;
    for (uint16_t e = 0; e < count && ok; e++) {
        if (p + 46 > z.size() || u32(&z[p]) != 0x02014b50) {
            error = L"damaged central directory";
            ok = false;
            break;
        }
        uint16_t flags = u16(&z[p + 8]), method = u16(&z[p + 10]);
        uint32_t crc = u32(&z[p + 16]), csize = u32(&z[p + 20]), usize = u32(&z[p + 24]);
        expanded+=usize;
        if (usize>(128u<<20)||expanded>(1ull<<30)||((u32(&z[p+38])>>16)&0170000)==0120000) {
            error=L"archive exceeds size limits or contains a symbolic link"; ok=false; break;
        }
        uint16_t nlen = u16(&z[p + 28]), xlen = u16(&z[p + 30]), clen = u16(&z[p + 32]);
        uint32_t local = u32(&z[p + 42]);
        if (p + 46 + nlen > z.size()) {
            error = L"damaged central directory";
            ok = false;
            break;
        }
        std::wstring name = safe_name(reinterpret_cast<const char*>(&z[p + 46]), nlen, (flags & 0x800) != 0);
        p += 46 + nlen + xlen + clen;
        if (name.empty()) {
            error = L"an entry's name points outside the folder";
            ok = false;
            break;
        }
        auto normalized=name; CharLowerBuffW(normalized.data(),DWORD(normalized.size()));
        if (!names.insert(normalized).second) { error=L"duplicate archive path"; ok=false; break; }
        if (name.back() == L'\\') { // folder entry
            make_dirs(dest, name);
            continue;
        }
        if (flags & 1) {
            error = L"encrypted .zip files aren't supported";
            ok = false;
            break;
        }
        if (size_t(local) + 30 > z.size() || u32(&z[local]) != 0x04034b50) {
            error = L"damaged entry";
            ok = false;
            break;
        }
        size_t data = size_t(local) + 30 + u16(&z[local + 26]) + u16(&z[local + 28]);
        if (data + csize > z.size()) {
            error = L"damaged entry";
            ok = false;
            break;
        }
        out.resize(usize);
        if (method == 0 && csize == usize) {
            if (usize) memcpy(out.data(), &z[data], usize);
        } else if (method == 8) {
            size_t got = 0;
            if (libdeflate_deflate_decompress(dec, &z[data], csize, out.data(), usize, &got) != LIBDEFLATE_SUCCESS || got != usize) {
                error = L"could not decompress " + name;
                ok = false;
                break;
            }
        } else {
            error = L"unsupported compression in " + name + L" (use a normal .zip)";
            ok = false;
            break;
        }
        if (libdeflate_crc32(0, out.data(), out.size()) != crc) {
            error = L"checksum mismatch in " + name;
            ok = false;
            break;
        }
        make_dirs(dest, name);
        if (!write_all(dest + L"\\" + name, out.data(), out.size())) {
            error = L"could not write " + name;
            ok = false;
        }
    }
    libdeflate_free_decompressor(dec);
    return ok;
}
