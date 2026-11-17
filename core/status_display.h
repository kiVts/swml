#pragma once
#include <cstdint>

// Installs the core-owned version/status label at the native UI boundary.
// This owner is internal and is never counted as a loaded mod.
bool core_status_install();

struct swml_mod;
// swml_api status_line: set (or with NULL / "" remove) a mod's segment after the mod count; color AABBGGRR, 0 = the
// label's own. Any thread.
void core_status_line_set(const swml_mod* mod, const char* text, uint32_t color);
