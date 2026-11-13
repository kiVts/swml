
#include "core.h"
#include "game.h"
#include "log.h"

#include <MinHook.h>

#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace {

using SetFilter_t = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilter_t g_orig_set_filter = nullptr;
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_game_filter{nullptr};
std::atomic<int> g_logged{0};
std::atomic<bool> g_dumped{false};
HMODULE g_self = nullptr;

bool fatal_code(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN (__fastfail / /GS)
    case 0xC0000374: // STATUS_HEAP_CORRUPTION
        return true;
    }
    return false;
}

bool in_module(HMODULE m, uintptr_t a)
{
    if (!m) return false;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
    uintptr_t b = reinterpret_cast<uintptr_t>(m);
    return a >= b && a < b + nt->OptionalHeader.SizeOfImage;
}

void describe(uintptr_t a, char* out, size_t n)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(a), &m) || !m) {
        snprintf(out, n, "%llx", static_cast<unsigned long long>(a));
        return;
    }
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    if (reinterpret_cast<uintptr_t>(m) == game_base())
        snprintf(out, n, "%s!%llx", base, static_cast<unsigned long long>(a - game_base() + kGhidraImageBase));
    else
        snprintf(out, n, "%s+%llx", base, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(m)));
}

void log_exception(EXCEPTION_POINTERS* ep, const char* tag)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    char where[160];
    describe(reinterpret_cast<uintptr_t>(er->ExceptionAddress), where, sizeof(where));
    std::string extra;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        char b[96];
        snprintf(b, sizeof(b), " (%s address %llx)", er->ExceptionInformation[0] == 8 ? "executing" :
                 er->ExceptionInformation[0] ? "writing" : "reading", static_cast<unsigned long long>(er->ExceptionInformation[1]));
        extra = b;
    }
    swlog("%s exception 0x%08lx at %s%s thread %lu", tag, er->ExceptionCode, where, extra.c_str(), GetCurrentThreadId());

    // Walk the stack from the exception context (a copy; this is the current thread).
    CONTEXT ctx = *ep->ContextRecord;
    std::string stack;
    swml_mod* culprit = nullptr;
    for (int i = 0; i < 32 && ctx.Rip; i++) {
        if (!culprit) culprit = core_mod_at(ctx.Rip);
        char f[160];
        describe(ctx.Rip, f, sizeof(f));
        stack += i ? " <- " : "";
        stack += f;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!rf) { // leaf function: return address is at [rsp]
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void* handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, rf, &ctx, &handler_data, &frame, nullptr);
        }
    }
    swlog("%s stack: %s", tag, stack.c_str());
    if (culprit) {
        char b[300];
        snprintf(b, sizeof(b), "crashed: exception 0x%08lx at %s", er->ExceptionCode, where);
        culprit->status = b;
        core_write_status(culprit);
        swlog("%s: in mod '%s'", tag, culprit->id.c_str());
    }
}

void write_dump(EXCEPTION_POINTERS* ep);
LONG CALLBACK vectored(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (!fatal_code(er->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
    if (in_module(g_self, at) || core_mod_at(at)) return EXCEPTION_CONTINUE_SEARCH; // core / mod __try reads
    if (g_logged.fetch_add(1) < 10) log_exception(ep, "CRASH?");
    // The game's own handlers can end the process before the unhandled filter runs, leaving no dump: write one
    // now for an access violation in game code (once per process).
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && in_module(GetModuleHandleW(nullptr), at)) write_dump(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void write_dump(EXCEPTION_POINTERS* ep)
{
    if (g_dumped.exchange(true)) return;
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    using Dump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                 PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto dump = dbg ? reinterpret_cast<Dump_t>(GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
    if (!dump) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"\\crash_%04d%02d%02d_%02d%02d%02d.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring path = std::wstring(log_dir()) + name;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                           MiniDumpWithUnloadedModules | MiniDumpWithDataSegs);
    BOOL ok = dump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
    CloseHandle(f);
    swlog("CRASH minidump %s: %ls", ok ? "written" : "FAILED", path.c_str());
}

LONG WINAPI unhandled(EXCEPTION_POINTERS* ep)
{
    log_exception(ep, "CRASH");
    write_dump(ep);
    LPTOP_LEVEL_EXCEPTION_FILTER game = g_game_filter.load();
    return game ? game(ep) : EXCEPTION_CONTINUE_SEARCH;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI set_filter_detour(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    if (f == &unhandled) return g_orig_set_filter(f);
    return g_game_filter.exchange(f); // keep ours installed; call the game's after ours
}

} // namespace

void core_crash_install()
{
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&core_crash_install), &g_self);
    AddVectoredExceptionHandler(1, &vectored);
    g_game_filter = SetUnhandledExceptionFilter(&unhandled);
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter"));
    bool ok = target && MH_CreateHook(target, reinterpret_cast<void*>(&set_filter_detour), reinterpret_cast<void**>(&g_orig_set_filter)) == MH_OK;
    swlog("crash capture: on (game filter %p chained, filter guard %s; dumps go to logs\\crash_<time>.dmp)",
          static_cast<void*>(g_game_filter.load()), ok ? "OK" : "FAILED");
}

