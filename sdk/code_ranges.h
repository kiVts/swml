#pragma once
#include <windows.h>
#include <cstdint>

// Machine code a mod generates at run time (probe stubs and the like), described for stack walkers.
//
// A mod DLL that generates code exports
// extern "C" __declspec(dllexport) const SwmlCodeRange* __cdecl swml_code_ranges(uint32_t* count);
// Each range is one block of generated code with its own unwind table (RUNTIME_FUNCTION RVAs relative to base, the
// same layout as an image's .pdata, also registered with RtlAddFunctionTable) and a name per function. The diagnostics
// sampler unwinds through these blocks like through a module. Blocks are never freed.
struct SwmlCodeRange {
    uintptr_t base, end;
    const RUNTIME_FUNCTION* pdata;
    uint32_t count;
    const char* const* names; // count names, one per RUNTIME_FUNCTION
    const char* owner;        // "physics_fixes probes"
};
using SwmlCodeRanges_t = const SwmlCodeRange*(__cdecl*)(uint32_t* count);
