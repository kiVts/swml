#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace swml_updates {
inline constexpr char kIndexUrl[] = "https://github.com/kiVts/swml/releases/download/index/index.json";

struct Entry { std::string id, version, url, sha256; uint32_t api = 1; std::string name, summary, multiplayer, homepage; bool obsolete = false; };
struct Index { std::string loader_version, loader_url; std::map<std::string, Entry> mods; std::string loader_download_url, loader_sha256; };

Index parse_index(std::string_view json);
std::string sha256_hex(std::string_view bytes);
std::string https_get(const std::string& url, size_t limit);
void install_package(const std::wstring& root, const Entry& entry, std::string_view zip);

bool newer(const std::string& offered, const std::string& installed);
std::wstring find_mod_root(const std::wstring& dir);

bool fetch_index(Index& index, std::wstring& error);
bool update_mod(const std::wstring& root, const Entry& entry, std::wstring& error);
std::wstring stage_loader_package(const std::wstring& root, const Index& index, std::string_view zip);
bool download_loader_update(const std::wstring& root, const Index& index, std::wstring& folder, std::wstring& error);
bool launch_loader_installer(const std::wstring& root, const std::wstring& folder, std::wstring& error);
}
