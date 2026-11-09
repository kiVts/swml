#pragma once
#include <cstdint>

inline constexpr uintptr_t kGhidraImageBase = 0x140000000;
bool game_build_matches(); // fixed addresses usable: the original build, or a ported build this mod declared
uintptr_t game_base();
uint32_t game_build();     // the running exe's PE timestamp
bool game_build_ported();  // running a translated build (offsets that changed must use the new values)


// The mod's struct offsets were checked against this build (call from a ModSetup, before hooks install).
void mod_ported_to(uint32_t build_timestamp);

uintptr_t game_live(uintptr_t ghidra_address);
// Same, for addresses that may legitimately be gone from a ported build: 0 without logging a port miss.
uintptr_t game_live_if_mapped(uintptr_t ghidra_address);
uintptr_t game_live_native(uintptr_t sig_build_address);
uintptr_t game_ghidra(uintptr_t live);

template <typename T = void*>
T game_ptr(uintptr_t ghidra_address)
{
    return reinterpret_cast<T>(game_live(ghidra_address));
}

// Pick the value for the running build: field offsets and the like that the update changed.
template <typename T>
T per_build(T original, T ported)
{
    return game_build_ported() ? ported : original;
}
