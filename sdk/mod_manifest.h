#pragma once
#include "../launcher/platform.h"
#include "mod_dependencies.h"
#include "swml_api.h"
#include <stdexcept>
#include <string>

// Shared by the launcher and core; no mod DLL must be executed to inspect metadata.
// All OS calls go through swml_platform so this header builds for Windows (MSVC), Wine/Proton
// and the native Linux launcher with identical semantics.
namespace swml_manifest {
inline std::wstring read(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    auto value = swml_platform::ini_read(file, section, key, fallback);
    if (value.size() >= 8191) throw std::runtime_error("manifest value too long");
    return value;
}
inline std::string utf8(const std::wstring& text) { return swml_platform::utf8(text); }
inline std::wstring wide(const std::string& text) { return swml_platform::wide(text); }
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
inline bool wine() { return swml_platform::is_wine(); }
inline swml_dependencies::Mod inspect(const std::wstring& directory, const std::wstring& id,
                                      const std::wstring& config, uint32_t api = SWML_API_VERSION) {
    swml_dependencies::Mod out;
    out.id = utf8(id);
    std::wstring manifest = (std::filesystem::path(directory) / "mod.ini").wstring();
    auto value = [&](const wchar_t* key) { return utf8(read(manifest.c_str(), L"mod", key)); };
    // [profile] default_disabled: written by install_linux.sh on a fresh Linux install, unlisted mods stay off there.
    int fallback = swml_platform::ini_read_int(config, L"profile", L"default_disabled", 0) ? 0
                 : swml_platform::ini_read_int(manifest, L"mod", L"default_enabled", 1);
    out.enabled = swml_platform::ini_read_int(config, L"mods", id.c_str(), fallback) != 0;
    out.version = value(L"version"); out.required = swml_dependencies::split(value(L"depends"));
    out.optional = swml_dependencies::split(value(L"optional_depends"));
    out.conflicts = swml_dependencies::split(value(L"conflicts"));
    out.before = swml_dependencies::split(value(L"load_before")); out.after = swml_dependencies::split(value(L"load_after"));
    auto declared = value(L"id");
    if (!valid_id(out.id) || (!declared.empty() && declared != out.id)) out.problem = "invalid or mismatched mod id";
    if (swml_platform::ini_read_int(manifest, L"mod", L"api", 1) > int(api)) out.problem = "requires a newer SWML core API";
    auto dll = read(manifest.c_str(), L"mod", L"dll");
    if (!dll.empty() && (!relative_path(dll) || dll.find_first_of(L"/\\") != dll.npos)) out.problem = "invalid DLL filename";
    auto data = read(manifest.c_str(), L"mod", L"game_data");
    if (!data.empty() && !relative_path(data)) out.problem = "invalid game_data path";
    auto platforms = swml_dependencies::split(value(L"platforms"));
    if (!platforms.empty() && std::find(platforms.begin(), platforms.end(), swml_platform::game_platform()) == platforms.end())
        out.problem = "mod does not support this platform";
    return out;
}
} // namespace swml_manifest
