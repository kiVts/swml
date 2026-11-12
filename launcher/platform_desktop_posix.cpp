// POSIX desktop subset of the platform layer (native Linux launcher): files, shell, HTTPS
// (libcurl), SHA-256 (OpenSSL), process spawning and desktop file pickers. Linked into the
// launcher only. Release builds link curl/OpenSSL statically so the launcher stays a single
// dependency-free binary (Steam Deck friendly).
#include "platform.h"
#include "platform_utf.h"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace swml_platform {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::wstring trim(const std::wstring& s)
{
    auto begin = s.find_first_not_of(L" \t\r\n");
    if (begin == s.npos) return {};
    return s.substr(begin, s.find_last_not_of(L" \t\r\n") - begin + 1);
}
} // namespace

std::wstring read_text(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) s = s.substr(3);
    return detail::decode_utf8(s, false); // lenient: READMEs may contain stray bytes
}

std::string read_bytes(const fs::path& path, size_t limit)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("could not read " + utf8(path.wstring()));
    std::string out;
    char buffer[16384];
    while (f) {
        f.read(buffer, sizeof(buffer));
        auto got = size_t(f.gcount());
        if (got > limit - out.size()) fail("file exceeds size limit: " + utf8(path.wstring()));
        out.append(buffer, got);
    }
    if (f.bad()) fail("file read failed: " + utf8(path.wstring()));
    return out;
}

bool remove_tree(const std::wstring& path, bool undo)
{
    if (undo) {
        // Prefer the desktop trash; fall back to a permanent delete when none is available.
        const std::pair<const char*, std::vector<std::wstring>> tools[] = {
            {"gio", {L"trash", path}},
            {"kioclient5", {L"move", path, L"trash:/"}},
            {"kioclient", {L"move", path, L"trash:/"}},
        };
        for (const auto& tool : tools) {
            auto result = run_process(wide(tool.first), tool.second, L"", 256);
            if (result.started && result.exit_code == 0) return true;
        }
    }
    std::error_code error;
    fs::remove_all(fs::path(path), error);
    return !error;
}

bool copy_tree(const std::wstring& from, const std::wstring& to)
{
    std::error_code error;
    fs::copy(fs::path(from), fs::path(to),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, error);
    return !error;
}

std::wstring temp_dir()
{
    const char* tmp = ::getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? tmp : "/tmp";
    if (dir.back() != '/') dir += '/';
    return wide(dir);
}

std::wstring roaming_appdata()
{
    const char* xdg = ::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return wide(xdg);
    const char* home = ::getenv("HOME");
    if (home && *home) return wide(home) + L"/.config";
    return {};
}

std::wstring env(const std::wstring& name)
{
    const char* value = ::getenv(utf8(name).c_str());
    return value ? wide(value) : std::wstring{};
}

std::string https_get(const std::string& url, size_t limit)
{
    CURL* curl = curl_easy_init();
    if (!curl) fail("could not initialise HTTPS");
    std::string body;
    struct Sink {
        static size_t write(char* data, size_t size, size_t count, void* user) {
            auto* out = static_cast<std::pair<std::string*, size_t>*>(user);
            size_t n = size * count;
            if (n > out->second - out->first->size()) return 0; // abort: size limit
            out->first->append(data, n);
            return n;
        }
    };
    std::pair<std::string*, size_t> sink{&body, limit};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "swml");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Sink::write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code == CURLE_WRITE_ERROR && body.size() >= limit) fail("download exceeds size limit");
    if (code != CURLE_OK) fail(std::string("download failed: ") + curl_easy_strerror(code));
    if (status != 200) fail("server answered HTTP " + std::to_string(status));
    return body;
}

std::string sha256_hex(std::string_view data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr) || length != 32)
        fail("SHA-256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < length; i++) { out += hex[digest[i] >> 4]; out += hex[digest[i] & 15]; }
    return out;
}

ProcessResult run_process(const std::wstring& program, const std::vector<std::wstring>& arguments,
                          const std::wstring& working_dir, size_t output_limit,
                          const std::vector<std::wstring>& extra_env)
{
    ProcessResult result;
    int pipefd[2] = {-1, -1};
    if (output_limit > 0 && ::pipe(pipefd) != 0) return result;

    std::string exe = utf8(program);
    std::vector<std::string> storage;
    storage.push_back(exe);
    for (const auto& a : arguments) storage.push_back(utf8(a));
    std::vector<char*> argv;
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    std::string cwd = utf8(working_dir);

    pid_t pid = ::fork();
    if (pid < 0) {
        if (pipefd[0] >= 0) { ::close(pipefd[0]); ::close(pipefd[1]); }
        return result;
    }
    if (pid == 0) {
        if (output_limit > 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            if (pipefd[1] > STDERR_FILENO) ::close(pipefd[1]);
            // stderr must not pollute captured output (zenity prints GTK warnings there).
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); if (devnull > STDERR_FILENO) ::close(devnull); }
        } else {
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDOUT_FILENO); ::dup2(devnull, STDERR_FILENO); if (devnull > STDERR_FILENO) ::close(devnull); }
        }
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) _exit(127);
        for (const auto& entry : extra_env) {
            auto text = utf8(entry);
            auto equal = text.find('=');
            if (equal != std::string::npos) {
                auto name = text.substr(0, equal), value = text.substr(equal + 1);
                ::setenv(name.c_str(), value.c_str(), 1);
            }
        }
        ::execvp(exe.c_str(), argv.data());
        _exit(127);
    }
    if (output_limit > 0) {
        ::close(pipefd[1]);
        std::string captured;
        char chunk[4096];
        ssize_t got;
        while ((got = ::read(pipefd[0], chunk, sizeof(chunk))) > 0)
            if (captured.size() < output_limit) captured.append(chunk, std::min<size_t>(size_t(got), output_limit - captured.size()));
        ::close(pipefd[0]);
        result.output = detail::decode_utf8(captured, false);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    result.started = true;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

bool open_external(const std::wstring& target)
{
    // Detach: xdg-open may block on some handlers; the launcher must not.
    std::string path = utf8(target);
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        pid_t inner = ::fork();
        if (inner != 0) _exit(inner < 0 ? 1 : 0);
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) { ::dup2(devnull, 0); ::dup2(devnull, 1); ::dup2(devnull, 2); if (devnull > 2) ::close(devnull); }
        ::execlp("xdg-open", "xdg-open", path.c_str(), (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool open_text_file(const std::wstring& path) { return open_external(path); }

std::wstring pick_open_file(const std::wstring& title,
                            const std::vector<std::pair<std::wstring, std::wstring>>& filters)
{
    // Desktop file pickers, in order of likelihood. The UI falls back to a path prompt when
    // none is installed (e.g. gamescope session on Steam Deck).
    std::wstring zenity_filter, kdialog_filter;
    for (const auto& [description, pattern] : filters) {
        std::wstring globs;
        size_t start = 0;
        while (start < pattern.size()) {
            auto end = pattern.find(L';', start);
            auto glob = trim(pattern.substr(start, end == pattern.npos ? pattern.npos : end - start));
            if (!glob.empty()) { if (!globs.empty()) globs += L" "; globs += glob; }
            if (end == pattern.npos) break;
            start = end + 1;
        }
        if (!zenity_filter.empty()) zenity_filter += L" ";
        zenity_filter += description + L" | " + globs;
        if (!kdialog_filter.empty()) kdialog_filter += L"\n";
        kdialog_filter += globs + L"|" + description;
    }
    auto attempt = [&](const wchar_t* tool, std::vector<std::wstring> arguments) -> std::wstring {
        auto result = run_process(tool, arguments, L"", 65536);
        if (!result.started || result.exit_code != 0) return {};
        return trim(result.output);
    };
    std::vector<std::wstring> zenity{L"--file-selection", L"--title=" + title};
    if (!zenity_filter.empty()) zenity.push_back(L"--file-filter=" + zenity_filter);
    if (auto picked = attempt(L"zenity", zenity); !picked.empty()) return picked;
    std::vector<std::wstring> kdialog{L"--getopenfile", L"--title", title};
    if (!kdialog_filter.empty()) kdialog.push_back(kdialog_filter);
    if (auto picked = attempt(L"kdialog", kdialog); !picked.empty()) return picked;
    return {};
}

} // namespace swml_platform
