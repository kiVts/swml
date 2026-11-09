#pragma once
#include <cstdint>


struct GameAddrs {
    bool editor_ok = false; // everything below resolved and verified

    uintptr_t editor_vtable = 0; // c_game_state_vehicle_editor::vftable
    uintptr_t key_handler = 0;
    uintptr_t update_fn = 0;
    uintptr_t keybind_getter = 0;
    uintptr_t layout_fn = 0;     // rotation-labels overlay layout (called from update)
    uintptr_t project_fn = 0;    // world -> screen (called from layout)

    // Editor object offsets.
    uint32_t off_mode_a = 0, off_mode_b = 0; // "placing a part" when *(int*)a == 1 && *(int*)b == 0
    uint32_t off_overlay = 0;                // c_vehicle_editor_rotation_labels_ui*
    uint32_t off_hints_setting = 0;          // byte: "Show Rotation UI" (gates the placing branch)
    uint32_t off_hints_flag = 0;             // byte: rotation hints active (makes the editor draw axis lines)
    uint32_t off_camera = 0;                 // camera object
    uint32_t off_transform = 0;              // part transform (4x4 doubles)
    uint32_t off_viewproj = 0;               // inside the camera: 4x4 double view-projection

    // Keybind table: offsets of the key field for rotate X/Y/Z.
    uint32_t key_rotate[3] = {0, 0, 0};
    // Keybind table: key fields of the binds that drive hold-to-rotate, in pairs per axis:
    // [0,1] Forward/Backward (action_up/down) -> X, [2,3] Left/Right -> Y,
    // [4,5] Interact Left/Right -> Z.
    uint32_t key_pairs[6] = {0, 0, 0, 0, 0, 0};
    // Keybind table: key field of "Show Rotation UI" (action_editor_show_rotation_ui), used as the
    // hold key for the rotation rings. 0 if the action wasn't found.
    uint32_t key_show_rotation = 0;

    bool physics_ok = false;
    uintptr_t physics_task_fn = 0;
    uint32_t off_phys_rate = 0; // float, Hz, inside the task object

    bool update_loop_ok = false;
    uintptr_t update_loop_fn = 0;
    uintptr_t clock_fn = 0;       // int ms clock (QueryPerformanceCounter based)
    uintptr_t ret_loop_start = 0; // return address of the loop's first clock call
    uintptr_t ret_loop_spin = 0;  // return address of the clock call inside the spin

    // Viewing scope (c_scene_vehicle_component_scope) and seat look outputs (scope Look X/Y feature).
    bool scope_ok = false;
    uintptr_t scope_tick_fn = 0;     // per-tick output update: Occupied = (occupant != null)
    uintptr_t scope_register_fn = 0; // registers the scope's logic slots into its slot list
    uintptr_t scope_setup_fn = 0;    // looks up the scope's logic nodes in its definition
    uintptr_t node_lookup_fn = 0;    // (definition, index, mode, type, 0, 0) -> logic node handle
    uintptr_t list_grow_fn = 0;      // grows a game ring list (list, new capacity)
    uintptr_t slot_f32_vtable = 0;   // c_vehicle_logic_slot_output_f32::vftable (0x38-byte number output slot)
    uintptr_t slot_composite_vtable = 0; // c_vehicle_logic_slot_output_composite::vftable (0xb8-byte composite output slot)
    uint32_t off_scope_occupant = 0; // character*
    uint32_t off_scope_occupied = 0; // bool output value (inside its output slot, slot = value - 0x18)
    uint32_t off_scope_def = 0;      // definition* passed to node_lookup_fn
    uint32_t off_scope_slots = 0;    // ring list of logic slot pointers
    uintptr_t translate_fn = 0;      // (out string*, key string*): key -> UI text (key itself if unknown)
    bool seat_ok = false;
    uintptr_t seat_outputs_fn = 0;   // per-tick: copies look (and axes) into the seat's outputs
    uint32_t off_seat_look = 0;      // float look x, y follows (turns)
    uint32_t off_seat_look_out = 0;
};

enum : unsigned {
    kResolveEditor = 1,
    kResolvePhysics = 2,
    kResolveUpdateLoop = 4,
    kResolveScope = 8,
    kResolveSeat = 16,
};
bool resolve(unsigned groups); // resolves (and logs) the given groups; true if all succeeded
const GameAddrs& addrs(); // results

// SDK reusable native discovery. Startup/explicit feature activation only: scanning is expensive.
// Patterns are space-separated hex bytes or ?/??. No result if malformed, absent or ambiguous.
// section is one of .text/.rdata/.data. Results refer to the current executable, not another DLL.
uintptr_t native_find_pattern(const char* pattern, const char* section = ".text");
uintptr_t native_find_vtable(const char* class_name);
bool native_function_bounds(uintptr_t address, uintptr_t* begin, uintptr_t* end); // false for leaf code without unwind metadata
bool native_object_is_a(const void* object, const char* class_name);
