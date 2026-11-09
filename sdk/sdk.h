#pragma once
#include "swml_api.h"

// Access to the core from mod code (valid from swml_mod_init on).
const swml_api* swml();
swml_mod* swml_self();

// Optional hook for a mod: runs before the registered parts install (e.g. to resolve addresses).
// Return false to stop loading the mod (call mod_status first).
struct ModSetup {
    explicit ModSetup(bool (*fn)());
};
void mod_status(const char* fmt, ...);
