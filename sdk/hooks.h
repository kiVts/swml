#pragma once
#include <cstdint>

// Each feature file of a mod declares one HookRegistrar at file scope:
//
// static HookRegistrar reg{{"frametime", /*needs_known_build=*/false, &install}};
//
// swml_mod_init (swml_sdk.cpp) runs every registered install(); install() calls hook_api() /
// hook_game() / hook_address() and returns false on failure. needs_known_build=true parts are
// skipped when the running exe isn't the build their fixed addresses came from.
struct HookDef {
    const char* name;
    bool needs_known_build;
    bool (*install)();
};

struct HookRegistrar {
    explicit HookRegistrar(const HookDef& def);
};

bool hook_api(const wchar_t* module, const char* proc, void* detour, void** original);
bool hook_address(uintptr_t target, void* detour, void** original); // address found at runtime
bool hook_game(uintptr_t ghidra_address, void* detour, void** original); // fixed address, known build only
