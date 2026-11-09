#include "log_sessions.h"

#include <windows.h>
#include <libdeflate.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace swml_logs {
namespace {

constexpr wchar_t suffix[] = L".log.gz";
constexpr size_t sequence_digits = 20;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE release() { HANDLE h = value; value = INVALID_HANDLE_VALUE; return h; }
};

struct Archive {
    uint64_t sequence;
    std::wstring path;
};

void warn(Session& session, const char* what, DWORD error = GetLastError())
{
    if (!session.warning.empty()) session.warning += "; ";
    session.warning += what;
    session.warning += " (Windows error " + std::to_string(error) + ")";
}

bool make_directory(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
    SetLastError(ERROR_DIRECTORY);
    return false;
}

// Only our exact names participate in retention. User exports and other mods'
// traces/CSVs in the log directory must never be pruned.
bool parse_name(const std::wstring& name, const std::wstring& prefix, uint64_t& sequence, bool& temporary)
{
    const size_t begin = prefix.size();
    const size_t end = begin + sequence_digits;
    if (name.compare(0, begin, prefix) != 0 || name.size() < end) return false;
    sequence = 0;
    for (size_t i = begin; i < end; ++i) {
        if (name[i] < L'0' || name[i] > L'9') return false;
        unsigned digit = unsigned(name[i] - L'0');
        if (sequence > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        sequence = sequence * 10 + digit;
    }
    temporary = name.compare(end, std::wstring::npos, L".log.gz.tmp") == 0;
    return sequence != 0 && (temporary || name.compare(end, std::wstring::npos, suffix) == 0);
}

bool scan(const std::wstring& directory, const std::wstring& prefix, std::vector<Archive>& archives,
          uint64_t& latest, Session& session)
{
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory + L"\\" + prefix + L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        warn(session, "could not list log history");
        return false;
    }
    do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        uint64_t sequence = 0;
        bool temporary = false;
        if (!parse_name(data.cFileName, prefix, sequence, temporary)) continue;
        latest = std::max(latest, sequence);
        std::wstring path = directory + L"\\" + data.cFileName;
        if (temporary) {
            // The readable source survives interrupted compression. Holding the
            // current log's writer lock excludes another session's compressor.
            if (!DeleteFileW(path.c_str())) warn(session, "could not remove unfinished log archive");
        } else {
            archives.push_back({sequence, std::move(path)});
        }
    } while (FindNextFileW(find, &data));
    DWORD error = GetLastError();
    FindClose(find);
    if (error == ERROR_NO_MORE_FILES) return true;
    warn(session, "could not finish listing log history", error);
    return false;
}

bool write_all(HANDLE file, const void* bytes, size_t size)
{
    const auto* cursor = static_cast<const unsigned char*>(bytes);
    while (size) {
        DWORD written = 0;
        if (!WriteFile(file, cursor, DWORD(size), &written, nullptr)) return false;
        if (!written) { SetLastError(ERROR_WRITE_FAULT); return false; }
        cursor += written;
        size -= written;
    }
    return true;
}

bool compress(HANDLE source, HANDLE output)
{
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(source, start, nullptr, FILE_BEGIN)) return false;
    std::unique_ptr<libdeflate_compressor, decltype(&libdeflate_free_compressor)> compressor(
        libdeflate_alloc_compressor(1), &libdeflate_free_compressor);
    if (!compressor) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    try {
        std::vector<unsigned char> input(compression_chunk_bytes);
        std::vector<unsigned char> packed(libdeflate_gzip_compress_bound(compressor.get(), input.size()));
        bool first = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(source, input.data(), DWORD(input.size()), &read, nullptr)) return false;
            if (!read && !first) break;
            size_t size = libdeflate_gzip_compress(compressor.get(), input.data(), read,
                                                  packed.data(), packed.size());
            if (!size) { SetLastError(ERROR_INVALID_DATA); return false; }
            if (!write_all(output, packed.data(), size)) return false;
            first = false;
            if (!read) break;
        }
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    return FlushFileBuffers(output) != FALSE;
}

bool archive(HANDLE source, const std::wstring& directory, const std::wstring& prefix, uint64_t& latest,
             std::vector<Archive>& archives, Session& session)
{
    if (latest == std::numeric_limits<uint64_t>::max()) {
        warn(session, "log archive sequence exhausted", ERROR_ARITHMETIC_OVERFLOW);
        return false;
    }
    uint64_t sequence = ++latest;
    wchar_t digits[32];
    swprintf_s(digits, L"%020llu", static_cast<unsigned long long>(sequence));
    std::wstring path = directory + L"\\" + prefix + digits + suffix;
    std::wstring temporary = path + L".tmp";
    Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.value == INVALID_HANDLE_VALUE) {
        warn(session, "could not create compressed log archive");
        return false;
    }
    bool ok = compress(source, output.value);
    DWORD error = GetLastError();
    if (!CloseHandle(output.release()) && ok) { ok = false; error = GetLastError(); }
    // Publish only the complete flushed .gz; never overwrite an existing archive.
    if (ok && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        ok = false;
        error = GetLastError();
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
        warn(session, "could not finish compressed log archive; original retained", error);
        return false;
    }
    archives.push_back({sequence, std::move(path)});
    return true;
}

void migrate_legacy(const std::wstring& logs, const std::wstring& history, const std::wstring& prefix, uint64_t& latest,
                    std::vector<Archive>& archives, Session& session)
{
    std::wstring path = logs + L"\\swml.prev.log";
    Handle previous(CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (previous.value == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) warn(session, "could not archive legacy swml.prev.log", error);
        return;
    }
    if (archive(previous.value, history, prefix, latest, archives, session)) {
        FILE_DISPOSITION_INFO remove{TRUE};
        if (!SetFileInformationByHandle(previous.value, FileDispositionInfo, &remove, sizeof(remove)))
            warn(session, "legacy log compressed but original could not be removed");
    }
}

void prune(std::vector<Archive>& archives, Session& session)
{
    std::sort(archives.begin(), archives.end(), [](const Archive& a, const Archive& b) {
        return a.sequence > b.sequence;
    });
    for (size_t i = archived_sessions; i < archives.size(); ++i)
        if (!DeleteFileW(archives[i].path.c_str())) warn(session, "could not remove expired log archive");
}

} // namespace

Session open_session(const std::wstring& logs, const std::wstring& name)
{
    Session session;
    if (!make_directory(logs)) { warn(session, "could not create log directory"); return session; }
    // The CRT FILE owns this handle for the entire session: a second process
    // cannot archive, truncate or append to an active log.
    Handle current(CreateFileW((logs + L"\\" + name + L".log").c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (current.value == INVALID_HANDLE_VALUE) {
        warn(session, "could not open the log exclusively; another session may be active");
        return session;
    }
    LARGE_INTEGER size{};
    bool have_size = GetFileSizeEx(current.value, &size) != FALSE;
    if (!have_size) warn(session, "could not inspect previous log; original retained");
    session.fresh = have_size && size.QuadPart == 0;
    std::wstring history = logs + L"\\sessions", prefix = name + L"-session-";
    std::vector<Archive> archives;
    uint64_t latest = 0;
    if (!make_directory(history)) {
        warn(session, "could not create log history; original retained");
    } else if (scan(history, prefix, archives, latest, session)) {
        if (name == L"swml") migrate_legacy(logs, history, prefix, latest, archives, session);
        if (have_size && size.QuadPart > 0 && archive(current.value, history, prefix, latest, archives, session)) {
            LARGE_INTEGER start{};
            if (SetFilePointerEx(current.value, start, nullptr, FILE_BEGIN) && SetEndOfFile(current.value))
                session.fresh = true;
            else
                warn(session, "previous log compressed but could not start a fresh log");
        }
        if (session.fresh) prune(archives, session);
    }
    // Archive failure intentionally falls back to append, never truncation.
    int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(current.value), _O_WRONLY | _O_APPEND | _O_TEXT);
    if (descriptor == -1) { warn(session, "could not attach log stream", ERROR_OPEN_FAILED); return session; }
    current.release(); // _close/fclose now owns the Windows handle
    session.file = _fdopen(descriptor, "a");
    if (!session.file) { _close(descriptor); warn(session, "could not create log stream", ERROR_OPEN_FAILED); }
    return session;
}

} // namespace swml_logs
