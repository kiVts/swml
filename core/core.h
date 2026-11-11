#pragma once
#include "swml_api.h"

#include <windows.h>
#include <string>
#include <vector>

// Internal to swml_core.dll.

struct swml_mod {
    std::string id;
    std::wstring dir;      // %LOCALAPPDATA%\swml\mods\<id>
    std::wstring settings; // generated defaults + sparse overrides; not user-owned
    HMODULE module = nullptr;
    bool operational = false;
    uintptr_t image_begin = 0, image_end = 0;
    std::vector<void*> hooks; // targets created for this mod
    std::string status;
    uint32_t log_threshold = 0, profiling = 0; // access through atomic_ref
    uint64_t profiled_callbacks = 0, callback_ns = 0, longest_callback_ns = 0;
};

// core_main.cpp
const std::wstring& core_root();
const std::wstring& core_logs();
void core_log(const char* tag, const char* msg);
void corelog(const char* fmt, ...); // tagged "core"
std::vector<swml_mod*>& core_mods();
swml_mod* core_mod_at(uintptr_t address); // the mod whose DLL contains address, or null
void core_write_status(swml_mod* m);

// core_events.cpp: shared hook points owned by the core
bool core_events_install(); // SwapBuffers (present/frame) + update loop (if found)
void core_on_present(swml_mod* m, swml_present_cb cb, void* ctx);
void core_on_frame(swml_mod* m, swml_frame_cb cb, void* ctx);
bool core_on_loop(swml_mod* m, swml_loop_begin_cb b, swml_loop_end_cb e, swml_loop_idle_cb i, void* ctx);
void core_events_drop(swml_mod* m); // remove a mod's callbacks (mod failed to load)

// core_crash.cpp
void core_crash_install();
