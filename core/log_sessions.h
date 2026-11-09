#pragma once

#include <cstdio>
#include <string>

namespace swml_logs {

// Five sessions in total: the readable current log and four gzip archives.
inline constexpr size_t archived_sessions = 4;
inline constexpr size_t compression_chunk_bytes = 1024 * 1024;

struct Session {
    FILE* file = nullptr; // caller owns it; fclose releases the exclusive writer lock
    bool fresh = false;
    std::string warning;
};

// Called once per log, before anything writes to it: <directory>\<name>.log, with
// history in <directory>\sessions\<name>-session-*.log.gz. Archives are published before
// the old log is truncated. On archive failure, append to the original instead of losing it.
Session open_session(const std::wstring& logs_directory, const std::wstring& name = L"swml");

} // namespace swml_logs
