// Windows desktop subset of the platform layer: files, shell, HTTPS (WinHTTP), SHA-256 (BCrypt),
// process spawning and the native file picker. Linked into the launcher only, never into the
// in-game core DLL. Behaviour matches the original Win32 launcher.
#include "platform.h"

#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace swml_platform {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::vector<std::wstring> g_args;
std::wstring g_self;
bool g_com_initialized = false;

struct HandleClose { void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
using Handle = std::unique_ptr<void, HandleClose>;

std::wstring last_error(const char* what)
{
    return wide(std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")");
}

bool shell_op(UINT op, const std::wstring& from, const std::wstring& to, FILEOP_FLAGS flags)
{
    std::wstring f = from + L'\0', t = to + L'\0';
    SHFILEOPSTRUCTW s{};
    s.wFunc = op;
    s.pFrom = f.c_str();
    s.pTo = to.empty() ? nullptr : t.c_str();
    s.fFlags = flags | FOF_NOCONFIRMMKDIR | FOF_SILENT;
    return SHFileOperationW(&s) == 0 && !s.fAnyOperationsAborted;
}
} // namespace

void init(int argc, char** argv)
{
    (void)argc; (void)argv;
    g_com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_self = std::wstring(self, n && n < MAX_PATH ? n : 0);
    int count = 0;
    auto wide_args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide_args) {
        for (int i = 0; i < count; i++) g_args.emplace_back(wide_args[i]);
        LocalFree(wide_args);
    }
}

std::vector<std::wstring> args() { return g_args; }
std::wstring self_path() { return g_self; }
uint64_t tick_ms() { return GetTickCount64(); }

std::wstring read_text(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) s = s.substr(3);
    if (s.empty()) return {};
    // Lenient decode (like the old read_utf8): READMEs may contain stray bytes.
    int count = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), count);
    return out;
}

std::string read_bytes(const fs::path& path, size_t limit)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) fail("could not read " + utf8(path.wstring()));
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size_t(size.QuadPart) > limit)
        fail("file exceeds size limit: " + utf8(path.wstring()));
    std::string out(size_t(size.QuadPart), '\0');
    DWORD got = 0;
    if (!out.empty() && (!ReadFile(file.get(), out.data(), DWORD(out.size()), &got, nullptr) || got != out.size()))
        fail("file read failed: " + utf8(path.wstring()));
    return out;
}

bool remove_tree(const std::wstring& path, bool undo)
{
    FILEOP_FLAGS flags = FOF_NOCONFIRMATION | (undo ? FOF_ALLOWUNDO : FOF_NOERRORUI);
    return shell_op(FO_DELETE, path, L"", flags);
}

bool copy_tree(const std::wstring& from, const std::wstring& to)
{
    return shell_op(FO_COPY, from, to, FOF_NOCONFIRMATION | FOF_NOERRORUI);
}

std::wstring temp_dir()
{
    wchar_t buffer[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buffer);
    if (!n || n >= MAX_PATH) return L".\\";
    return buffer;
}

std::wstring make_unique_id()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) fail("could not allocate a unique id");
    wchar_t text[64]{};
    StringFromGUID2(guid, text, int(std::size(text)));
    return text;
}

std::wstring roaming_appdata()
{
    wchar_t* roaming = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming)
        r = std::wstring(roaming);
    CoTaskMemFree(roaming);
    return r;
}

std::wstring env(const std::wstring& name)
{
    wchar_t buffer[32768]{};
    auto count = GetEnvironmentVariableW(name.c_str(), buffer, DWORD(std::size(buffer)));
    if (!count || count >= std::size(buffer)) return {};
    return buffer;
}

std::string https_get(const std::string& url, size_t limit)
{
    struct InternetClose { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
    using Internet = std::unique_ptr<void, InternetClose>;
    auto wide_url = wide(url);
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        fail("invalid download URL");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    Internet session(WinHttpOpen(L"swml", is_wine() ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) fail(utf8(last_error("WinHttpOpen")));
    WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 30000);
    Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) fail(utf8(last_error("connecting")));
    Internet request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) fail(utf8(last_error("opening the request")));
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        fail(utf8(last_error("sending the request")));
    if (!WinHttpReceiveResponse(request.get(), nullptr)) fail(utf8(last_error("receiving the response")));
    DWORD status = 0, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
        fail(utf8(last_error("reading the response")));
    if (status != 200) fail("server answered HTTP " + std::to_string(status));

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) fail(utf8(last_error("downloading")));
        if (!available) return body;
        if (available > limit - body.size()) fail("download exceeds size limit");
        size_t at = body.size();
        body.resize(at + available);
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), body.data() + at, available, &got)) fail(utf8(last_error("downloading")));
        body.resize(at + got);
        if (!got) return body;
    }
}

std::string sha256_hex(std::string_view bytes)
{
    if (bytes.size() > MAXDWORD) fail("data too large to hash");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32]{};
    bool okay = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) &&
                BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), ULONG(bytes.size()), 0)) &&
                BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!okay) fail("SHA-256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (UCHAR byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}

ProcessResult run_process(const std::wstring& program, const std::vector<std::wstring>& arguments,
                          const std::wstring& working_dir, size_t output_limit,
                          const std::vector<std::wstring>& extra_env)
{
    // Build a Unicode environment block with the extra variables merged in.
    std::vector<wchar_t> env_block;
    bool have_env = !extra_env.empty();
    if (have_env) {
        struct CaseLess { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
        std::map<std::wstring, std::wstring, CaseLess> values;
        wchar_t* block = GetEnvironmentStringsW();
        if (block) {
            for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
                std::wstring entry = p;
                auto equal = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
                if (equal != entry.npos) values[entry.substr(0, equal)] = entry.substr(equal + 1);
            }
            FreeEnvironmentStringsW(block);
        }
        for (const auto& entry : extra_env) {
            auto equal = entry.find(L'=');
            if (equal != entry.npos) values[entry.substr(0, equal)] = entry.substr(equal + 1);
        }
        for (const auto& [key, value] : values) {
            auto entry = key + L"=" + value;
            env_block.insert(env_block.end(), entry.begin(), entry.end());
            env_block.push_back(0);
        }
        env_block.push_back(0);
    }
    // Quote like CommandLineToArgvW expects (backslashes before quotes doubled).
    auto quote = [](const std::wstring& arg) {
        if (!arg.empty() && arg.find_first_of(L" \t\"\n") == arg.npos) return arg;
        std::wstring out = L"\"";
        size_t backslashes = 0;
        for (wchar_t c : arg) {
            if (c == L'\\') { backslashes++; out += c; continue; }
            if (c == L'"') { out.append(backslashes + 1, L'\\'); out += c; }
            else out += c;
            backslashes = 0;
        }
        out.append(backslashes, L'\\');
        out += L'"';
        return out;
    };
    std::wstring cmd = quote(program);
    for (const auto& arg : arguments) cmd += L" " + quote(arg);

    ProcessResult result;
    Handle read_end, write_end, null_out;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (output_limit > 0) {
        HANDLE r = nullptr, w = nullptr;
        if (!CreatePipe(&r, &w, &sa, 0)) return result;
        read_end.reset(r);
        write_end.reset(w);
        SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
        // stderr must not pollute captured output.
        null_out.reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    }
    STARTUPINFOW si{sizeof(si)};
    if (output_limit > 0) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_end.get();
        si.hStdError = null_out ? null_out.get() : write_end.get();
        si.hStdInput = nullptr;
    }
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
    buffer.push_back(0);
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, output_limit > 0,
                        CREATE_NO_WINDOW | (have_env ? CREATE_UNICODE_ENVIRONMENT : 0),
                        have_env ? env_block.data() : nullptr,
                        working_dir.empty() ? nullptr : working_dir.c_str(), &si, &pi))
        return result;
    CloseHandle(pi.hThread);
    write_end.reset(); // our copy; the child keeps its own
    if (output_limit > 0) {
        std::string captured;
        char chunk[4096];
        DWORD got = 0;
        while (ReadFile(read_end.get(), chunk, DWORD(std::size(chunk)), &got, nullptr) && got > 0) {
            if (captured.size() < output_limit) captured.append(chunk, std::min<size_t>(got, output_limit - captured.size()));
        }
        if (!captured.empty()) {
            int count = MultiByteToWideChar(CP_UTF8, 0, captured.data(), int(captured.size()), nullptr, 0);
            result.output.resize(size_t(count));
            MultiByteToWideChar(CP_UTF8, 0, captured.data(), int(captured.size()), result.output.data(), count);
        }
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    result.started = true;
    result.exit_code = int(code);
    return result;
}

bool open_external(const std::wstring& target)
{
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool open_text_file(const std::wstring& path)
{
    std::wstring quoted = L"\"" + path + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

std::wstring pick_open_file(const std::wstring& title,
                            const std::vector<std::pair<std::wstring, std::wstring>>& filters)
{
    std::wstring result;
    if (!g_com_initialized) return result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    std::vector<COMDLG_FILTERSPEC> types;
    for (const auto& [description, pattern] : filters) types.push_back({description.c_str(), pattern.c_str()});
    if (!types.empty()) dlg->SetFileTypes(UINT(types.size()), types.data());
    dlg->SetTitle(title.c_str());
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) result = path;
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

} // namespace swml_platform
