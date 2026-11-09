#pragma once
#include "swml_v3.h"
const swml_api_v3* core_v3_api();
void core_v3_attach(swml_mod*);
void core_v3_drop(swml_mod*);
// Shares the core's hook chain; internal SDK providers need no mod DLL of their own.
bool core_internal_hook(void* target, void* detour, void** original, const char* name);
