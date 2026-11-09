#pragma once
#include "mod_dependencies.h"
#include "swml_api.h"
#include <windows.h>
#include <string>
#include <stdexcept>
#include <iterator>

// Shared by the loader and core; no mod DLL must be executed to inspect metadata.
namespace swml_manifest {
inline std::wstring read(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buffer[8192]{};
    DWORD count = GetPrivateProfileStringW(section, key, fallback, buffer, DWORD(std::size(buffer)), file.c_str());
    if (count >= std::size(buffer) - 1) throw std::runtime_error("manifest value too long");
    return buffer;
}
inline std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("invalid UTF-16 metadata");
    std::string out(size_t(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}
inline std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("invalid UTF-8 metadata");
    std::wstring out(size_t(count), L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), count); return out;
}
inline bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 48 && id[0] >= 'a' && id[0] <= 'z' &&
        std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}
inline bool relative_path(const std::wstring& path) {
    if (path.empty() || path.size() >= 200 || path.front() == L'/' || path.front() == L'\\' || path.find_first_of(L":*?\"<>|") != path.npos) return false;
    size_t at = 0;
    while (at <= path.size()) {
        auto end = path.find_first_of(L"/\\", at); auto part = path.substr(at, end - at);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        for (wchar_t c : part) if (c < 32) return false;
        if (end == path.npos) break; at = end + 1;
    }
    return true;
}
inline bool wine() {
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}
inline swml_dependencies::Mod inspect(const std::wstring& directory, const std::wstring& id,
                                      const std::wstring& config, uint32_t api = SWML_API_VERSION) {
    swml_dependencies::Mod out;
    out.id = utf8(id);
    std::wstring manifest = directory + L"\\mod.ini";
    auto value = [&](const wchar_t* key) { return utf8(read(manifest, L"mod", key)); };
    // [profile] default_disabled: written by install_linux.sh on a fresh Linux install, unlisted mods stay off there.
    int fallback=GetPrivateProfileIntW(L"profile",L"default_disabled",0,config.c_str())?0:GetPrivateProfileIntW(L"mod",L"default_enabled",1,manifest.c_str());
    out.enabled = GetPrivateProfileIntW(L"mods", id.c_str(), fallback, config.c_str()) != 0;
    out.version = value(L"version"); out.required = swml_dependencies::split(value(L"depends"));
    out.optional = swml_dependencies::split(value(L"optional_depends"));
    out.conflicts = swml_dependencies::split(value(L"conflicts"));
    out.before = swml_dependencies::split(value(L"load_before")); out.after = swml_dependencies::split(value(L"load_after"));
    auto declared = value(L"id");
    if (!valid_id(out.id) || (!declared.empty() && declared != out.id)) out.problem = "invalid or mismatched mod id";
    if (GetPrivateProfileIntW(L"mod", L"api", 1, manifest.c_str()) > api) out.problem = "requires a newer SWML core API";
    auto dll = read(manifest, L"mod", L"dll");
    if (!dll.empty() && (!relative_path(dll) || dll.find_first_of(L"/\\") != dll.npos)) out.problem = "invalid DLL filename";
    auto data = read(manifest, L"mod", L"game_data");
    if (!data.empty() && !relative_path(data)) out.problem = "invalid game_data path";
    auto platforms = swml_dependencies::split(value(L"platforms"));
    if (!platforms.empty() && std::find(platforms.begin(), platforms.end(), wine() ? "proton" : "windows") == platforms.end())
        out.problem = "mod does not support this platform";
    return out;
}
} // namespace swml_manifest
