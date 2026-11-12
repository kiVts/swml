// Mod discovery and enable state. Reads the same files the old Win32 launcher did:
// mods\<id>\mod.ini, swml.ini ([mods], [profile] default_disabled), status.ini, README.md.
#include "launcher_model.h"
#include "mod_manifest.h"
#include "swml_v2.h"
#include <algorithm>
#include <filesystem>

namespace swml_launcher {
namespace {
namespace fs = std::filesystem;

std::wstring ini_str(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"")
{
    return swml_platform::ini_read(file, section, key, fallback);
}
}

void Model::init(const std::wstring& root_dir)
{
    root = root_dir;
    mods_dir = (fs::path(root) / "mods").wstring();
    ini = (fs::path(root) / "swml.ini").wstring();
    reload();
}

void Model::reload()
{
    mods.clear();
    std::error_code error;
    for (fs::directory_iterator it(fs::path(mods_dir), error), end; !error && it != end; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        auto id = it->path().filename().wstring();
        if (id.empty() || id[0] == L'.') continue;
        Mod m;
        m.id = id;
        m.dir = it->path().wstring();
        auto manifest = (it->path() / "mod.ini").wstring();
        if (!swml_platform::path_exists(manifest)) continue;
        m.name = ini_str(manifest, L"mod", L"name", m.id.c_str());
        m.version = ini_str(manifest, L"mod", L"version");
        m.author = ini_str(manifest, L"mod", L"author");
        m.summary = ini_str(manifest, L"mod", L"summary");
        m.multiplayer = ini_str(manifest, L"mod", L"multiplayer", L"local");
        m.game_data = ini_str(manifest, L"mod", L"game_data");
        m.dll = ini_str(manifest, L"mod", L"dll");
        int def = swml_platform::ini_read_int(manifest, L"mod", L"default_enabled", 1);
        m.enabled = swml_platform::ini_read_int(ini, L"mods", m.id.c_str(), def) != 0;
        auto status_file = (it->path() / "status.ini").wstring();
        m.status = ini_str(status_file, L"status", L"text", L"not run yet");
        m.status_time = ini_str(status_file, L"status", L"time");
        m.readme = swml_platform::read_text((it->path() / "README.md").wstring());
        mods.push_back(std::move(m));
    }
    std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return swml_platform::compare_icase(a.name, b.name) < 0; });
    std::vector<swml_dependencies::Mod> descriptors;
    for (auto& m : mods) {
        try { descriptors.push_back(swml_manifest::inspect(m.dir, m.id, ini)); }
        catch (const std::exception& e) { swml_dependencies::Mod bad{swml_manifest::utf8(m.id)}; bad.problem = e.what(); descriptors.push_back(std::move(bad)); }
    }
    auto plan = swml_dependencies::plan(descriptors);
    for (auto& m : mods) {
        auto descriptor = std::find_if(descriptors.begin(), descriptors.end(), [&](const auto& d) { return d.id == swml_manifest::utf8(m.id); });
        if (descriptor == descriptors.end()) continue;
        m.enabled = descriptor->enabled;
        auto problem = plan.blocked.find(swml_manifest::utf8(m.id));
        if (problem != plan.blocked.end()) m.problem = swml_manifest::wide(problem->second);
    }
}

void Model::set_enabled(Mod& mod, bool on)
{
    mod.enabled = on;
    std::wstring value = on ? L"1" : L"0";
    swml_platform::ini_write(ini, L"mods", mod.id, &value);
}

const swml_updates::Entry* Model::offered_update(const Mod& mod, const swml_updates::Index& index, bool have_index) const
{
    if (!have_index) return nullptr;
    try {
        auto found = index.mods.find(swml_manifest::utf8(mod.id));
        if (found == index.mods.end() || !swml_updates::newer(found->second.version, swml_manifest::utf8(mod.version))) return nullptr;
        return &found->second;
    } catch (const std::exception&) { return nullptr; }
}

bool Model::can_update(const Mod& mod, const swml_updates::Index& index, bool have_index) const
{
    auto* entry = offered_update(mod, index, have_index);
    return entry && entry->api <= SWML_API_VERSION;
}

std::wstring game_command_line(const std::vector<std::wstring>& args)
{
    std::wstring cmd;
    for (size_t i = 1; i < args.size(); i++) {
        const auto& arg = args[i];
        bool quote = arg.empty() || arg.find_first_of(L" \t\"") != arg.npos;
        if (i > 1) cmd += L" ";
        if (quote) {
            cmd += L"\"";
            size_t backslashes = 0;
            for (wchar_t c : arg) {
                if (c == L'\\') { backslashes++; cmd += c; continue; }
                if (c == L'"') { cmd.append(backslashes + 1, L'\\'); cmd += c; }
                else cmd += c;
                backslashes = 0;
            }
            cmd.append(backslashes, L'\\');
            cmd += L"\"";
        } else cmd += arg;
    }
    return cmd;
}

} // namespace swml_launcher
