// Mod index + update downloads. Portable: HTTPS/SHA-256/staging go through swml_platform;
// the index format, URL allowlist (https://github.com/ only), SHA-256 verification and size
// limits are unchanged from the Win32 launcher.
#include "updates.h"
#include "management.h"
#include "platform.h"
#include "mod_manifest.h"
#include "json.h"
#include "zip.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace swml_updates {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view kAllowedPrefix = "https://github.com/";
constexpr size_t kIndexLimit = 1024 * 1024, kPackageLimit = 128ull * 1024 * 1024;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

bool allowed_url(std::string_view url) {
    if (!url.starts_with(kAllowedPrefix) || url.size() == kAllowedPrefix.size() || url.size() > 512) return false;
    for (unsigned char c : url) if (c <= 32 || c >= 127 || c == '\\') return false;
    return true;
}
bool sha256_text(std::string_view text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == text.npos;
}

struct Staging {
    fs::path path;
    ~Staging() { std::error_code ignored; fs::remove_all(path, ignored); }
};
Staging make_staging(const std::wstring& root) {
    fs::path path = fs::path(root) / "staging" / (std::wstring(L"update-") + swml_platform::make_unique_id());
    fs::create_directories(path);
    return Staging{path};
}
template <class Operation> bool guarded(std::wstring& error, const Operation& operation) {
    error.clear();
    try { operation(); return true; }
    catch (const std::exception& e) { try { error = swml_manifest::wide(e.what()); } catch (...) { error = L"Update failed"; } }
    catch (...) { error = L"Update failed"; }
    return false;
}
}

Index parse_index(std::string_view json) {
    auto root = swml_json::parse(json);
    if (!root.is_object()) fail("index is not an object");
    auto* schema = root.find("schema");
    if (!schema || !schema->is_number()) fail("index has no schema number");
    if (schema->number() != 1) fail("index needs a newer SWML");
    Index index;
    if (auto* loader = root.find("loader")) {
        index.loader_version = loader->text("version"); index.loader_url = loader->text("url");
        if (!swml_version::parse(index.loader_version) || !allowed_url(index.loader_url)) fail("index has an invalid loader entry");
    }
    auto* mods = root.find("mods");
    if (!mods || !mods->is_object()) fail("index has no mods object");
    for (const auto& [id, value] : mods->object()) {
        Entry entry; entry.id = id;
        entry.version = value.text("version"); entry.url = value.text("url"); entry.sha256 = value.text("sha256");
        if (!swml_manifest::valid_id(id)) fail("index has an invalid mod id: " + id);
        if (!swml_version::parse(entry.version)) fail(id + ": invalid version in index");
        if (!allowed_url(entry.url)) fail(id + ": index URL must be on https://github.com/");
        if (!sha256_text(entry.sha256)) fail(id + ": invalid SHA-256 in index");
        if (auto* api = value.find("api")) {
            if (!api->is_number() || api->number() < 1 || api->number() > 65535 || api->number() != double(uint32_t(api->number()))) fail(id + ": invalid api in index");
            entry.api = uint32_t(api->number());
        }
        index.mods.emplace(id, std::move(entry));
    }
    return index;
}

bool newer(const std::string& offered, const std::string& installed) {
    auto next = swml_version::parse(offered), current = swml_version::parse(installed);
    if (!next) return false;
    return !current || swml_version::compare(*next, *current) > 0;
}

std::string sha256_hex(std::string_view bytes) { return swml_platform::sha256_hex(bytes); }

std::string https_get(const std::string& url, size_t limit) {
    if (!allowed_url(url)) fail("refused download from outside https://github.com/");
    return swml_platform::https_get(url, limit);
}

std::wstring find_mod_root(const std::wstring& dir) {
    fs::path base(dir);
    std::error_code error;
    if (fs::is_regular_file(base / "mod.ini", error)) return base.wstring();
    for (fs::directory_iterator it(base, error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error) && it->path().filename().wstring()[0] != L'.' && fs::is_regular_file(it->path() / "mod.ini", error))
            return it->path().wstring();
    return {};
}

void install_package(const std::wstring& root, const Entry& entry, std::string_view zip) {
    if (sha256_hex(zip) != entry.sha256) fail("download does not match the SHA-256 in the mod index");
    auto staging = make_staging(root);
    auto archive = staging.path / L"package.zip", extracted = staging.path / L"extracted";
    {
        std::ofstream file(archive, std::ios::binary);
        file.write(zip.data(), std::streamsize(zip.size()));
        if (!file.flush()) fail("could not write the downloaded package");
    }
    fs::create_directories(extracted);
    std::wstring why;
    if (!zip_extract(archive.wstring(), extracted.wstring(), why)) fail("could not extract the package: " + swml_manifest::utf8(why));
    auto folder = find_mod_root(extracted.wstring());
    if (folder.empty()) fail("package has no mod.ini");
    auto manifest = (fs::path(folder) / "mod.ini").wstring();
    if (swml_manifest::utf8(swml_manifest::read(manifest, L"mod", L"id")) != entry.id) fail("package is for a different mod than the index says");
    if (swml_manifest::utf8(swml_manifest::read(manifest, L"mod", L"version")) != entry.version) fail("package version differs from the index");
    if (!swml_manager::install(root, folder, swml_manifest::wide(entry.id), why)) fail(swml_manifest::utf8(why));
}

bool fetch_index(Index& index, std::wstring& error) {
    return guarded(error, [&] { index = parse_index(https_get(kIndexUrl, kIndexLimit)); });
}

bool update_mod(const std::wstring& root, const Entry& entry, std::wstring& error) {
    return guarded(error, [&] { install_package(root, entry, https_get(entry.url, kPackageLimit)); });
}
}
