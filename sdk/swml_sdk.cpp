// Mod-side runtime, linked into every mod DLL: the swml_mod_init export, and the familiar helpers
// (swlog, hook_*, present_add_callback, ...) implemented on top of the core's swml_api table.

#include "game.h"
#include "hooks.h"
#include "log.h"
#include "port_fields.h"
#include "port_runtime.h"
#include "port_sigs.h"
#include "port_table.h"
#include "present.h"
#include "sdk.h"
#include "status_line.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
const swml_api* g_api = nullptr;
swml_mod* g_mod = nullptr;

std::vector<HookDef>& registry()
{
    static std::vector<HookDef> defs; // function-local: safe regardless of static init order
    return defs;
}
std::vector<bool (*)()>& setups()
{
    static std::vector<bool (*)()> fns;
    return fns;
}

void present_trampoline(void* hdc, void* ctx) { reinterpret_cast<PresentCallback>(ctx)(static_cast<HDC>(hdc)); }
} // namespace

const swml_api* swml() { return g_api; }
swml_mod* swml_self() { return g_mod; }

HookRegistrar::HookRegistrar(const HookDef& def) { registry().push_back(def); }
ModSetup::ModSetup(bool (*fn)()) { setups().push_back(fn); }

void swlog(const char* fmt, ...)
{
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (g_api) g_api->log(g_mod, msg);
}

void mod_status(const char* fmt, ...)
{
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (g_api) g_api->set_status(g_mod, msg);
}

const wchar_t* log_dir() { return g_api ? g_api->logs_dir() : L"."; }
const wchar_t* settings_ini() { return g_api ? g_api->settings_path(g_mod) : L"settings.ini"; }
const wchar_t* mod_dir() { return g_api ? g_api->mod_dir(g_mod) : L"."; }

uintptr_t game_base() { return g_api ? g_api->game_base() : 0; }

namespace {
constexpr uint32_t kOriginalBuild = kPortFromBuild;
uint32_t g_ported_to = 0;
std::atomic<int> g_port_misses{0}; // game_live()/game_field() calls that found nothing (a part using one is switched off)

// One part's install(), with a crash (e.g. reading through an unmapped address) turned into a failed part instead of
// the whole mod. No C++ objects in this frame, so SEH is allowed here.
enum InstallResult { kInstalled, kFailed, kCrashed };
InstallResult run_install(bool (*install)())
{
    __try {
        return install() ? kInstalled : kFailed;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kCrashed;
    }
}

uint32_t read_build(uint32_t* size_of_image)
{
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game_base());
    if (!dos) return 0;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(game_base() + dos->e_lfanew);
    if (size_of_image) *size_of_image = nt->OptionalHeader.SizeOfImage;
    return nt->FileHeader.TimeDateStamp;
}

// swml.ini [port] force_signatures=1: use the signatures even on the port table's build (tests the start-up path)
bool force_signatures()
{
    std::wstring ini = log_dir();
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\swml.ini";
    return GetPrivateProfileIntW(L"port", L"force_signatures", 0, ini.c_str()) != 0;
}

bool is_table_build()
{
    static const bool table = [] {
        uint32_t size = 0;
        return read_build(&size) == kPortToBuild && size == kPortToSize && !force_signatures();
    }();
    return table;
}

// The game exe mapped again from disk (port_runtime.h): the bytes before any mod's hooks patched them (the mods load
// one after another, so the live image has the earlier mods' detours in it).
uintptr_t signature_image()
{
    static const uintptr_t image = [] {
        if (uintptr_t p = port_pristine_image(game_base())) return p;
        swlog("port: could not map the game exe from disk; signatures read the live image (other mods' hooks may hide some)");
        return game_base();
    }();
    return image;
}

// --- where the port data comes from ---------------------------------------------------------------------------
// The core (swml_api port_*) resolves the port data it was built with once for every mod. This mod uses it when it is
// newer than its own: a loader released for a new game build then ports mods built before that build. Its own data
// stays the fallback (older core, or an address/field the core's data doesn't know).
const swml_port_info* core_port()
{
    static swml_port_info info{};
    static const bool ok = [] {
        if (!SWML_API_HAS_PORT(g_api)) return false;
        info.size = sizeof(info);
        return g_api->port_info(&info) && info.original_build == kPortFromBuild;
    }();
    return ok ? &info : nullptr;
}

bool use_core_port()
{
    static const bool use = [] {
        const swml_port_info* c = core_port();
        if (!c || is_table_build()) return false; // this mod's own table is exact for the running build
        return c->table_build == read_build(nullptr) || c->sig_build >= kSigBuild;
    }();
    return use;
}

std::vector<swml_port_range> core_ranges(int native)
{
    std::vector<swml_port_range> v(g_api->port_ranges(native, nullptr, 0));
    v.resize(g_api->port_ranges(native, v.data(), v.size()));
    return v;
}

std::vector<PortRange> found_only(const std::vector<swml_port_range>& v)
{
    std::vector<PortRange> out;
    for (const swml_port_range& r : v)
        if (r.new_rva != ~0u) out.push_back({r.old_rva, r.size, r.new_rva});
    return out;
}

const std::vector<PortRange>& own_ranges()
{
    static const std::vector<PortRange> v = [] {
        if (is_table_build()) return std::vector<PortRange>(std::begin(kPortRanges), std::end(kPortRanges));
        return port_resolve_signatures(signature_image(), false);
    }();
    return v;
}

// Every range key the core's data has (found or not): an address outside all of them is unknown to the core.
const std::vector<swml_port_range>& core_keys()
{
    static const std::vector<swml_port_range> v = use_core_port() ? core_ranges(0) : std::vector<swml_port_range>{};
    return v;
}

// The ranges in use.
const std::vector<PortRange>& ranges()
{
    static const std::vector<PortRange> v = use_core_port() ? found_only(core_keys()) : own_ranges();
    return v;
}

const std::vector<PortRange>& by_new()
{
    static const std::vector<PortRange> v = [] {
        std::vector<PortRange> r = ranges();
        std::sort(r.begin(), r.end(), [](const PortRange& a, const PortRange& b) { return a.new_rva < b.new_rva; });
        return r;
    }();
    return v;
}

// Ranges are few (one per address the sources name); the smallest containing range wins.
template <typename R>
const R* containing(const std::vector<R>& v, uint32_t rva)
{
    const R* best = nullptr;
    for (const R& r : v)
        if (rva >= r.old_rva && rva - r.old_rva < r.size && (!best || r.size < best->size)) best = &r;
    return best;
}

const std::vector<PortField>& own_fields()
{
    static const std::vector<PortField> v = [] {
        if (is_table_build() && kFieldBuild == kPortToBuild) return port_fields_compiled();
        return port_resolve_fields(signature_image());
    }();
    return v;
}

const std::vector<PortField>& fields()
{
    static const std::vector<PortField> v = [] {
        if (!use_core_port()) return own_fields();
        std::vector<swml_port_field> c(g_api->port_fields(nullptr, 0));
        c.resize(g_api->port_fields(c.data(), c.size()));
        std::vector<PortField> out;
        for (const swml_port_field& f : c) {
            PortField p{};
            memcpy(p.owner, f.owner, sizeof(p.owner));
            p.owner[sizeof(p.owner) - 1] = 0;
            p.original = f.original;
            p.value = f.value;
            out.push_back(p);
        }
        return out;
    }();
    return v;
}
} // namespace

uint32_t game_build() { return read_build(nullptr); }

uintptr_t game_live_native(uintptr_t sig_build_address)
{
    uint32_t rva = static_cast<uint32_t>(sig_build_address - kGhidraImageBase);
    if (!g_api || game_build() == kOriginalBuild) return 0;
    if (is_table_build()) return game_base() + rva;
    // The core's native ranges are keyed by its signatures' build: usable only when that is this mod's too.
    if (use_core_port() && core_port()->sig_build == kSigBuild) {
        static const std::vector<swml_port_range> native = core_ranges(1);
        for (const swml_port_range& r : native)
            if (r.old_rva == rva) return r.new_rva == ~0u ? 0 : game_base() + r.new_rva;
    }
    static const std::vector<PortRange> native = port_resolve_signatures(signature_image(), true);
    for (const PortRange& r : native)
        if (r.old_rva == rva) return game_base() + r.new_rva;
    return 0;
}

bool game_build_ported()
{
    if (!g_api) return false; // before swml_mod_init (static initializers): not known yet, so not cached either
    static const bool ported = [] {
        if (is_table_build()) return true;
        if (game_build() == kOriginalBuild) return false;
        return !ranges().empty(); // any other build: ported by signature
    }();
    return ported;
}

bool game_build_matches()
{
    if (!g_api) return false;
    if (game_build_ported()) return g_ported_to == kPortToBuild;
    return g_api->game_build_known() && game_build() == kOriginalBuild;
}

void mod_ported_to(uint32_t build_timestamp)
{
    g_ported_to = build_timestamp;
    if (!game_build_ported()) return;
    const char* not_ported = build_timestamp == kPortToBuild ? "" : "; this mod is NOT ported to its own data's build";
    size_t fields_found = 0;
    for (const PortField& f : fields()) fields_found += f.value != kPortFieldUnresolved;
    if (is_table_build())
        swlog("running on build %08x through the port table (%zu ranges from %08x), %zu/%zu fields%s", game_build(),
              std::size(kPortRanges), kPortFromBuild, fields_found, fields().size(), not_ported);
    else if (use_core_port())
        swlog("running on build %08x with the loader's port data (%s, signatures %08x; this mod's own: %08x): %zu of %zu "
              "ranges, %zu/%zu fields; parts needing the others switch off%s", game_build(),
              core_port()->table_build == game_build() ? "ported to it" : "not ported yet", core_port()->sig_build,
              kSigBuild, ranges().size(), core_keys().size(), fields_found, fields().size(), not_ported);
    else
        swlog("running on build %08x, not ported yet: %zu of %zu ranges found by signature (from %08x), %zu/%zu fields; "
              "parts needing the others switch off%s", game_build(), ranges().size(), std::size(kSigRanges), kSigBuild,
              fields_found, fields().size(), not_ported);
}

uintptr_t game_live_if_mapped(uintptr_t ghidra_address)
{
    uint32_t rva = static_cast<uint32_t>(ghidra_address - kGhidraImageBase);
    if (!game_build_ported()) return game_base() + rva;
    if (const PortRange* r = containing(ranges(), rva)) return game_base() + r->new_rva + (rva - r->old_rva);
    // An address the core's data doesn't know (this mod is newer than the loader): this mod's own data.
    if (use_core_port() && !containing(core_keys(), rva))
        if (const PortRange* r = containing(own_ranges(), rva)) return game_base() + r->new_rva + (rva - r->old_rva);
    return 0;
}

uintptr_t game_live(uintptr_t ghidra_address)
{
    if (uintptr_t live = game_live_if_mapped(ghidra_address)) return live;
    if (g_port_misses.fetch_add(1) < 20) swlog("PORT: %llx is not in the port table", static_cast<unsigned long long>(ghidra_address));
    return 0;
}

size_t game_field(const char* owner, size_t original_offset)
{
    if (!g_api || !owner) return 0;
    if (game_build() == kOriginalBuild) return original_offset;
    const uint32_t original = static_cast<uint32_t>(original_offset);
    uint32_t value = 0;
    PortFieldLookup r = port_field_lookup(fields(), owner, original, &value);
    if (r == PortFieldLookup::not_covered && use_core_port()) r = port_field_lookup(own_fields(), owner, original, &value);
    if (r == PortFieldLookup::found) return value;
    if (g_port_misses.fetch_add(1) < 20)
        swlog("FIELD: %s+%x %s on build %08x", owner, original,
              r == PortFieldLookup::unresolved ? "can't be trusted (its anchors changed, or a field was inserted next to it)"
                                               : "is not anchored (sdk/port_fields.txt)", game_build());
    return 0;
}

uintptr_t game_ghidra(uintptr_t live)
{
    if (!live || live < game_base()) return 0;
    uint32_t rva = static_cast<uint32_t>(live - game_base());
    if (!game_build_ported()) return kGhidraImageBase + rva;
    const auto& v = by_new();
    auto it = std::upper_bound(v.begin(), v.end(), rva, [](uint32_t x, const PortRange& r) { return x < r.new_rva; });
    while (it != v.begin()) {
        --it;
        if (rva - it->new_rva < it->size) return kGhidraImageBase + it->old_rva + (rva - it->new_rva);
        if (rva - it->new_rva > 0x100000) break;
    }
    return 0;
}

bool hook_api(const wchar_t* module, const char* proc, void* detour, void** original)
{
    return g_api->hook_export(g_mod, module, proc, detour, original) != 0;
}

bool hook_address(uintptr_t target, void* detour, void** original)
{
    char what[32];
    uintptr_t ghidra = game_ghidra(target);
    snprintf(what, sizeof(what), "%llx", static_cast<unsigned long long>(ghidra ? ghidra : target - game_base() + kGhidraImageBase));
    return g_api->hook(g_mod, reinterpret_cast<void*>(target), detour, original, what) != 0;
}

bool hook_game(uintptr_t ghidra_address, void* detour, void** original)
{
    if (!game_build_matches()) {
        swlog("hook %llx refused: unknown game build", static_cast<unsigned long long>(ghidra_address));
        return false;
    }
    uintptr_t live = game_live(ghidra_address);
    if (!live) return false;
    return hook_address(live, detour, original);
}

bool status_line(const char* text, uint32_t color)
{
    if (!SWML_API_HAS_STATUS_LINE(g_api)) return false;
    g_api->status_line(g_mod, text, color);
    return true;
}

void present_add_callback(PresentCallback cb) { g_api->on_present(g_mod, &present_trampoline, reinterpret_cast<void*>(cb)); }

extern "C" __declspec(dllexport) int swml_mod_init(const swml_api* api, swml_mod* mod)
{
    // A newly compiled mod may still use only the v1 prefix on an older core.
    if (!api || api->version < 1 || api->size < SWML_API_V1_SIZE) return 0;
    g_api = api;
    g_mod = mod;
    for (auto fn : setups()) {
        if (!fn()) return 0;
    }
    int ok = 0;
    for (const HookDef& h : registry()) {
        if (h.needs_known_build && !game_build_matches()) {
            swlog("skip %s: needs the known game build", h.name);
            continue;
        }
        int misses = g_port_misses.load();
        InstallResult r = run_install(h.install);
        bool missed = g_port_misses.load() != misses;
        if (r != kInstalled || missed) swml_report_fault(g_api, g_mod);
        if (r == kCrashed)
            swlog("FAILED %s: crashed while installing%s; the other parts carry on", h.name,
                  missed ? " (it used an address missing from the port table)" : "");
        else if (r == kInstalled && missed)
            swlog("installed %s, but it asked for an address missing from the port table (see PORT: above)", h.name);
        else
            swlog("%s %s", r == kInstalled ? "installed" : "FAILED", h.name);
        ok += r == kInstalled;
    }
    // Callback/service-only mods need no private native hooks.
    if (ok == 0 && (!registry().empty() || setups().empty())) {
        mod_status("nothing could be installed (see this mod's log in logs\\mods)");
        return 0;
    }
    if (ok < static_cast<int>(registry().size()))
        mod_status("partly running: %d of %zu parts (see this mod's log in logs\\mods)", ok, registry().size());
    else
        mod_status("OK");
    return 1;
}
