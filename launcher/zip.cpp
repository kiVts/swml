// Minimal .zip extraction for "Add mod", so it works without an external tool. Stored and
// deflated entries (libdeflate), CRC-checked; encrypted entries, ZIP64 archives and names that
// would land outside the target folder ("..", absolute, drive letters, Windows reserved device
// names) are refused. Portable: the same reader runs in the Windows launcher and the native
// Linux launcher (and under Wine/Proton, where tar.exe isn't available).

#include "zip.h"
#include "platform.h"

#include <libdeflate.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <set>

namespace {
namespace fs = std::filesystem;

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

bool read_all(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::ifstream f(fs::path(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto size = f.tellg();
    if (size < 0 || size > (256ll << 20)) return false;
    out.resize(size_t(size));
    f.seekg(0);
    return out.empty() || bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

bool write_all(const std::wstring& path, const uint8_t* data, size_t n)
{
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    return n == 0 || bool(f.write(reinterpret_cast<const char*>(data), std::streamsize(n)));
}

// Codepage 437 -> Unicode, for archives without the UTF-8 flag (the classic DOS/Windows case).
wchar_t cp437(unsigned char c)
{
    static const wchar_t high[128] = {
        0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
        0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
        0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
        0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
        0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
        0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
        0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
        0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
    };
    return c < 0x80 ? wchar_t(c) : high[c - 0x80];
}

// "folder/sub/file.txt" -> "folder\sub\file.txt" (wide, platform separators); empty if unsafe.
std::wstring safe_name(const char* name, size_t len, bool utf8)
{
    std::wstring w;
    if (utf8) {
        try { w = swml_platform::wide(std::string(name, len)); }
        catch (const std::exception&) { return {}; }
    } else {
        for (size_t i = 0; i < len; i++) w += cp437(static_cast<unsigned char>(name[i]));
    }
    if (w.empty() || w.size() >= 200) return {};
    for (auto& c : w)
        if (c == L'/') c = L'\\';
    if (w[0] == L'\\' || w.find(L':') != std::wstring::npos) return L"";
    size_t start = 0;
    while (start <= w.size()) {
        size_t end = w.find(L'\\', start);
        std::wstring part = w.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty()) { if (start == w.size()) break; return {}; }
        if (part == L".." || part == L"." || part.back() == L'.' || part.back() == L' ') return {};
        for (wchar_t c : part) if (c < 32 || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') return {};
        std::wstring base = swml_platform::lower(part.substr(0, part.find(L'.')));
        if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul" ||
            (base.size() == 4 && (base.starts_with(L"com") || base.starts_with(L"lpt")) && base[3] >= L'1' && base[3] <= L'9')) return {};
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return w;
}

fs::path entry_path(const std::wstring& root, const std::wstring& rel)
{
    fs::path p(root);
    size_t start = 0;
    while (start < rel.size()) {
        auto end = rel.find(L'\\', start);
        auto part = rel.substr(start, end == rel.npos ? rel.npos : end - start);
        if (!part.empty()) p /= part;
        if (end == rel.npos) break;
        start = end + 1;
    }
    return p;
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
    if (!dec) { error = L"could not allocate decompressor"; return false; }
    size_t p = cd_off;
    bool ok = true;
    std::vector<uint8_t> out;
    std::set<std::wstring> names;
    uint64_t expanded = 0;
    for (uint16_t e = 0; e < count && ok; e++) {
        if (p + 46 > z.size() || u32(&z[p]) != 0x02014b50) {
            error = L"damaged central directory";
            ok = false;
            break;
        }
        uint16_t flags = u16(&z[p + 8]), method = u16(&z[p + 10]);
        uint32_t crc = u32(&z[p + 16]), csize = u32(&z[p + 20]), usize = u32(&z[p + 24]);
        expanded += usize;
        if (usize > (128u << 20) || expanded > (1ull << 30) || ((u32(&z[p + 38]) >> 16) & 0170000) == 0120000) {
            error = L"archive exceeds size limits or contains a symbolic link"; ok = false; break;
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
        auto normalized = swml_platform::lower(name);
        if (!names.insert(normalized).second) { error = L"duplicate archive path"; ok = false; break; }
        if (name.back() == L'\\') { // folder entry
            std::error_code ignored;
            fs::create_directories(entry_path(dest, name), ignored);
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
        auto target = entry_path(dest, name);
        std::error_code ignored;
        fs::create_directories(target.parent_path(), ignored);
        if (!write_all(target.wstring(), out.data(), out.size())) {
            error = L"could not write " + name;
            ok = false;
        }
    }
    libdeflate_free_decompressor(dec);
    return ok;
}
