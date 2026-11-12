// Windows core subset of the platform layer: text, INI profiles, atomic files, locks.
// Only kernel32/advapi32 are used here, so linking this into swml_core.dll adds no shell,
// COM or network imports. The desktop subset lives in platform_desktop_win.cpp.
#include "platform.h"

#include <windows.h>

#include <cstring>
#include <stdexcept>

namespace swml_platform {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

struct HandleClose { void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
using Handle = std::unique_ptr<void, HandleClose>;

uint32_t random_word()
{
    ULONG value = 0;
    // advapi32 (SystemFunction036), no bcrypt import needed for staging names.
    using RtlGenRandom = BOOLEAN(WINAPI*)(void*, ULONG);
    static auto gen = reinterpret_cast<RtlGenRandom>(GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "SystemFunction036"));
    if (gen && gen(&value, sizeof(value))) return value;
    return GetTickCount() ^ GetCurrentProcessId();
}
} // namespace

std::wstring wide(const std::string& text)
{
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!count) fail("invalid UTF-8 text");
    std::wstring out(size_t(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), count);
    return out;
}

std::string utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) fail("invalid UTF-16 text");
    std::string out(size_t(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::wstring lower(std::wstring text)
{
    if (!text.empty()) CharLowerBuffW(text.data(), DWORD(text.size()));
    return text;
}

int compare_icase(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()); }

bool is_wine()
{
    static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    return wine;
}

const char* game_platform() { return is_wine() ? "proton" : "windows"; }

std::wstring ini_read(const std::wstring& file, const std::wstring& section, const std::wstring& key,
                      const std::wstring& fallback)
{
    wchar_t buffer[8192];
    DWORD count = GetPrivateProfileStringW(section.c_str(), key.c_str(), fallback.c_str(), buffer,
                                           DWORD(std::size(buffer)), file.c_str());
    // The Win32 API truncates silently; callers that need the overflow signal use ini_sections.
    return std::wstring(buffer, count >= std::size(buffer) ? std::size(buffer) - 1 : count);
}

int ini_read_int(const std::wstring& file, const std::wstring& section, const std::wstring& key, int fallback)
{
    return GetPrivateProfileIntW(section.c_str(), key.c_str(), fallback, file.c_str());
}

bool ini_write(const std::wstring& file, const std::wstring& section, const std::wstring& key, const std::wstring* value)
{
    return WritePrivateProfileStringW(section.c_str(), key.c_str(), value ? value->c_str() : nullptr, file.c_str()) != 0;
}

std::vector<IniSection> ini_sections(const std::wstring& file)
{
    std::vector<IniSection> out;
    std::vector<wchar_t> names(65536);
    if (GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), file.c_str()) >= names.size() - 2)
        fail("too many INI sections");
    for (const wchar_t* name = names.data(); *name; name += wcslen(name) + 1) {
        IniSection section;
        section.name = name;
        std::vector<wchar_t> entries(65536);
        if (GetPrivateProfileSectionW(name, entries.data(), DWORD(entries.size()), file.c_str()) >= entries.size() - 2)
            fail("INI section too large: " + utf8(section.name));
        for (const wchar_t* entry = entries.data(); *entry; entry += wcslen(entry) + 1) {
            std::wstring text = entry;
            auto equal = text.find(L'=');
            if (equal == text.npos) continue;
            section.entries.push_back({text.substr(0, equal), text.substr(equal + 1)});
        }
        out.push_back(std::move(section));
    }
    return out;
}

void ini_flush(const std::wstring& file)
{
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
}

void write_bytes_atomic(const fs::path& path, std::string_view data)
{
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(random_word());
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) fail("could not create " + utf8(temporary.wstring()));
    DWORD written = 0;
    bool ok = data.size() <= MAXDWORD &&
              WriteFile(file.get(), data.data(), DWORD(data.size()), &written, nullptr) && written == data.size() &&
              FlushFileBuffers(file.get());
    file.reset();
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        fail("could not commit " + utf8(path.wstring()));
    }
}

void ini_write_all(const std::wstring& file, const std::wstring& header, const std::vector<IniKeyValue>& values)
{
    std::wstring text = header;
    std::wstring section;
    for (const auto& item : values) {
        if (item.value.find_first_of(L"\r\n") != item.value.npos || item.value.find(L'\0') != item.value.npos)
            fail("invalid multiline setting");
        if (item.section != section) { section = item.section; text += L"\r\n[" + section + L"]\r\n"; }
        text += item.key + L"=" + item.value + L"\r\n";
    }
    std::string bytes("\xff\xfe", 2); // UTF-16LE BOM, like the old settings store wrote
    bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    write_bytes_atomic(fs::path(file), bytes);
}

bool path_exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool is_link(const fs::path& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

uint32_t process_id() { return GetCurrentProcessId(); }

struct DirLock::Impl {
    Handle mutex;
};

DirLock::DirLock(const std::wstring& dir) : impl_(new Impl)
{
    uint64_t hash = 14695981039346656037ull;
    for (auto c : lower(fs::absolute(fs::path(dir)).wstring())) { hash ^= uint16_t(c); hash *= 1099511628211ull; }
    auto name = L"Local\\SWML_settings_" + std::to_wstring(hash);
    impl_->mutex.reset(CreateMutexW(nullptr, FALSE, name.c_str()));
    if (!impl_->mutex) { delete impl_; impl_ = nullptr; fail("settings lock unavailable"); }
    auto result = WaitForSingleObject(impl_->mutex.get(), 5000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        delete impl_; impl_ = nullptr;
        fail("settings are busy");
    }
}

DirLock::~DirLock()
{
    if (impl_) {
        if (impl_->mutex) ReleaseMutex(impl_->mutex.get());
        delete impl_;
    }
}

bool DirLock::locked() const { return impl_ != nullptr; }

} // namespace swml_platform
