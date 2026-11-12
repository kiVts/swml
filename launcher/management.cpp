// Mod installation, rollback and diagnostics. Portable: std::filesystem + swml_platform, shared
// by the Windows launcher and the native Linux launcher. Packages are validated (PE check for
// DLL mods, path rules, size limits) before anything is published; installs are staged and
// renamed into place, with the previous version kept in backups\<id>.
#include "management.h"
#include "platform.h"
#include "mod_manifest.h"
#include "json.h"
#include "settings_schema.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>

namespace swml_manager {
namespace {
namespace fs = std::filesystem;
using swml_json::Value;

std::wstring temporary(const fs::path& directory) {
    static std::atomic<uint64_t> serial{0};
    fs::create_directories(directory);
    for (unsigned i = 0; i < 100; ++i) {
        auto path = directory / (L"swml-stage-" + std::to_wstring(swml_platform::process_id()) + L"-" +
                                 std::to_wstring(swml_platform::tick_ms()) + L"-" + std::to_wstring(serial.fetch_add(1)));
        if (fs::create_directory(path)) return path.wstring();
    }
    throw std::runtime_error("could not allocate staging directory");
}
void remove_owned(const fs::path& parent, const fs::path& path) {
    auto base = fs::absolute(parent).lexically_normal(); auto target = fs::absolute(path).lexically_normal();
    if (target.parent_path() != base || target.filename().empty()) throw std::runtime_error("cleanup target outside owned directory");
    fs::remove_all(target);
}
void rename_directory(const fs::path& source, const fs::path& target) {
    // A just-moved directory can remain briefly locked by a filesystem observer (Windows).
    // Retry only sharing/access failures; never replace an existing target.
    std::error_code error;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        error.clear();
        fs::rename(source, target, error);
        if (!error) return;
#ifdef _WIN32
        int code = error.value();
        if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
#else
        break;
#endif
    }
    throw fs::filesystem_error("could not publish directory", source, target, error);
}
bool wrapped(std::wstring& error, const auto& operation) {
    error.clear();
    try { operation(); return true; }
    catch (const std::exception& e) { try { error = swml_manifest::wide(e.what()); } catch (...) { error = L"Filesystem operation failed"; } return false; }
}
void id(const std::wstring& name) {
    if (!swml_manifest::valid_id(swml_manifest::utf8(name))) throw std::runtime_error("invalid id (use lowercase letters, digits and underscores)");
}
void copy_tree(const fs::path& source, const fs::path& target) {
    if (!fs::is_directory(source) || swml_platform::is_link(source)) throw std::runtime_error("invalid source directory or directory link");
    fs::create_directories(target);
    size_t files = 0; uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        if (swml_platform::is_link(entry.path())) throw std::runtime_error("links/reparse points are not accepted in mod packages");
        auto relative = entry.path().lexically_relative(source);
        if (!swml_manifest::relative_path(relative.wstring())) throw std::runtime_error("invalid package path");
        if (entry.is_directory()) fs::create_directories(target / relative);
        else if (entry.is_regular_file()) {
            auto size = entry.file_size(); total += size;
            if (++files > 4096 || size > 128ull * 1024 * 1024 || total > 1024ull * 1024 * 1024) throw std::runtime_error("package exceeds file/count/size limits");
            fs::copy_file(entry.path(), target / relative, fs::copy_options::overwrite_existing);
        } else throw std::runtime_error("unsupported package entry");
    }
}
void preserve(const fs::path& old, const fs::path& staged) {
    for (auto name : {L"settings.ini", L"settings.overrides.ini", L"status.ini"})
        if (fs::is_regular_file(old / name)) fs::copy_file(old / name, staged / name, fs::copy_options::overwrite_existing);
    if (fs::is_directory(old / "data")) { if (fs::exists(staged / "data")) remove_owned(staged, staged / "data"); copy_tree(old / "data", staged / "data"); }
}
}
bool install(const std::wstring& root, const std::wstring& source, const std::wstring& name, std::wstring& error) {
    return wrapped(error, [&] {
        id(name); fs::path target = fs::path(root) / "mods" / name, backup = fs::path(root) / "backups" / name;
        auto stage = fs::path(temporary(fs::path(root) / "staging")); bool moved = false;
        try {
            auto data = stage / "mod"; copy_tree(source, data);
            auto descriptor = swml_manifest::inspect(data.wstring(), name, (fs::path(root) / "swml.ini").wstring());
            if (!descriptor.problem.empty()) throw std::runtime_error(descriptor.problem);
            auto dll = swml_manifest::read((data / "mod.ini").wstring(), L"mod", L"dll");
            if (!dll.empty() && !fs::is_regular_file(data / dll)) throw std::runtime_error("declared mod DLL is missing");
            if (!dll.empty()) {
                auto image = swml_platform::read_bytes(data / dll, 128ull * 1024 * 1024);
                auto u16 = [&](size_t at) { if (at + 2 > image.size()) throw std::runtime_error("truncated DLL header"); return uint16_t(uint8_t(image[at]) | (uint16_t(uint8_t(image[at + 1])) << 8)); };
                auto u32 = [&](size_t at) { return uint32_t(u16(at)) | (uint32_t(u16(at + 2)) << 16); };
                if (u16(0) != 0x5a4d) throw std::runtime_error("mod DLL is not a PE image");
                size_t pe = u32(0x3c);
                if (pe > image.size() || u32(pe) != 0x4550 || u16(pe + 4) != 0x8664 || !(u16(pe + 22) & 0x2000) || u16(pe + 24) != 0x20b)
                    throw std::runtime_error("mod DLL must be a Windows x64 DLL");
            }
            if (fs::exists(data / "settings.schema.json")) swml_settings::parse(swml_json::parse(swml_platform::read_bytes(data / "settings.schema.json", 65535)));
            // Packages carry defaults, never the developer's local overrides/cache.
            for (auto file : {L"settings.overrides.ini", L".settings.effective.ini"}) if (fs::exists(data / file)) remove_owned(data, data / file);
            if (fs::exists(target)) {
                preserve(target, data); fs::create_directories(backup.parent_path());
                if (fs::exists(backup)) remove_owned(backup.parent_path(), backup);
                rename_directory(target, backup); moved = true;
            }
            fs::create_directories(target.parent_path());
            try { rename_directory(data, target); }
            catch (...) { if (moved) rename_directory(backup, target); throw; }
            remove_owned(stage.parent_path(), stage);
        } catch (...) { remove_owned(stage.parent_path(), stage); throw; }
    });
}
bool rollback(const std::wstring& root, const std::wstring& name, std::wstring& error) {
    return wrapped(error, [&] {
        id(name); fs::path target = fs::path(root) / "mods" / name, backup = fs::path(root) / "backups" / name;
        if (!fs::is_directory(backup)) throw std::runtime_error("no previous installation available");
        auto stage = fs::path(temporary(fs::path(root) / "staging"));
        try {
            if (fs::exists(target)) { preserve(target, backup); rename_directory(target, stage / "mod"); }
            try { rename_directory(backup, target); }
            catch (...) { if (fs::exists(stage / "mod")) rename_directory(stage / "mod", target); throw; }
            if (fs::exists(stage / "mod")) rename_directory(stage / "mod", backup);
            remove_owned(stage.parent_path(), stage);
        } catch (...) {
            // Never destroy the only retained current installation if recovery
            // or moving it into the backup slot failed. Report its exact path.
            if (fs::exists(stage / "mod")) throw std::runtime_error("rollback interrupted; retained installation at " + swml_manifest::utf8((stage / "mod").wstring()));
            remove_owned(stage.parent_path(), stage); throw;
        }
    });
}
bool doctor(const std::wstring& root, const std::wstring& path, std::wstring& error) {
    return wrapped(error, [&] {
        Value::Array entries; std::vector<swml_dependencies::Mod> input; auto config = (fs::path(root) / "swml.ini").wstring();
        if (fs::is_directory(fs::path(root) / "mods")) for (const auto& entry : fs::directory_iterator(fs::path(root) / "mods"))
            if (entry.is_directory() && fs::is_regular_file(entry.path() / "mod.ini")) input.push_back(swml_manifest::inspect(entry.path().wstring(), entry.path().filename().wstring(), config));
        auto plan = swml_dependencies::plan(input);
        for (const auto& mod : input) entries.emplace_back(Value::Object{{"id", mod.id}, {"version", mod.version}, {"enabled", mod.enabled}, {"problem", plan.blocked.contains(mod.id) ? plan.blocked[mod.id] : ""}});
        auto output = Value::Object{{"sdk_api", int(SWML_API_VERSION)}, {"platform", swml_platform::game_platform()}, {"root", swml_manifest::utf8(root)}, {"config", swml_manifest::utf8(config)},
            {"core_present", fs::is_regular_file(fs::path(root) / "swml_core.dll")}, {"mods", std::move(entries)}, {"native_bindings", "not probed: game is not running"}};
        swml_platform::write_bytes_atomic(fs::path(path), swml_json::dump(output));
    });
}
} // namespace swml_manager
