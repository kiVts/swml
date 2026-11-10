#include "core.h"
#include "health.h"
#include "core_v2.h"
#include "core_v3.h"
#include "native_ui.h"
#include "swml_v4.h"
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <set>

namespace {
struct Command { swml_mod* owner; swml_handle handle; swml_command_info info; bool active = true; };
std::mutex mutex;
std::set<swml_mod*> owners;
std::map<std::string, Command> commands;
uint64_t capabilities() {
    return SWML_V3_CAP_COMMANDS | SWML_V3_CAP_SETTINGS_WRITE | SWML_V3_CAP_LOGGING | SWML_V3_CAP_UI_APPLICATION_CONTEXT |
           SWML_V3_CAP_UI_TEXT_AUTO_HEIGHT | SWML_V3_CAP_UI_BUTTON_RESIZE |
           (sdk_ui::enabled() ? uint64_t(SWML_V3_CAP_NATIVE_UI) : 0);
}
bool registered(swml_mod* m) { std::lock_guard lock(mutex); return owners.contains(m); }
bool utf8(const char* value, size_t limit) {
    if (!value || strnlen_s(value, limit + 1) > limit) return false;
    return !*value || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0) > 0;
}
bool command_name(const char* name) {
    if (!name || !*name || strnlen_s(name, SWML_NAME_CAPACITY) >= SWML_NAME_CAPACITY) return false;
    for (const char* p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-')) return false;
    return true;
}
const char* result_name(swml_result result) {
    switch (result) {
    case SWML_OK: return "OK"; case SWML_PENDING: return "pending";
    case SWML_INVALID: return "invalid argument"; case SWML_UNAVAILABLE: return "unavailable";
    case SWML_NOT_FOUND: return "not found"; case SWML_CONFLICT: return "conflict";
    case SWML_LIMIT: return "limit exceeded"; case SWML_BUFFER_SMALL: return "buffer too small";
    case SWML_IO_ERROR: return "IO error"; case SWML_DENIED: return "denied";
    case SWML_WRONG_THREAD: return "wrong execution context"; default: return "unknown result";
    case SWML_APPLIED_WITHOUT_EVENT: return "applied; change notification unavailable";
    case SWML_RECOVERED_BACKUP: return "recovered from backup";
    case SWML_CORRUPT: return "corrupt stored data";
    }
}
swml_result log(swml_mod* m, uint32_t level, const char* message) {
    if (level > SWML_LOG_ERROR || !utf8(message, 16384) || strchr(message, '\r') || strchr(message, '\n')) return SWML_INVALID;
    if (!registered(m)) return SWML_DENIED;
    if (level == SWML_LOG_ERROR) swml_health::note_fault();
    if (level < std::atomic_ref(m->log_threshold).load()) return SWML_OK;
    const char* levels[]{"DEBUG", "INFO", "WARN", "ERROR"};
    std::string line = std::string("[") + levels[level] + "] " + message;
    core_log(m->id.c_str(), line.c_str()); return SWML_OK;
}
swml_result setting_write(swml_mod* m, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    auto valid = [](const wchar_t* s) { return s && *s && wcsnlen_s(s, 256) < 256 && !wcspbrk(s, L"\r\n[]="); };
    if (!valid(section) || !valid(key) || !value || wcsnlen_s(value, 32767) >= 32767 || wcspbrk(value, L"\r\n")) return SWML_INVALID;
    if (!registered(m)) return SWML_DENIED;
    return WritePrivateProfileStringW(section, key, value, m->settings.c_str()) ? SWML_OK : SWML_IO_ERROR;
}
swml_result command_register(swml_mod* m, const char* name, const char* help, swml_callback callback, void* context, swml_handle* out) {
    if (out) *out = 0;
    if (!out || !command_name(name) || !utf8(help, 255) || !callback) return SWML_INVALID;
    if (!registered(m) || !std::string(name).starts_with(m->id + ".")) return SWML_DENIED;
    std::lock_guard lock(mutex);
    if (!owners.contains(m)) return SWML_DENIED;
    if (commands.contains(name)) return SWML_CONFLICT;
    if (commands.size() >= 256) return SWML_LIMIT;
    swml_handle handle = 0;
    auto r = core_v2_api()->subscribe(m, name, callback, context, &handle);
    if (r != SWML_OK) return r;
    Command command{}; command.owner = m; command.handle = handle;
    strcpy_s(command.info.name, name); strcpy_s(command.info.help, help);
    commands[name] = command; *out = handle; return SWML_OK;
}
swml_result command_remove(swml_mod* m, swml_handle handle) {
    std::lock_guard lock(mutex);
    if (!owners.contains(m)) return SWML_DENIED;
    for (auto it = commands.begin(); it != commands.end(); ++it) if (it->second.handle == handle) {
        if (it->second.owner != m) return SWML_DENIED;
        it->second.active = false;
        auto r = core_v2_api()->cancel(m, handle);
        if (r != SWML_PENDING) commands.erase(it);
        return r == SWML_NOT_FOUND ? SWML_OK : r;
    }
    return SWML_NOT_FOUND;
}
swml_result command_run(swml_mod* m, const char* name, const char* arguments) {
    if (!command_name(name) || !utf8(arguments, 4095)) return SWML_INVALID;
    std::lock_guard lock(mutex);
    if (!owners.contains(m)) return SWML_DENIED;
    auto it = commands.find(name);
    if (it == commands.end() || !it->second.active) return SWML_NOT_FOUND;
    return core_v2_forward(m, it->second.owner, name, arguments, static_cast<uint32_t>(strlen(arguments) + 1));
}
swml_result command_list(swml_mod* m, swml_command_info* out, uint32_t capacity, uint32_t* needed) {
    if (needed) *needed = 0;
    if (!needed || (!out && capacity)) return SWML_INVALID;
    std::lock_guard lock(mutex);
    if (!owners.contains(m)) return SWML_DENIED;
    for (const auto& [name, command] : commands) if (command.active) ++*needed;
    if (capacity < *needed) return SWML_BUFFER_SMALL;
    uint32_t i = 0; for (const auto& [name, command] : commands) if (command.active) out[i++] = command.info;
    return SWML_OK;
}
const swml_api_v3 api{
    SWML_V3_VERSION, sizeof(swml_api_v3), capabilities, result_name, log, setting_write,
    command_register, command_remove, command_run, command_list,
    sdk_ui::enable, sdk_ui::watch, sdk_ui::unwatch, sdk_ui::create, sdk_ui::destroy,
    sdk_ui::children, sdk_ui::find_class, sdk_ui::inspect, sdk_ui::set_rect, sdk_ui::set_visible,
    sdk_ui::set_enabled, sdk_ui::set_text, sdk_ui::get_text, sdk_ui::watch_rect
};
}
const swml_api_v3* core_v3_api() { return &api; }
void core_v3_attach(swml_mod* m) {
    { std::lock_guard lock(mutex); owners.insert(m); }
    sdk_ui::attach(m);
}
void core_v3_drop(swml_mod* m) {
    { std::lock_guard lock(mutex); owners.erase(m);
      for (auto it = commands.begin(); it != commands.end();)
          if (it->second.owner == m) it = commands.erase(it); else ++it;
    }
    sdk_ui::drop(m);
}
