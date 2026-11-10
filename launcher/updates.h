#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace swml_updates {
inline constexpr char kIndexUrl[] = "https://github.com/kiVts/swml/releases/download/index/index.json";

struct Entry { std::string id, version, url, sha256; uint32_t api = 1; };
struct Index { std::string loader_version, loader_url; std::map<std::string, Entry> mods; };

Index parse_index(std::string_view json);
std::string sha256_hex(std::string_view bytes);
std::string https_get(const std::string& url, size_t limit);
void install_package(const std::wstring& root, const Entry& entry, std::string_view zip);

bool newer(const std::string& offered, const std::string& installed);
std::wstring find_mod_root(const std::wstring& dir);

bool fetch_index(Index& index, std::wstring& error);
bool update_mod(const std::wstring& root, const Entry& entry, std::wstring& error);
}
