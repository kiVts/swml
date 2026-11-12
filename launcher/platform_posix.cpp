// POSIX core subset of the platform layer (native Linux launcher): text, INI profiles, atomic
// files, locks. Reads and writes exactly the formats the Windows loader and the Proton-hosted
// core use: UTF-16LE/BE (BOM), UTF-8 (BOM) or plain UTF-8 INI files with GetPrivateProfile-style
// parsing. The desktop subset lives in platform_desktop_posix.cpp.
#include "platform.h"
#include "platform_utf.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <stdexcept>

static_assert(sizeof(wchar_t) == 4, "POSIX build expects 32-bit wchar_t");

namespace swml_platform {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::vector<std::wstring> g_args;
std::wstring g_self;

// --- UTF helpers -----------------------------------------------------------------------------------
using detail::decode_utf8;
using detail::encode_utf8;

wchar_t lower_char(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return c + (L'a' - L'A');
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20; // Latin-1 supplement
    return c;
}

// --- INI machinery ---------------------------------------------------------------------------------
struct IniFile {
    std::vector<std::wstring> lines; // decoded, without line endings
    int encoding = 0;                // 0 = UTF-8 (no BOM), 1 = UTF-8 BOM, 2 = UTF-16LE, 3 = UTF-16BE
    bool crlf = true;
};

std::wstring trim(const std::wstring& s)
{
    auto begin = s.find_first_not_of(L" \t\r\n");
    if (begin == s.npos) return {};
    return s.substr(begin, s.find_last_not_of(L" \t\r\n") - begin + 1);
}

IniFile load_ini(const std::wstring& file)
{
    IniFile ini;
    std::ifstream stream(fs::path(file), std::ios::binary);
    if (!stream) return ini;
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        ini.encoding = 2;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            text += wchar_t(static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8));
    } else if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        ini.encoding = 3;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            text += wchar_t((static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]));
    } else if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
               static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        ini.encoding = 1;
        text = decode_utf8(std::string_view(bytes).substr(3), false);
    } else {
        ini.encoding = 0;
        text = decode_utf8(bytes, false);
    }
    ini.crlf = text.find(L"\r\n") != text.npos;
    size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(L'\n', start);
        auto line = text.substr(start, end == text.npos ? text.npos : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        ini.lines.push_back(line);
        if (end == text.npos) break;
        start = end + 1;
    }
    if (!ini.lines.empty() && ini.lines.back().empty() && text.size() && text.back() == L'\n') ini.lines.pop_back();
    return ini;
}

void save_ini(const std::wstring& file, const IniFile& ini)
{
    std::wstring text;
    const wchar_t* eol = ini.crlf ? L"\r\n" : L"\n";
    for (size_t i = 0; i < ini.lines.size(); i++) {
        text += ini.lines[i];
        text += eol;
    }
    std::string bytes;
    if (ini.encoding == 2 || ini.encoding == 3) {
        bytes = ini.encoding == 2 ? std::string("\xff\xfe", 2) : std::string("\xfe\xff", 2);
        for (wchar_t c : text) {
            // Settings values are BMP text; encode anything else as a replacement character
            // rather than corrupting the file.
            uint16_t u = (uint32_t(c) > 0xFFFF) ? uint16_t(0xFFFD) : uint16_t(c);
            if (ini.encoding == 2) { bytes += char(u & 0xFF); bytes += char(u >> 8); }
            else { bytes += char(u >> 8); bytes += char(u & 0xFF); }
        }
    } else {
        if (ini.encoding == 1) bytes = std::string("\xef\xbb\xbf", 3);
        bytes += encode_utf8(text);
    }
    write_bytes_atomic(fs::path(file), bytes);
}

// Profile value semantics, matching the Win32 API (verified against Wine): surrounding
// matching quotes are stripped and the inside kept verbatim; unquoted values are trimmed on
// both sides.
std::wstring unquote(const std::wstring& raw)
{
    auto value = trim(raw);
    if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front())
        return value.substr(1, value.size() - 2);
    return value;
}

bool is_section(const std::wstring& line, std::wstring& name)
{
    auto t = trim(line);
    if (t.size() < 2 || t.front() != L'[' || t.back() != L']') return false;
    name = trim(t.substr(1, t.size() - 2));
    return true;
}

bool is_comment(const std::wstring& line)
{
    auto t = trim(line);
    return t.empty() || t.front() == L';' || t.front() == L'#';
}

// Split "key=value"; false for comments/sections/malformed lines.
bool split_entry(const std::wstring& line, std::wstring& key, std::wstring& value)
{
    if (is_comment(line)) return false;
    auto equal = line.find(L'=');
    if (equal == line.npos) return false;
    key = trim(line.substr(0, equal));
    if (key.empty() || key.front() == L'[') return false;
    value = unquote(line.substr(equal + 1));
    return true;
}

bool write_profile(IniFile& ini, const std::wstring& section, const std::wstring& key, const std::wstring* value)
{
    // Locate the section and (first matching) key.
    size_t section_end = ini.lines.size(), key_line = ini.lines.size();
    bool found_section = false;
    for (size_t i = 0; i < ini.lines.size(); i++) {
        std::wstring name;
        if (is_section(ini.lines[i], name)) {
            if (found_section) { section_end = i; break; }
            if (compare_icase(name, section) == 0) { found_section = true; section_end = ini.lines.size(); }
            continue;
        }
        if (!found_section) continue;
        std::wstring k, v;
        if (key_line == ini.lines.size() && split_entry(ini.lines[i], k, v) && compare_icase(k, key) == 0) key_line = i;
    }
    if (!found_section) {
        if (!value) return true; // deleting a nonexistent key/section: nothing to do
        if (!ini.lines.empty() && !trim(ini.lines.back()).empty()) ini.lines.push_back(L"");
        ini.lines.push_back(L"[" + section + L"]");
        ini.lines.push_back(key + L"=" + *value);
        return true;
    }
    if (!value) {
        if (key_line < ini.lines.size()) ini.lines.erase(ini.lines.begin() + long(key_line));
        return true;
    }
    // Write values verbatim, like the Win32 profile writer (no quoting).
    const std::wstring& out = *value;
    if (key_line < ini.lines.size()) {
        auto equal = ini.lines[key_line].find(L'=');
        ini.lines[key_line] = ini.lines[key_line].substr(0, equal + 1) + out;
    } else {
        ini.lines.insert(ini.lines.begin() + long(section_end), key + L"=" + out);
    }
    return true;
}
} // namespace

void init(int argc, char** argv)
{
    for (int i = 0; i < argc; i++) g_args.push_back(decode_utf8(argv[i], false));
    if (!g_args.empty()) g_self = g_args[0];
}

std::vector<std::wstring> args() { return g_args; }

std::wstring self_path()
{
    char buffer[4096];
    ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n > 0) { buffer[n] = 0; return decode_utf8(buffer, false); }
    return g_self.empty() ? std::wstring{} : g_self;
}

uint64_t tick_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000 + uint64_t(now.tv_nsec) / 1000000;
}

uint32_t process_id() { return uint32_t(::getpid()); }

std::wstring wide(const std::string& text) { return decode_utf8(text, true); }
std::string utf8(const std::wstring& text) { return encode_utf8(text); }

std::wstring lower(std::wstring text)
{
    for (auto& c : text) c = lower_char(c);
    return text;
}

int compare_icase(const std::wstring& a, const std::wstring& b)
{
    auto la = lower(a), lb = lower(b);
    return la < lb ? -1 : (la == lb ? 0 : 1);
}

bool is_wine() { return false; }
const char* game_platform() { return "proton"; } // the native Linux launcher always targets Proton

std::wstring ini_read(const std::wstring& file, const std::wstring& section, const std::wstring& key,
                      const std::wstring& fallback)
{
    auto ini = load_ini(file);
    bool in_section = section.empty();
    for (const auto& line : ini.lines) {
        std::wstring name;
        if (is_section(line, name)) { in_section = compare_icase(name, section) == 0; continue; }
        if (!in_section) continue;
        std::wstring k, v;
        if (split_entry(line, k, v) && compare_icase(k, key) == 0) return v;
    }
    return fallback;
}

int ini_read_int(const std::wstring& file, const std::wstring& section, const std::wstring& key, int fallback)
{
    auto text = trim(ini_read(file, section, key, L""));
    if (text.empty()) return fallback;
    size_t i = 0;
    bool negative = false;
    if (text[i] == L'+' || text[i] == L'-') { negative = text[i] == L'-'; i++; }
    int base = 10;
    if (text.size() > i + 1 && text[i] == L'0' && (text[i + 1] == L'x' || text[i + 1] == L'X')) { base = 16; i += 2; }
    long long value = 0;
    bool any = false;
    for (; i < text.size(); i++) {
        wchar_t c = text[i];
        int digit = -1;
        if (c >= L'0' && c <= L'9') digit = c - L'0';
        else if (base == 16 && c >= L'a' && c <= L'f') digit = c - L'a' + 10;
        else if (base == 16 && c >= L'A' && c <= L'F') digit = c - L'A' + 10;
        if (digit < 0 || digit >= base) break;
        value = value * base + digit;
        if (value > 0x7FFFFFFFll) value = 0x7FFFFFFFll;
        any = true;
    }
    if (!any) return fallback;
    if (negative) value = -value;
    if (value > 0x7FFFFFFFll) value = 0x7FFFFFFFll;
    if (value < -0x80000000ll) value = -0x80000000ll;
    return int(value);
}

bool ini_write(const std::wstring& file, const std::wstring& section, const std::wstring& key, const std::wstring* value)
{
    try {
        if (!value && !path_exists(file)) return true; // deleting from a missing file: nothing to do
        auto ini = load_ini(file);
        if (!write_profile(ini, section, key, value)) return false;
        save_ini(file, ini);
        return true;
    } catch (const std::exception&) { return false; }
}

std::vector<IniSection> ini_sections(const std::wstring& file)
{
    std::vector<IniSection> out;
    auto ini = load_ini(file);
    IniSection* current = nullptr;
    for (const auto& line : ini.lines) {
        std::wstring name;
        if (is_section(line, name)) {
            auto found = std::find_if(out.begin(), out.end(), [&](const IniSection& s) { return compare_icase(s.name, name) == 0; });
            if (found == out.end()) { out.push_back({name, {}}); current = &out.back(); }
            else current = &*found;
            continue;
        }
        if (!current) continue;
        std::wstring k, v;
        if (!split_entry(line, k, v)) continue;
        auto duplicate = std::any_of(current->entries.begin(), current->entries.end(),
                                     [&](const IniEntry& e) { return compare_icase(e.key, k) == 0; });
        if (!duplicate) current->entries.push_back({k, v});
    }
    return out;
}

void ini_flush(const std::wstring&) {}

void ini_write_all(const std::wstring& file, const std::wstring& header, const std::vector<IniKeyValue>& values)
{
    IniFile ini;
    ini.encoding = 2; // UTF-16LE + BOM, the settings store's on-disk format
    ini.crlf = true;
    size_t start = 0;
    while (start < header.size()) {
        auto end = header.find(L'\n', start);
        auto line = header.substr(start, end == header.npos ? header.npos : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        ini.lines.push_back(line);
        if (end == header.npos) break;
        start = end + 1;
    }
    std::wstring section;
    for (const auto& item : values) {
        if (item.value.find_first_of(L"\r\n") != item.value.npos || item.value.find(L'\0') != item.value.npos)
            fail("invalid multiline setting");
        if (item.section != section) {
            section = item.section;
            ini.lines.push_back(L"");
            ini.lines.push_back(L"[" + section + L"]");
        }
        ini.lines.push_back(item.key + L"=" + item.value);
    }
    save_ini(file, ini);
}

bool path_exists(const std::wstring& path)
{
    std::error_code error;
    return fs::exists(fs::path(path), error);
}

bool is_link(const fs::path& path)
{
    std::error_code error;
    return fs::is_symlink(fs::symlink_status(path, error));
}

std::wstring make_unique_id()
{
    std::random_device device;
    std::uniform_int_distribution<uint32_t> byte(0, 255);
    uint8_t raw[16];
    for (auto& b : raw) b = uint8_t(byte(device));
    raw[6] = uint8_t((raw[6] & 0x0F) | 0x40);
    raw[8] = uint8_t((raw[8] & 0x3F) | 0x80);
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "{";
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += hex[raw[i] >> 4];
        out += hex[raw[i] & 15];
    }
    out += '}';
    return wide(out);
}

void write_bytes_atomic(const fs::path& path, std::string_view data)
{
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(process_id()) + L"." + make_unique_id();
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) fail("could not create " + utf8(temporary.wstring()));
    size_t at = 0;
    bool ok = true;
    while (at < data.size()) {
        ssize_t put = ::write(fd, data.data() + at, data.size() - at);
        if (put < 0) { if (errno == EINTR) continue; ok = false; break; }
        at += size_t(put);
    }
    if (ok) ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        fail("could not commit " + utf8(path.wstring()));
    }
}

struct DirLock::Impl {
    int fd = -1;
};

DirLock::DirLock(const std::wstring& dir) : impl_(new Impl)
{
    auto path = fs::path(dir) / ".settings.lock";
    std::error_code error;
    fs::create_directories(fs::path(dir), error);
    impl_->fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (impl_->fd < 0) { delete impl_; impl_ = nullptr; fail("settings lock unavailable"); }
    auto deadline = tick_ms() + 5000;
    for (;;) {
        if (::flock(impl_->fd, LOCK_EX | LOCK_NB) == 0) break;
        if (tick_ms() >= deadline) {
            ::close(impl_->fd);
            delete impl_; impl_ = nullptr;
            fail("settings are busy");
        }
        timespec pause{0, 50 * 1000 * 1000};
        ::nanosleep(&pause, nullptr);
    }
}

DirLock::~DirLock()
{
    if (impl_) {
        if (impl_->fd >= 0) { ::flock(impl_->fd, LOCK_UN); ::close(impl_->fd); }
        delete impl_;
    }
}

bool DirLock::locked() const { return impl_ != nullptr; }

} // namespace swml_platform
