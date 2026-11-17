#pragma once
#include "swml_api.h"

#include <cstddef>

// core_port.cpp: port data for mods (swml_api port_*).
void core_port_init(); // resolve now (start-up, before mods hook the game); logs the result
int (*core_port_info())(swml_port_info*);
size_t (*core_port_ranges())(int, swml_port_range*, size_t);
size_t (*core_port_fields())(swml_port_field*, size_t);
