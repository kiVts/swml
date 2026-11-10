#pragma once
#include "swml_v4.h"
#include <string>
#include <vector>

// Backend contract; all object methods are called ONLY at native UI boundaries.
// The mock backend in tests validates registry behavior without starting Stormworks.
struct NativeUiBackend {
    bool (*enable)();
    uintptr_t (*class_table)(const char*);
    uintptr_t (*table)(void*);
    bool (*is_a)(void*, const char*);
    std::vector<void*> (*children)(void*);
    void* (*create)(const swml_ui_desc&);
    void (*attach)(void* parent, uintptr_t game, void* child, const swml_ui_desc&);
    void (*erase)(void* object, uintptr_t game, void* known_parent);
    swml_ui_info (*inspect)(void*);
    void (*rect)(void*, const swml_ui_rect&, uintptr_t game);
    void (*visible)(void*, bool);
    void (*enabled)(void*, bool, uintptr_t game);
    bool (*get_text)(void*, std::string&);
    void (*set_text)(void*, const std::string&, uintptr_t game);
    bool (*enable_control)(uint32_t kind) = nullptr;
    void* (*create_control)(const swml_ui_control_desc&) = nullptr;
    // Raw bool/number text, or unquoted editbox text. -1 unsupported, 0
    // unchanged, 1 changed; reuse the supplied string without allocating on no change.
    int (*control_value)(void*, std::string&) = nullptr;
    void (*control_set)(void*, const std::string&, uintptr_t game) = nullptr;
    void (*control_configure)(void*, const swml_ui_control_desc&, uintptr_t game) = nullptr;
};
const NativeUiBackend& native_ui_backend();

namespace sdk_ui {
void set_backend_for_tests(const NativeUiBackend*); // before enabling; never used by shipped core
void attach(swml_mod*);
void drop(swml_mod*);
bool enabled();
swml_result enable(swml_mod*);
swml_result watch(swml_mod*, const char*, swml_ui_callback, void*, swml_handle*);
swml_result watch_rect(swml_mod*, const char*, const swml_ui_rect*, swml_ui_callback, void*, swml_handle*);
swml_result unwatch(swml_mod*, swml_handle);
swml_result create(swml_mod*, swml_ui_handle, const swml_ui_desc*, swml_ui_handle*);
swml_result destroy(swml_mod*, swml_ui_handle);
swml_result children(swml_mod*, swml_ui_handle, swml_ui_handle*, uint32_t, uint32_t*);
swml_result find_class(swml_mod*, swml_ui_handle, const char*, uint32_t, swml_ui_handle*);
swml_result inspect(swml_mod*, swml_ui_handle, swml_ui_info*);
swml_result set_rect(swml_mod*, swml_ui_handle, const swml_ui_rect*);
swml_result set_visible(swml_mod*, swml_ui_handle, int);
swml_result set_enabled(swml_mod*, swml_ui_handle, int);
swml_result set_text(swml_mod*, swml_ui_handle, const char*);
swml_result get_text(swml_mod*, swml_ui_handle, char*, uint32_t, uint32_t*);
swml_result create_control(swml_mod*, swml_ui_handle, const swml_ui_control_desc*, swml_ui_handle*);
swml_result value_get(swml_mod*, swml_ui_handle, char*, uint32_t, uint32_t*);
swml_result value_set(swml_mod*, swml_ui_handle, const char*);
bool controls_enabled();
// Called by the native backend; UI root records and generation handles are invalidated on teardown.
void update(void*, uintptr_t game);
void deleting(void*);
void pressed(void*);
}
