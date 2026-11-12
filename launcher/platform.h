#pragma once
// Small OS abstraction shared by the launcher logic and the UI. The Windows implementation keeps
// the exact Win32 behaviour the loader has always had (profile APIs, WinHTTP, BCrypt, shell); the
// POSIX implementation reads/writes the same on-disk formats so a native Linux launcher and the
// Proton-hosted core agree on every file.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swml_platform {

// --- text ----------------------------------------------------------------------------------------
// UTF-8 <-> wide (UTF-16 on Windows, UTF-32 elsewhere). Throw std::runtime_error on invalid input.
std::wstring wide(const std::string& utf8);
std::string utf8(const std::wstring& text);
std::wstring lower(std::wstring text);
int compare_icase(const std::wstring& a, const std::wstring& b); // <0 / 0 / >0, like _wcsicmp

// True when running inside Wine/Proton (always false for the native Linux build).
bool is_wine();
// "proton" when the game will run under Wine/Proton (Linux or Wine), else "windows".
const char* game_platform();

// --- INI files (GetPrivateProfile/WritePrivateProfile semantics) -----------------------------------
// Case-insensitive sections/keys, ';' comments, quoted values. Files may be UTF-16 (BOM), UTF-8
// (BOM) or plain bytes. Values are returned without surrounding quotes.
std::wstring ini_read(const std::wstring& file, const std::wstring& section, const std::wstring& key,
                      const std::wstring& fallback = L"");
int ini_read_int(const std::wstring& file, const std::wstring& section, const std::wstring& key, int fallback);
// value == nullptr deletes the key. Preserves unrelated lines/comments. Returns success.
bool ini_write(const std::wstring& file, const std::wstring& section, const std::wstring& key, const std::wstring* value);
struct IniEntry { std::wstring key, value; };
struct IniSection { std::wstring name; std::vector<IniEntry> entries; };
// Ordered sections with their keys (first occurrence of a duplicate key wins, like the Win32 API).
std::vector<IniSection> ini_sections(const std::wstring& file);
// Drop cached profile data for a file (no-op outside Windows).
void ini_flush(const std::wstring& file);
// Rewrite a whole INI as UTF-16LE with BOM (the format the settings store has always used).
struct IniKeyValue { std::wstring section, key, value; };
void ini_write_all(const std::wstring& file, const std::wstring& header, const std::vector<IniKeyValue>& values);

// --- files ---------------------------------------------------------------------------------------
bool path_exists(const std::wstring& path);
bool is_link(const std::filesystem::path& path); // symlink / junction / reparse point
// UTF-8 text file -> wide; missing file -> empty; strips a UTF-8 BOM.
std::wstring read_text(const std::wstring& path);
std::string read_bytes(const std::filesystem::path& path, size_t limit); // throws on error/limit
void write_bytes_atomic(const std::filesystem::path& path, std::string_view data); // throws
// Delete a tree. undo=true asks for the Recycle Bin (Windows) / trash (Linux, falls back to a
// permanent delete when no trash is available). Returns success.
bool remove_tree(const std::wstring& path, bool undo);
// Recursive copy (no shell, no undo), overwriting files. Returns success.
bool copy_tree(const std::wstring& from, const std::wstring& to);
std::wstring temp_dir();
std::wstring make_unique_id(); // GUID-like text for staging directories

// --- paths / environment -------------------------------------------------------------------------
std::wstring self_path();
std::wstring roaming_appdata(); // %APPDATA% / $XDG_CONFIG_HOME (or ~/.config)
std::wstring env(const std::wstring& name);
// argv including the executable path; on POSIX captured by init(argc, argv).
std::vector<std::wstring> args();
void init(int argc, char** argv);
uint64_t tick_ms();
uint32_t process_id();

// --- network / crypto ----------------------------------------------------------------------------
// HTTPS GET restricted by the caller; throws std::runtime_error with a readable message.
std::string https_get(const std::string& url, size_t limit);
std::string sha256_hex(std::string_view data); // throws on failure

// --- process -------------------------------------------------------------------------------------
struct ProcessResult {
    bool started = false;
    int exit_code = -1; // -1 when the process could not be started
    std::wstring output; // stdout only (stderr discarded), bounded to output_limit bytes
};
// Run a program with arguments (no shell). When output_limit > 0 the output is captured;
// otherwise output stays empty. extra_env entries ("NAME=value") are added to the environment.
// Blocks until the process exits.
ProcessResult run_process(const std::wstring& program, const std::vector<std::wstring>& arguments,
                          const std::wstring& working_dir = L"", size_t output_limit = 0,
                          const std::vector<std::wstring>& extra_env = {});

// --- shell ---------------------------------------------------------------------------------------
// Open a folder/file/URL with the desktop's default handler. Returns success.
bool open_external(const std::wstring& target);
// Open a log/report in a text viewer (notepad on Windows, xdg-open elsewhere).
bool open_text_file(const std::wstring& path);
// Native file picker. filters: (description, "mod.ini;*.zip"). Empty result = cancelled/unavailable.
std::wstring pick_open_file(const std::wstring& title,
                            const std::vector<std::pair<std::wstring, std::wstring>>& filters);

// --- settings lock -------------------------------------------------------------------------------
// Cross-process lock for one mod directory (named mutex on Windows, flock on POSIX).
struct DirLock {
    explicit DirLock(const std::wstring& dir);
    ~DirLock();
    DirLock(const DirLock&) = delete;
    DirLock& operator=(const DirLock&) = delete;
    bool locked() const;
private:
    struct Impl;
    Impl* impl_;
};

} // namespace swml_platform
