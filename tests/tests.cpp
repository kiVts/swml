// Portable unit tests for the launcher logic + platform layer. No UI, no network.
// Run: ctest --test-dir build  (or ./build/swml_tests)
#include "platform.h"
#include "launcher_model.h"
#include "management.h"
#include "updates.h"
#include "zip.h"
#include "settings_store.h"
#include "settings_schema.h"
#include "mod_manifest.h"
#include "json.h"

#include <libdeflate.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace swml_platform;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static fs::path scratch()
{
    static fs::path dir;
    if (dir.empty()) {
        dir = fs::path(temp_dir()) / ("swml-tests-" + utf8(make_unique_id()));
        fs::create_directories(dir);
    }
    return dir;
}

static void put(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}

static std::string get(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static void test_utf()
{
    CHECK(utf8(wide("hello")) == "hello");
    CHECK(utf8(wide("st\xf0\x9f\x98\x80 rm")) == "st\xf0\x9f\x98\x80 rm"); // emoji (non-BMP on Windows)
    CHECK(wide("Gr\xc3\xbc\xc3\x9f" "e") == L"Gr\u00fc\u00dfe");
    bool threw = false;
    try { wide(std::string("\xff\xfe", 2)); } catch (const std::exception&) { threw = true; }
    CHECK(threw); // strict decode rejects invalid UTF-8
    CHECK(compare_icase(L"ModName", L"modname") == 0);
    CHECK(lower(L"ABCdef") == L"abcdef");
}

static void test_ini()
{
    auto file = scratch() / "profile.ini";
    // Plain UTF-8 with comments, quotes, mixed case (the install_linux.sh format).
    put(file, "; comment\r\n[Profile]\r\ndefault_disabled=1\r\n[MODS]\r\nfoo=1\r\nbar = \" spaced \"\r\n");
    CHECK(ini_read(file.wstring(), L"mods", L"FOO") == L"1");
    CHECK(ini_read(file.wstring(), L"profile", L"default_disabled") == L"1");
    CHECK(ini_read(file.wstring(), L"mods", L"bar") == L" spaced ");
    CHECK(ini_read(file.wstring(), L"mods", L"missing", L"fallback") == L"fallback");
    CHECK(ini_read_int(file.wstring(), L"mods", L"foo", 0) == 1);
    CHECK(ini_read_int(file.wstring(), L"mods", L"nope", 7) == 7);
    std::wstring value = L"0";
    CHECK(ini_write(file.wstring(), L"mods", L"foo", &value));
    CHECK(ini_read(file.wstring(), L"mods", L"foo") == L"0");
    CHECK(get(file).find("; comment") != std::string::npos); // comments preserved
    value = L"2";
    CHECK(ini_write(file.wstring(), L"mods", L"newkey", &value));
    CHECK(ini_read(file.wstring(), L"mods", L"newkey") == L"2");
    CHECK(ini_write(file.wstring(), L"mods", L"foo", nullptr));
    CHECK(ini_read(file.wstring(), L"mods", L"foo", L"gone") == L"gone");

    // UTF-16LE with BOM (the settings store format).
    auto utf16 = scratch() / "utf16.ini";
    std::string bytes = "\xff\xfe";
    std::wstring text = L"[mods]\r\nalpha=1\r\n";
    for (wchar_t c : text) { bytes += char(uint16_t(c) & 0xFF); bytes += char(uint16_t(c) >> 8); }
    put(utf16, bytes);
    CHECK(ini_read(utf16.wstring(), L"mods", L"alpha") == L"1");
    value = L"0";
    CHECK(ini_write(utf16.wstring(), L"mods", L"alpha", &value));
    auto raw = get(utf16);
    CHECK(raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE); // encoding kept
    CHECK(ini_read(utf16.wstring(), L"mods", L"alpha") == L"0");

    // Sections enumeration + whole-file rewrite round-trip.
    auto sections = ini_sections(file.wstring());
    CHECK(sections.size() >= 2);
    std::vector<IniKeyValue> items{{L"general", L"count", L"3"}, {L"general", L"name", L"test"}, {L"other", L"x", L"y"}};
    auto whole = scratch() / "whole.ini";
    ini_write_all(whole.wstring(), L"; header\r\n", items);
    CHECK(ini_read(whole.wstring(), L"general", L"count") == L"3");
    CHECK(ini_read(whole.wstring(), L"other", L"x") == L"y");
    raw = get(whole);
    CHECK(raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE);
}

static void test_settings_store()
{
    auto dir = scratch() / "settingsmod";
    put(dir / "settings.default.ini", "[graphics]\r\n; 1 = on, 0 = off\r\nenabled=1\r\nscale=2\r\n");
    swml_settings_store::prepare(dir.wstring());
    CHECK(fs::exists(dir / "settings.overrides.ini"));
    CHECK(fs::exists(dir / ".settings.effective.ini"));
    swml_settings_store::set(dir.wstring(), L"graphics", L"scale", L"3");
    auto merged = swml_settings_store::merged(dir.wstring());
    CHECK((merged[{L"graphics", L"scale"}] == L"3"));
    CHECK((merged[{L"graphics", L"enabled"}] == L"1"));
    // Setting back to the default removes the override.
    swml_settings_store::set(dir.wstring(), L"graphics", L"scale", L"2");
    merged = swml_settings_store::merged(dir.wstring());
    CHECK((merged[{L"graphics", L"scale"}] == L"2"));
    auto overrides = swml_settings_store::read(swml_settings_store::overrides(dir.wstring()));
    CHECK((!overrides.contains({L"graphics", L"scale"})));
    // The effective file must be readable by the Windows profile APIs: UTF-16LE + BOM.
    auto raw = get(dir / ".settings.effective.ini");
    CHECK(raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE);
}

static void test_manifest()
{
    auto dir = scratch() / "manifestmod";
    put(dir / "mod.ini", "[mod]\r\nid=manifestmod\r\nname=Manifest Mod\r\nversion=1.2.3\r\ndepends=other_mod\r\n");
    auto config = scratch() / "swml.ini";
    put(config, "[mods]\r\nmanifestmod=1\r\n");
    auto descriptor = swml_manifest::inspect(dir.wstring(), L"manifestmod", config.wstring());
    CHECK(descriptor.problem.empty());
    CHECK(descriptor.enabled);
    CHECK(descriptor.version == "1.2.3");
    CHECK(descriptor.required.size() == 1 && descriptor.required[0] == "other_mod");
    CHECK(swml_manifest::valid_id("good_id2"));
    CHECK(!swml_manifest::valid_id("Bad-ID"));
    CHECK(swml_manifest::relative_path(L"data\\stuff"));
    CHECK(!swml_manifest::relative_path(L"..\\escape"));
    CHECK(!swml_manifest::relative_path(L"C:\\absolute"));
}

// Minimal stored-entry zip writer for the extractor test.
static void put_u16(std::string& out, uint16_t v) { out += char(v & 0xFF); out += char(v >> 8); }
static void put_u32(std::string& out, uint32_t v) { for (int i = 0; i < 4; i++) out += char((v >> (8 * i)) & 0xFF); }

static std::string make_zip(const std::vector<std::pair<std::string, std::string>>& entries)
{
    std::string local, central;
    for (const auto& [name, data] : entries) {
        uint32_t crc = libdeflate_crc32(0, data.data(), data.size());
        uint32_t offset = uint32_t(local.size());
        put_u32(local, 0x04034b50); put_u16(local, 20); put_u16(local, 0x800); put_u16(local, 0);
        put_u16(local, 0); put_u16(local, 0); put_u32(local, crc); put_u32(local, uint32_t(data.size()));
        put_u32(local, uint32_t(data.size())); put_u16(local, uint16_t(name.size())); put_u16(local, 0);
        local += name; local += data;
        put_u32(central, 0x02014b50); put_u16(central, 20); put_u16(central, 20); put_u16(central, 0x800);
        put_u16(central, 0); put_u16(central, 0); put_u16(central, 0); put_u32(central, crc);
        put_u32(central, uint32_t(data.size())); put_u32(central, uint32_t(data.size()));
        put_u16(central, uint16_t(name.size())); put_u16(central, 0); put_u16(central, 0); put_u16(central, 0);
        put_u16(central, 0); put_u32(central, 0); put_u32(central, offset); central += name;
    }
    std::string out = local + central;
    put_u32(out, 0x06054b50); put_u16(out, 0); put_u16(out, 0);
    put_u16(out, uint16_t(entries.size())); put_u16(out, uint16_t(entries.size()));
    put_u32(out, uint32_t(central.size())); put_u32(out, uint32_t(local.size())); put_u16(out, 0);
    return out;
}

static void test_zip()
{
    auto archive = scratch() / "mod.zip";
    put(archive, make_zip({{"mod.ini", "[mod]\r\nid=zipmod\r\n"}, {"data/file.txt", "hello"}}));
    auto dest = scratch() / "zipout";
    std::wstring error;
    CHECK(zip_extract(archive.wstring(), dest.wstring(), error));
    CHECK(get(dest / "mod.ini").find("zipmod") != std::string::npos);
    CHECK(get(dest / "data" / "file.txt") == "hello");
    // Path traversal must be refused.
    auto evil = scratch() / "evil.zip";
    put(evil, make_zip({{"../escape.txt", "x"}}));
    CHECK(!zip_extract(evil.wstring(), (scratch() / "evilout").wstring(), error));
}

static void test_management()
{
    auto root = scratch() / "root";
    auto source = scratch() / "newmod";
    put(source / "mod.ini", "[mod]\r\nid=newmod\r\nname=New Mod\r\nversion=1.0.0\r\n");
    put(source / "README.md", "hello");
    std::wstring error;
    CHECK(swml_manager::install(root.wstring(), source.wstring(), L"newmod", error));
    CHECK(fs::exists(root / "mods" / "newmod" / "mod.ini"));
    // Update: settings preserved, backup kept.
    put(root / "mods" / "newmod" / "settings.ini", "[user]\r\nx=1\r\n");
    put(source / "mod.ini", "[mod]\r\nid=newmod\r\nname=New Mod\r\nversion=1.1.0\r\n");
    CHECK(swml_manager::install(root.wstring(), source.wstring(), L"newmod", error));
    CHECK(get(root / "mods" / "newmod" / "settings.ini").find("x=1") != std::string::npos);
    CHECK(fs::exists(root / "backups" / "newmod" / "mod.ini"));
    CHECK(swml_manager::rollback(root.wstring(), L"newmod", error));
    CHECK(get(root / "mods" / "newmod" / "mod.ini").find("1.0.0") != std::string::npos);
    // Doctor writes parseable JSON.
    auto doctor = scratch() / "doctor.json";
    CHECK(swml_manager::doctor(root.wstring(), doctor.wstring(), error));
    auto json = swml_json::parse(get(doctor));
    CHECK(json.is_object());
    CHECK(json.find("mods") && json.find("mods")->is_array());
}

static void test_updates()
{
    CHECK(swml_updates::newer("1.2.0", "1.1.9"));
    CHECK(!swml_updates::newer("1.1.9", "1.2.0"));
    CHECK(!swml_updates::newer("same", "same"));
    auto index = swml_updates::parse_index(R"({"schema":1,"loader":{"version":"9.9.9","url":"https://github.com/x/y/releases"},"mods":{"mymod":{"version":"2.0.0","url":"https://github.com/x/y/releases/download/v2/mymod.zip","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","api":4}}})");
    CHECK(index.loader_version == "9.9.9");
    CHECK(index.mods.at("mymod").version == "2.0.0");
    CHECK(index.mods.at("mymod").api == 4);
    bool threw = false;
    try { swml_updates::parse_index(R"({"schema":1,"mods":{"bad":{"version":"1.0.0","url":"https://evil.example/x.zip","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}}})"); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw); // URLs outside github.com refused
    CHECK(swml_updates::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void test_model()
{
    auto root = scratch() / "modelroot";
    put(root / "mods" / "alpha" / "mod.ini", "[mod]\r\nid=alpha\r\nname=Alpha\r\nversion=1.0.0\r\n");
    put(root / "mods" / "beta" / "mod.ini", "[mod]\r\nid=beta\r\nname=Beta\r\nversion=1.0.0\r\ndefault_enabled=0\r\n");
    put(root / "mods" / "mod_status" / "mod.ini", "[mod]\r\nid=mod_status\r\nname=Legacy Mod Status\r\ndll=mod_status.dll\r\n");
    put(root / "swml.ini", "[mods]\r\nalpha=0\r\n");
    swml_launcher::Model model;
    model.init(root.wstring());
    CHECK(model.mods.size() == 2);
    CHECK(model.mods[0].name == L"Alpha"); // sorted by name
    CHECK(!model.mods[0].enabled);        // swml.ini override
    CHECK(!model.mods[1].enabled);        // default_enabled=0
    model.set_enabled(model.mods[1], true);
    model.reload();
    CHECK(model.mods[1].enabled);
    // Command-line reconstruction keeps quoted arguments intact.
    std::vector<std::wstring> args{L"swml.exe", L"C:\\Games\\Stormworks\\stormworks64.exe", L"-arg with space"};
    auto cmd = swml_launcher::game_command_line(args);
    CHECK(cmd.find(L"C:\\Games\\Stormworks\\stormworks64.exe") != std::wstring::npos);
    CHECK(cmd.find(L"\"-arg with space\"") != std::wstring::npos);
}

int main(int argc, char** argv)
{
    init(argc, argv);
    test_utf();
    test_ini();
    test_settings_store();
    test_manifest();
    test_zip();
    test_management();
    test_updates();
    test_model();
    std::error_code ignored;
    fs::remove_all(scratch(), ignored);
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all tests passed\n");
    return 0;
}
