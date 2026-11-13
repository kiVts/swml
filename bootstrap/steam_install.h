#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace swml_steam_install {
namespace fs = std::filesystem;
struct Entry {
    std::string key, value;
    std::vector<Entry> children;
    bool object = false;
};
inline std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
class Vdf {
    const std::string& text;
    size_t at = 0, entries = 0;
    void space() {
        for (;;) {
            while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
            if (at + 1 < text.size() && text[at] == '/' && text[at + 1] == '/') {
                while (at < text.size() && text[at] != '\n') ++at;
            } else break;
        }
    }
    std::string token() {
        space(); std::string out;
        if (at >= text.size() || text[at] == '{' || text[at] == '}') throw std::runtime_error("Invalid Steam record");
        if (text[at] == '"') {
            ++at;
            while (at < text.size()) {
                char c = text[at++];
                if (c == '"') return out;
                if (c == '\\' && at < text.size() && (text[at] == '\\' || text[at] == '"')) c = text[at++];
                out += c;
            }
            throw std::runtime_error("Truncated Steam record");
        }
        while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at])) && text[at] != '{' && text[at] != '}') out += text[at++];
        return out;
    }
    std::vector<Entry> object(unsigned depth) {
        if (depth > 32) throw std::runtime_error("Steam record too deep");
        std::vector<Entry> out;
        for (;;) {
            space();
            if (at == text.size()) {
                if (depth) throw std::runtime_error("Truncated Steam object");
                return out;
            }
            if (text[at] == '}') {
                if (!depth) throw std::runtime_error("Unexpected Steam object end");
                ++at; return out;
            }
            if (++entries > 100000) throw std::runtime_error("Steam record too large");
            Entry e; e.key = token(); space();
            if (at < text.size() && text[at] == '{') { ++at; e.object = true; e.children = object(depth + 1); }
            else e.value = token();
            out.push_back(std::move(e));
        }
    }
public:
    explicit Vdf(const std::string& value) : text(value) {
        if (text.starts_with("\xef\xbb\xbf")) at = 3;
    }
    std::vector<Entry> parse() { return object(0); }
};
inline const Entry* find(const std::vector<Entry>& entries, const char* key) {
    for (const auto& entry : entries) if (lower(entry.key) == lower(key)) return &entry;
    return nullptr;
}
inline std::vector<Entry> read(const fs::path& path) {
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        auto size = input.tellg();
        if (size < 0 || size > 4 * 1024 * 1024) return {};
        std::string text(size_t(size), '\0'); input.seekg(0);
        if (!input.read(text.data(), std::streamsize(text.size()))) return {};
        return Vdf(text).parse();
    } catch (...) { return {}; }
}
inline void add(std::vector<fs::path>& paths, const fs::path& path) {
    if (path.empty() || !path.is_absolute()) return;
    const auto normalized = path.lexically_normal();
    for (const auto& existing : paths) {
#ifdef _WIN32
        if (!_wcsicmp(existing.c_str(), normalized.c_str())) return;
#else
        if (existing == normalized) return;
#endif
    }
    paths.push_back(normalized);
}
inline fs::path utf8_path(const std::string& text) {
    try { return fs::u8path(text); } catch (...) { return {}; }
}
inline std::vector<fs::path> discover(const std::vector<fs::path>& roots) {
    std::vector<fs::path> libraries, games;
    for (const auto& root : roots) {
        add(libraries, root);
        for (const auto& file : {root / "steamapps/libraryfolders.vdf", root / "config/libraryfolders.vdf"}) {
            const auto document = read(file);
            const auto* folder = find(document, "libraryfolders");
            if (!folder || !folder->object) continue;
            for (const auto& entry : folder->children) {
                if (entry.key.empty() || !std::all_of(entry.key.begin(), entry.key.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) continue;
                const auto* path = entry.object ? find(entry.children, "path") : &entry;
                if (path && !path->object && path->value.find('\0') == std::string::npos)
                    add(libraries, utf8_path(path->value));
            }
        }
    }
    for (const auto& library : libraries) {
        const auto document = read(library / "steamapps/appmanifest_573090.acf");
        const auto* app = find(document, "AppState");
        if (!app || !app->object) continue;
        const auto* id = find(app->children, "appid");
        const auto* dir = find(app->children, "installdir");
        if (!id || id->object || id->value != "573090" || !dir || dir->object || dir->value.empty() ||
            dir->value == "." || dir->value == ".." || dir->value.find_first_of("/\\:") != std::string::npos ||
            dir->value.find('\0') != std::string::npos) continue;
        const auto directory = utf8_path(dir->value);
        if (directory.empty()) continue;
        const auto candidate = library / "steamapps/common" / directory;
        std::error_code error;
        if (fs::is_regular_file(candidate / "stormworks64.exe", error)) add(games, candidate);
    }
    return games;
}
#ifdef _WIN32
inline std::vector<fs::path> roots() {
    std::vector<fs::path> out;
    auto registry = [&](HKEY hive, const wchar_t* subkey, const wchar_t* name, REGSAM view) {
        HKEY key{};
        if (RegOpenKeyExW(hive, subkey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return;
        wchar_t buffer[32768]{}; DWORD bytes = sizeof(buffer), type = 0;
        const auto result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes >= sizeof(buffer) ||
            (type != REG_SZ && type != REG_EXPAND_SZ)) return;
        if (type == REG_EXPAND_SZ) {
            wchar_t expanded[32768]{};
            const auto count = ExpandEnvironmentStringsW(buffer, expanded, DWORD(std::size(expanded)));
            if (count && count <= std::size(expanded)) add(out, fs::path(expanded));
        } else add(out, fs::path(buffer));
    };
    registry(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0);
    for (auto view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY})
        registry(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath", view);
    for (auto name : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        wchar_t directory[32768]{};
        const auto count = GetEnvironmentVariableW(name, directory, DWORD(std::size(directory)));
        if (count && count < std::size(directory)) add(out, fs::path(directory) / "Steam");
    }
    return out;
}
#endif
}
