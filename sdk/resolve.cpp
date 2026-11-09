#include "resolve.h"
#include "log.h"

#include <windows.h>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

GameAddrs g_addrs;
std::mutex g_resolve_mutex;

struct Section {
    uintptr_t begin = 0, end = 0;
};
uintptr_t g_base = 0;
Section g_text, g_rdata, g_data;

bool load_sections()
{
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        char name[9] = {};
        memcpy(name, sec->Name, 8);
        Section s{g_base + sec->VirtualAddress, g_base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (strcmp(name, ".text") == 0) g_text = s;
        else if (strcmp(name, ".rdata") == 0) g_rdata = s;
        else if (strcmp(name, ".data") == 0) g_data = s;
    }
    return g_text.begin && g_rdata.begin && g_data.begin;
}

bool in(const Section& s, uintptr_t p, size_t n = 1) { return p >= s.begin && p + n <= s.end; }

// "83 BE ?? ?? ?? ?? 01" -> bytes + mask.
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
};
Pattern parse(const char* text)
{
    Pattern p;
    for (const char* c = text; *c;) {
        if (*c == ' ') { c++; continue; }
        if (c[0] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            c += (c[1] == '?') ? 2 : 1;
        } else {
            p.bytes.push_back(static_cast<uint8_t>(strtoul(std::string(c, 2).c_str(), nullptr, 16)));
            p.mask.push_back(true);
            c += 2;
        }
    }
    return p;
}

uintptr_t find(uintptr_t from, uintptr_t to, const Pattern& p)
{
    size_t n = p.bytes.size();
    for (uintptr_t a = from; a + n <= to; a++) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(a);
        size_t i = 0;
        while (i < n && (!p.mask[i] || m[i] == p.bytes[i])) i++;
        if (i == n) return a;
    }
    return 0;
}

uint32_t rd32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }
uintptr_t rel32_target(uintptr_t field, uintptr_t insn_end) { return insn_end + static_cast<int32_t>(rd32(field)); }

// Function bounds from the exe's x64 unwind table (.pdata).
bool function_range(uintptr_t addr, uintptr_t* begin, uintptr_t* end)
{
    DWORD64 image = 0;
    auto rf = RtlLookupFunctionEntry(addr, &image, nullptr);
    if (!rf) return false;
    *begin = static_cast<uintptr_t>(image + rf->BeginAddress);
    *end = static_cast<uintptr_t>(image + rf->EndAddress);
    return true;
}

// Start of the function containing addr. MSVC splits some functions into several chunks, each with
// its own .pdata entry; a chunk's unwind info is "chained" to its parent's entry, so follow the chain.
uintptr_t function_start(uintptr_t addr)
{
    DWORD64 image = 0;
    auto rf = RtlLookupFunctionEntry(addr, &image, nullptr);
    for (int depth = 0; rf && depth < 16; depth++) {
        uintptr_t uw = static_cast<uintptr_t>(image + (rf->UnwindData & ~1u));
        if (!in(g_rdata, uw, 4)) return 0; // unwind info always lives in .rdata
        uint8_t flags = *reinterpret_cast<const uint8_t*>(uw) >> 3;
        if (!(flags & UNW_FLAG_CHAININFO)) return static_cast<uintptr_t>(image + rf->BeginAddress);
        uint8_t codes = *reinterpret_cast<const uint8_t*>(uw + 2);
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(uw + 4 + ((codes + 1u) & ~1u) * 2);
    }
    return 0;
}

uintptr_t vtable_for(const char* class_name)
{
    for (const char* prefix : {".?AV", ".?AU"}) {
        std::string mangled = std::string(prefix) + class_name + "@@";
        // TypeDescriptor = { vftable*, spare, char name[] } lives in .data.
        Pattern p;
        for (char ch : mangled) { p.bytes.push_back(static_cast<uint8_t>(ch)); p.mask.push_back(true); }
        p.bytes.push_back(0); p.mask.push_back(true);
        uintptr_t name = find(g_data.begin, g_data.end, p);
        if (!name) continue;
        uint32_t td_rva = static_cast<uint32_t>(name - 0x10 - g_base);
        // CompleteObjectLocator (x64) in .rdata: signature 1, offset 0, cdOffset, pTypeDescriptor, pClassDescriptor, pSelf.
        for (uintptr_t a = g_rdata.begin; a + 24 <= g_rdata.end; a += 4) {
            if (rd32(a) != 1 || rd32(a + 4) != 0 || rd32(a + 12) != td_rva) continue;
            if (rd32(a + 20) != static_cast<uint32_t>(a - g_base)) continue;
            for (uintptr_t v = g_rdata.begin; v + 16 <= g_rdata.end; v += 8) {
                if (*reinterpret_cast<const uintptr_t*>(v) == a) return v + 8;
            }
        }
    }
    return 0;
}

// --- keybind table -----------------------------------------------------------------------
struct GameString {
    const char* data;
    uint32_t len;
    uint32_t cap;
};
constexpr uint32_t kEntryStride = 0x58, kEntryKey = 0x28, kEntryDefaultKey = 0x2c, kMaxEntries = 256;
constexpr uint32_t kDefaultRotateXKey = 0x1f; // 'j' in the game's key code enum (keynames.h)

int64_t find_action(const uint8_t* table, const char* name)
{
    size_t want = strlen(name);
    __try {
        for (uint32_t i = 0; i < kMaxEntries; i++) {
            auto s = reinterpret_cast<const GameString*>(table + i * kEntryStride);
            if (!s->data || s->len > 64) continue;
            if (s->len == want && memcmp(s->data, name, want) == 0) return static_cast<int64_t>(i) * kEntryStride;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

using Getter_t = uint8_t*(__fastcall*)();
uint8_t* call_getter(uintptr_t f)
{
    __try {
        return reinterpret_cast<Getter_t>(f)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// A small function using MSVC's thread-safe static init (reads the TLS block via gs:[0x58]
// near its start) - the shape of the keybind table getter, and safe to call with no arguments.
bool looks_like_static_getter(uintptr_t f)
{
    static const Pattern tls = parse("65 48 8B 04 25 58 00 00 00");
    uintptr_t b, e;
    if (!function_range(f, &b, &e) || b != f || e - b > 0x100) return false;
    return find(f, f + 0x20, tls) != 0;
}

bool fail(const char* what)
{
    swlog("resolve: FAILED %s - editor features disabled for this game version", what);
    return false;
}

bool resolve_editor()
{
    GameAddrs& r = g_addrs;
    r.editor_vtable = vtable_for("c_game_state_vehicle_editor");
    if (!r.editor_vtable) return fail("RTTI vtable of c_game_state_vehicle_editor");

    const Pattern placing = parse("83 BE ?? ?? ?? ?? 01 0F 85 ?? ?? ?? ?? 83 BE ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? "
                                  "80 BE ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? C6 86 ?? ?? ?? ?? 01 48 8B 86 ?? ?? ?? ??");
    const Pattern layout_call = parse("48 8D 96 ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 7E ?? "
                                      "48 89 44 24 20 4C 8B CF 4C 8D 86 ?? ?? ?? ?? 49 8B D5 48 8B CB E8 ?? ?? ?? ??");
    const Pattern project_head = parse("48 83 EC 18 F2 41 0F 10 18 48 8B C2 F2 41 0F 10 60 10 0F 28 C3 F2 0F 59 81 ?? ?? ?? ??");

    uintptr_t best_handler = 0, best_target = 0;
    int best_count = 0;
    for (int slot = 0; slot < 96; slot++) {
        uintptr_t f = *reinterpret_cast<const uintptr_t*>(r.editor_vtable + slot * 8);
        if (!in(g_text, f)) break;
        uintptr_t b, e;
        if (!function_range(f, &b, &e)) continue;

        if (!r.update_fn) {
            if (uintptr_t hit = find(b, e, placing)) {
                r.update_fn = f;
                r.off_mode_a = rd32(hit + 2);
                r.off_mode_b = rd32(hit + 15);
                r.off_overlay = rd32(hit + 49);
                r.off_hints_flag = rd32(hit + 41);
                r.off_hints_setting = rd32(hit + 28);
                if (uintptr_t lc = find(hit, e, layout_call)) {
                    r.off_transform = rd32(lc + 3);
                    r.off_camera = *reinterpret_cast<const uint8_t*>(lc + 22);
                    r.layout_fn = rel32_target(lc + 45, lc + 49);
                }
            }
        }
        // Key handler: the function making by far the most calls to one target (the keybind getter).
        std::map<uintptr_t, int> calls;
        for (uintptr_t a = b; a + 5 <= e; a++) {
            if (*reinterpret_cast<const uint8_t*>(a) != 0xE8) continue;
            uintptr_t t = rel32_target(a + 1, a + 5);
            if (in(g_text, t)) calls[t]++;
        }
        for (auto& [t, n] : calls) {
            if (n <= best_count || !looks_like_static_getter(t)) continue;
            best_count = n;
            best_target = t;
            best_handler = f;
        }
    }
    if (!r.update_fn) return fail("editor update pattern (placing-a-part check)");
    if (!r.layout_fn) return fail("editor update pattern (overlay layout call)");
    if (best_count < 15) return fail("editor key handler (no function with repeated keybind lookups)");

    // Verify the getter semantically: it must return a table containing the rotate actions.
    uint8_t* table = call_getter(best_target);
    if (!table) return fail("keybind table getter call");
    const char* names[3] = {"action_editor_rotate_x", "action_editor_rotate_y", "action_editor_rotate_z"};
    for (int i = 0; i < 3; i++) {
        int64_t off = find_action(table, names[i]);
        if (off < 0) return fail(names[i]);
        r.key_rotate[i] = static_cast<uint32_t>(off) + kEntryKey;
    }
    const char* pair_names[6] = {"action_up",   "action_down",          "action_left",
                                 "action_right", "action_interact_left", "action_interact_right"};
    for (int i = 0; i < 6; i++) {
        int64_t off = find_action(table, pair_names[i]);
        if (off < 0) return fail(pair_names[i]);
        r.key_pairs[i] = static_cast<uint32_t>(off) + kEntryKey;
    }
    int64_t show = find_action(table, "action_editor_show_rotation_ui");
    r.key_show_rotation = show < 0 ? 0 : static_cast<uint32_t>(show) + kEntryKey;
    uint32_t default_x = *reinterpret_cast<const uint32_t*>(table + r.key_rotate[0] - kEntryKey + kEntryDefaultKey);
    if (default_x != kDefaultRotateXKey) return fail("key code check (rotate X default is no longer 'j')");
    r.key_handler = best_handler;
    r.keybind_getter = best_target;

    // World->screen: find the projection function among the layout function's calls.
    uintptr_t lb, le;
    if (!function_range(r.layout_fn, &lb, &le)) return fail("layout function bounds");
    for (uintptr_t a = lb; a + 5 <= le && !r.project_fn; a++) {
        if (*reinterpret_cast<const uint8_t*>(a) != 0xE8) continue;
        uintptr_t t = rel32_target(a + 1, a + 5);
        if (in(g_text, t, project_head.bytes.size()) && find(t, t + project_head.bytes.size(), project_head) == t) {
            r.project_fn = t;
            r.off_viewproj = rd32(t + 25) - 3 * 8; // first multiply reads matrix element [col 0][row 3]
        }
    }
    if (!r.project_fn) return fail("world-to-screen function pattern");

    r.editor_ok = true;
    return true;
}

bool resolve_physics()
{
    GameAddrs& r = g_addrs;
    uintptr_t vt = vtable_for("update_thread_task@c_scene");
    if (!vt) {
        swlog("resolve: FAILED RTTI vtable of c_scene::update_thread_task - physics features disabled");
        return false;
    }
    uintptr_t f = *reinterpret_cast<const uintptr_t*>(vt + 8);
    uintptr_t b, e;
    if (!in(g_text, f) || !function_range(f, &b, &e)) {
        swlog("resolve: FAILED physics task function bounds - physics features disabled");
        return false;
    }
    const Pattern rate = parse("F3 0F 5E 83 ?? ?? ?? ??");
    const Pattern step = parse("41 B8 E8 03 00 00");
    const Pattern call = parse("FF 50 60");
    uintptr_t hr = find(b, e, rate), hs = find(b, e, step), hc = find(b, e, call);
    if (!hr || !hs || !hc || !(hr < hs && hs < hc)) {
        swlog("resolve: FAILED physics task pattern (rate / stepSimulation call) - physics features disabled");
        return false;
    }
    r.physics_task_fn = f;
    r.off_phys_rate = rd32(hr + 4);
    r.physics_ok = true;
    return true;
}

bool resolve_update_loop()
{
    GameAddrs& r = g_addrs;
    uintptr_t vt = vtable_for("update_thread@c_application_state_game");
    if (!vt) {
        swlog("resolve: FAILED RTTI vtable of c_application_state_game::update_thread - update loop fix disabled");
        return false;
    }
    uintptr_t f = *reinterpret_cast<const uintptr_t*>(vt + 8);
    uintptr_t b, e;
    if (!in(g_text, f) || !function_range(f, &b, &e)) {
        swlog("resolve: FAILED update loop function bounds - update loop fix disabled");
        return false;
    }
    // Spin: CALL clock; SUB EAX,<start>; CMP EAX,0x10; JB <back to the CALL>
    const Pattern spin = parse("E8 ?? ?? ?? ?? 2B ?? 83 F8 10 72 ??");
    uintptr_t hs = find(b, e, spin);
    const uint8_t* sb = reinterpret_cast<const uint8_t*>(hs);
    if (!hs || (sb[6] & 0xF8) != 0xC0 || hs + 12 + static_cast<int8_t>(sb[11]) != hs) {
        swlog("resolve: FAILED update loop spin pattern - update loop fix disabled");
        return false;
    }
    uintptr_t clock = rel32_target(hs + 1, hs + 5);
    // Loop start: the first call to the same clock function.
    uintptr_t hf = 0;
    for (uintptr_t a = b; a + 5 <= hs && !hf; a++) {
        if (*reinterpret_cast<const uint8_t*>(a) == 0xE8 && rel32_target(a + 1, a + 5) == clock) hf = a;
    }
    if (!in(g_text, clock) || !hf || hf == hs) {
        swlog("resolve: FAILED update loop start clock call - update loop fix disabled");
        return false;
    }
    r.update_loop_fn = f;
    r.clock_fn = clock;
    r.ret_loop_start = hf + 5;
    r.ret_loop_spin = hs + 5;
    r.update_loop_ok = true;
    return true;
}

bool resolve_scope()
{
    GameAddrs& r = g_addrs;
    auto fail = [](const char* what) {
        swlog("resolve: FAILED %s - scope look disabled", what);
        return false;
    };
    uintptr_t vt = vtable_for("c_scene_vehicle_component_scope");
    if (!vt) return fail("RTTI vtable of c_scene_vehicle_component_scope");
    r.slot_f32_vtable = vtable_for("c_vehicle_logic_slot_output_f32");
    if (!r.slot_f32_vtable) return fail("RTTI vtable of c_vehicle_logic_slot_output_f32");
    r.slot_composite_vtable = vtable_for("c_vehicle_logic_slot_output_composite");
    if (!r.slot_composite_vtable) return fail("RTTI vtable of c_vehicle_logic_slot_output_composite");

    // CMP qword [RCX+occupant],0; SETNZ AL; MOV [RCX+occupied],AL; RET
    const Pattern tick = parse("48 83 B9 ?? ?? ?? ?? 00 0F 95 C0 88 81 ?? ?? ?? ?? C3");
    // LEA RBX,[RCX+slots]; MOV RSI,RCX; MOV RCX,RBX; CALL <clear lists>
    const Pattern reg = parse("48 8D 99 ?? ?? ?? ?? 48 8B F1 48 8B CB E8");
    // CMOVC EDX,R14D; CALL <grow list>
    const Pattern grow = parse("41 0F 42 D6 E8 ?? ?? ?? ??");
    const Pattern setup = parse("48 8B 49 ?? 44 8D 4A 04 44 8D 42 01 E8 ?? ?? ?? ??");
    // Pass 1: the tick function gives the occupant / Occupied offsets. Pass 2: the registration and
    // setup functions must also reference the Occupied slot (inherited base-class functions don't).
    auto contains_u32 = [](uintptr_t b, uintptr_t e, uint32_t v) {
        for (uintptr_t a = b; a + 4 <= e; a++)
            if (rd32(a) == v) return true;
        return false;
    };
    for (int pass = 0; pass < 2; pass++) {
        for (int slot = 0; slot < 200; slot++) {
            uintptr_t f = *reinterpret_cast<const uintptr_t*>(vt + slot * 8);
            if (!in(g_text, f)) break;
            if (pass == 0) {
                // A leaf function with no stack frame: it has no .pdata entry, so match at f directly.
                if (!r.scope_tick_fn && in(g_text, f, tick.bytes.size()) && find(f, f + tick.bytes.size(), tick) == f) {
                    r.scope_tick_fn = f;
                    r.off_scope_occupant = rd32(f + 3);
                    r.off_scope_occupied = rd32(f + 13);
                }
                continue;
            }
            uintptr_t b, e;
            if (!function_range(f, &b, &e)) continue;
            if (!r.scope_tick_fn) break;
            const uint32_t occ_slot = r.off_scope_occupied - 0x18, occ_handle = r.off_scope_occupied - 0x08;
            if (!r.scope_register_fn) {
                uintptr_t h = find(b, e, reg);
                uintptr_t g = h ? find(h, e, grow) : 0;
                if (g && contains_u32(b, e, occ_slot)) {
                    r.scope_register_fn = f;
                    r.off_scope_slots = rd32(h + 3);
                    r.list_grow_fn = rel32_target(g + 5, g + 9);
                    continue;
                }
            }
            if (!r.scope_setup_fn) {
                uintptr_t h = find(b, e, setup);
                if (h && contains_u32(b, e, occ_handle)) {
                    r.scope_setup_fn = f;
                    r.off_scope_def = *reinterpret_cast<const uint8_t*>(h + 3);
                    r.node_lookup_fn = rel32_target(h + 13, h + 17);
                }
            }
        }
    }
    if (!r.scope_tick_fn) return fail("scope tick function (occupied output pattern)");
    if (!r.scope_register_fn) return fail("scope slot registration function");
    if (!r.scope_setup_fn) return fail("scope node setup function");
    if (!in(g_text, r.list_grow_fn) || !in(g_text, r.node_lookup_fn)) return fail("scope helper call targets");
    // Translation lookup: walks a table of 0x30-byte {key, english, translated} entries.
    const Pattern translate = parse("48 8B F2 48 8B F9 33 ED 89 6C 24 20 8B DD E8 ?? ?? ?? ?? 39 A8 ?? ?? ?? ?? 76 ?? E8 ?? ?? ?? ?? "
                                    "4C 8B C0 8B 80 ?? ?? ?? ?? 03 C3 33 D2 41 F7 B0 ?? ?? ?? ?? 48 8D 0C 52 48 C1 E1 04");
    uintptr_t th = find(g_text.begin, g_text.end, translate);
    uintptr_t tb = th ? function_start(th) : 0;
    if (!th || find(th + 1, g_text.end, translate) || !tb) return fail("translation lookup pattern");
    r.translate_fn = tb;
    r.scope_ok = true;
    return true;
}

bool resolve_seat()
{
    GameAddrs& r = g_addrs;
    const Pattern copy = parse("F3 0F 10 82 ?? ?? ?? ?? F3 0F 10 8A ?? ?? ?? ?? 48 83 BA ?? ?? ?? ?? 00 "
                               "F3 0F 11 82 ?? ?? ?? ?? F3 0F 11 8A ?? ?? ?? ?? F3 0F 11 82 ?? ?? ?? ?? F3 0F 11 8A ?? ?? ?? ??");
    uintptr_t h = find(g_text.begin, g_text.end, copy);
    uintptr_t b = h ? function_start(h) : 0;
    if (!h || find(h + 1, g_text.end, copy) || !b) {
        swlog("resolve: FAILED seat look output pattern - scope look disabled");
        return false;
    }
    uint32_t look = rd32(h + 4), out = rd32(h + 28);
    if (rd32(h + 12) != look + 4 || rd32(h + 36) != out + 0x38) {
        swlog("resolve: FAILED seat look output layout - scope look disabled");
        return false;
    }
    r.seat_outputs_fn = b;
    r.off_seat_look = look;
    r.off_seat_look_out = out;
    r.seat_ok = true;
    return true;
}

} // namespace

bool resolve(unsigned groups)
{
    std::lock_guard lock(g_resolve_mutex);
    if (!g_text.begin && !load_sections()) {
        swlog("resolve: could not read exe sections");
        return false;
    }
    const GameAddrs& r = g_addrs;
    auto ghidra = [](uintptr_t p) { return p ? static_cast<unsigned long long>(p - g_base + 0x140000000) : 0ull; };
    bool ok = true;
    if (groups & kResolveEditor) {
        bool e = resolve_editor();
        ok &= e;
        swlog("resolve: editor vtable=%llx update=%llx keyhandler=%llx getter=%llx layout=%llx project=%llx", ghidra(r.editor_vtable),
              ghidra(r.update_fn), ghidra(r.key_handler), ghidra(r.keybind_getter), ghidra(r.layout_fn), ghidra(r.project_fn));
        swlog("resolve: offsets mode=%x/%x overlay=%x setting=%x hints=%x camera=%x transform=%x viewproj=%x keys=%x/%x/%x show=%x -> %s",
              r.off_mode_a, r.off_mode_b, r.off_overlay, r.off_hints_setting, r.off_hints_flag, r.off_camera, r.off_transform,
              r.off_viewproj, r.key_rotate[0], r.key_rotate[1], r.key_rotate[2], r.key_show_rotation, e ? "OK" : "incomplete");
    }
    if (groups & kResolvePhysics) {
        ok &= resolve_physics();
        swlog("resolve: physics task=%llx rate offset=%x -> %s", ghidra(r.physics_task_fn), r.off_phys_rate, r.physics_ok ? "OK" : "FAILED");
    }
    if (groups & kResolveUpdateLoop) {
        ok &= resolve_update_loop();
        swlog("resolve: update loop=%llx clock=%llx start call ret=%llx spin call ret=%llx -> %s", ghidra(r.update_loop_fn),
              ghidra(r.clock_fn), ghidra(r.ret_loop_start), ghidra(r.ret_loop_spin), r.update_loop_ok ? "OK" : "FAILED");
    }
    if (groups & kResolveScope) {
        ok &= resolve_scope();
        swlog("resolve: scope translate=%llx tick=%llx register=%llx setup=%llx lookup=%llx grow=%llx f32vt=%llx occupant=%x occupied=%x def=%x slots=%x -> %s",
              ghidra(r.translate_fn), ghidra(r.scope_tick_fn), ghidra(r.scope_register_fn), ghidra(r.scope_setup_fn), ghidra(r.node_lookup_fn),
              ghidra(r.list_grow_fn), ghidra(r.slot_f32_vtable), r.off_scope_occupant, r.off_scope_occupied, r.off_scope_def, r.off_scope_slots,
              r.scope_ok ? "OK" : "FAILED");
    }
    if (groups & kResolveSeat) {
        ok &= resolve_seat();
        swlog("resolve: seat outputs=%llx look=%x look out=%x -> %s", ghidra(r.seat_outputs_fn), r.off_seat_look, r.off_seat_look_out,
              r.seat_ok ? "OK" : "FAILED");
    }
    return ok;
}

const GameAddrs& addrs() { return g_addrs; }

uintptr_t native_find_pattern(const char* text, const char* section)
{
    if (!text || !section || strnlen_s(text, 4096) >= 4096) return 0;
    // Validate before handing a pattern to the original internal parser.
    auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); };
    size_t count = 0;
    for (const char* p = text; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { ++p; if (*p == '?') ++p; }
        else { if (!hex(p[0]) || !p[1] || !hex(p[1])) return 0; p += 2; }
        if (*p && *p != ' ') return 0;
        if (++count > 1024) return 0;
    }
    if (!count) return 0;
    std::lock_guard lock(g_resolve_mutex);
    if (!g_text.begin && !load_sections()) return 0;
    const Section* range = strcmp(section, ".text") == 0 ? &g_text :
                           strcmp(section, ".rdata") == 0 ? &g_rdata : strcmp(section, ".data") == 0 ? &g_data : nullptr;
    if (!range) return 0;
    Pattern pattern = parse(text);
    uintptr_t found = find(range->begin, range->end, pattern);
    return found && !find(found + 1, range->end, pattern) ? found : 0;
}

uintptr_t native_find_vtable(const char* class_name)
{
    if (!class_name || !*class_name || strnlen_s(class_name, 256) >= 256) return 0;
    std::lock_guard lock(g_resolve_mutex);
    if (!g_text.begin && !load_sections()) return 0;
    return vtable_for(class_name);
}

bool native_function_bounds(uintptr_t address, uintptr_t* begin, uintptr_t* end)
{
    if (begin) *begin = 0;
    if (end) *end = 0;
    return address && begin && end && function_range(address, begin, end);
}

bool native_object_is_a(const void* object, const char* class_name)
{
    if (!object || !class_name || !*class_name || strnlen_s(class_name, 256) >= 256) return false;
    __try {
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        uintptr_t limit = base + nt->OptionalHeader.SizeOfImage;
        uintptr_t table = *static_cast<const uintptr_t*>(object);
        if (table < base + 8 || table >= limit) return false;
        uintptr_t col = *reinterpret_cast<const uintptr_t*>(table - 8);
        if (col < base || col > limit - 24 || rd32(col) != 1 || base + rd32(col + 20) != col) return false;
        uintptr_t hierarchy = base + rd32(col + 16);
        if (hierarchy < base || hierarchy > limit - 16) return false;
        uint32_t count = rd32(hierarchy + 8);
        uintptr_t array = base + rd32(hierarchy + 12);
        if (!count || count > 256 || array < base || array > limit - count * 4) return false;
        const size_t name_length = strlen(class_name);
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t descriptor = base + rd32(array + i * 4);
            if (descriptor < base || descriptor > limit - 4) return false;
            uintptr_t type = base + rd32(descriptor);
            if (type < base || type > limit - 16 - name_length - 7) return false;
            const char* name = reinterpret_cast<const char*>(type + 16);
            if (!memcmp(name, ".?A", 3) && (name[3] == 'V' || name[3] == 'U') &&
                !memcmp(name + 4, class_name, name_length) && !memcmp(name + 4 + name_length, "@@", 3)) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}
