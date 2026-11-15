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
#include <set>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

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
        index.loader_download_url = loader->text("download_url");
        index.loader_sha256 = loader->text("sha256");
        if ((!index.loader_download_url.empty() || !index.loader_sha256.empty()) &&
            (!allowed_url(index.loader_download_url) || !sha256_text(index.loader_sha256))) fail("index has an invalid loader download");
    }
    auto* mods = root.find("mods");
    if (!mods || !mods->is_object()) fail("index has no mods object");
    for (const auto& [id, value] : mods->object()) {
        Entry entry; entry.id = id;
        entry.name = value.text("name"); entry.summary = value.text("summary"); entry.multiplayer = value.text("multiplayer");
        if (entry.name.empty()) entry.name = id;
        entry.version = value.text("version"); entry.url = value.text("url"); entry.sha256 = value.text("sha256");
        entry.homepage = value.text("homepage");
        if (entry.homepage.empty() && entry.url.starts_with(kAllowedPrefix)) {
            auto end = entry.url.find('/', kAllowedPrefix.size());
            if (end != std::string::npos) end = entry.url.find('/', end + 1);
            if (end != std::string::npos) entry.homepage = entry.url.substr(0, end);
        }
        if (!entry.homepage.empty() && !allowed_url(entry.homepage)) fail(id + ": invalid GitHub page");
        if (auto* obsolete = value.find("obsolete")) entry.obsolete = obsolete->is_bool() && obsolete->boolean();
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

std::wstring stage_loader_package(const std::wstring& root, const Index& index, std::string_view zip) {
    if (!sha256_text(index.loader_sha256) || sha256_hex(zip) != index.loader_sha256) fail("SWML download checksum mismatch");
    if (!swml_version::parse(index.loader_version)) fail("Invalid SWML version");
    for (fs::path p = fs::absolute(root); !p.empty();) {
        if (swml_platform::is_link(p)) fail("SWML update paths must not be links");
        auto parent = p.parent_path(); if (parent == p) break; p = parent;
    }
    fs::path stage_parent = fs::path(root) / "staging";
    if (swml_platform::is_link(stage_parent)) fail("SWML staging must not be a link");
    auto staging = make_staging(root);
    auto archive = staging.path / "package.zip", extracted = staging.path / "kit";
    swml_platform::write_bytes_atomic(archive, zip); fs::create_directories(extracted);
    std::wstring why;
    if (!zip_extract(archive.wstring(), extracted.wstring(), why)) fail(swml_manifest::utf8(why));
    const std::set<std::string> payload = {"README.md","install.bat","install_linux.sh","swml_startup_setup.exe",
        "startup/swml.exe","startup/swml_core.dll","startup/opengl32.dll","swml/swml.exe","swml/swml_core.dll","swml/swml_proton.sh"};
    auto allowed = payload; allowed.insert("manifest.json"); allowed.insert("startup-manifest.json");
    std::set<std::string> found;
    for (const auto& p : fs::recursive_directory_iterator(extracted)) {
        if (swml_platform::is_link(p.path())) fail("SWML kit contains a link");
        if (p.is_regular_file()) found.insert(p.path().lexically_relative(extracted).generic_string());
    }
    if (found != allowed) fail("SWML kit contains missing or unexpected files");
    auto manifest = swml_json::parse(swml_platform::read_bytes(extracted / "manifest.json", kIndexLimit));
    if (manifest.text("version") != index.loader_version) fail("SWML kit version does not match the index");
    auto* files = manifest.find("files"); if (!files || !files->is_object() || files->object().size() != payload.size()) fail("Invalid SWML kit manifest");
    for (const auto& name : payload) {
        auto expected = files->text(name);
        if (!sha256_text(expected) || sha256_hex(swml_platform::read_bytes(extracted / name, kPackageLimit)) != expected) fail("SWML kit file checksum mismatch: " + name);
    }
    auto startup = swml_json::parse(swml_platform::read_bytes(extracted / "startup-manifest.json", kIndexLimit));
    auto* startup_files = startup.find("files");
    if (startup.text("kind") != "swml-startup-v1" || !startup_files || !startup_files->is_object()) fail("Invalid SWML startup manifest");
    for (const char* name : {"swml.exe","swml_core.dll","opengl32.dll"})
        if (startup_files->text(name) != files->text(std::string("startup/") + name)) fail("SWML startup manifest mismatch");
    auto result = extracted.wstring(); staging.path.clear(); return result;
}

bool download_loader_update(const std::wstring& root, const Index& index, std::wstring& folder, std::wstring& error) {
    return guarded(error, [&] {
#ifndef _WIN32
        fail("Automatic installation requires the Windows SWML launcher.");
#else
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) fail("Could not check running games");
        PROCESSENTRY32W process{sizeof process}; bool running = false;
        for (BOOL ok = Process32FirstW(snapshot, &process); ok; ok = Process32NextW(snapshot, &process))
            if (!_wcsicmp(process.szExeFile,L"stormworks64.exe") || !_wcsicmp(process.szExeFile,L"server64.exe")) running = true;
        CloseHandle(snapshot);
        if (running) fail("Close Stormworks before updating SWML. Your game will not be closed automatically.");
        if (!allowed_url(index.loader_download_url) || !sha256_text(index.loader_sha256)) fail("Update download information is unavailable. Check for updates again.");
        folder = stage_loader_package(root, index, https_get(index.loader_download_url, kPackageLimit));
#endif
    });
}

bool launch_loader_installer(const std::wstring& root, const std::wstring& folder, std::wstring& error) {
    return guarded(error, [&] {
#ifdef _WIN32
        swml_json::Value::Array resume;
        auto args = swml_platform::args();
        for (size_t i = 1; i < args.size(); ++i) resume.emplace_back(swml_platform::utf8(args[i]));
        auto request = fs::path(folder).parent_path() / "resume.json";
        swml_platform::write_bytes_atomic(request, swml_json::dump(resume));
        const auto exe = fs::path(folder) / "swml_startup_setup.exe";
        auto quote = [](const std::wstring& input) {
            std::wstring out=L"\"";size_t slashes=0;
            for(wchar_t c:input){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}
            out.append(slashes*2,L'\\');return out+L'\"';
        };
        std::wstring cmd = quote(exe.wstring()) + L" --update-loader --loader " + quote(root) + L" --parent " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW info{sizeof info}; PROCESS_INFORMATION child{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &info, &child)) fail("Could not start the SWML update installer");
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
#else
        fail("Automatic installation requires the Windows SWML launcher.");
#endif
    });
}
}
