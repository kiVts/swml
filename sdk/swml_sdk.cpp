// Mod-side runtime, linked into every mod DLL: the swml_mod_init export, and the familiar helpers
// (swlog, hook_*, present_add_callback, ...) implemented on top of the core's swml_api table.

#include "game.h"
#include "hooks.h"
#include "log.h"
#include "port_sigs.h"
#include "port_table.h"
#include "present.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
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

static uint64_t sig_hash(const uint8_t* p, const SigFunc& f)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const SigSpan* span = kSigSpans + f.span_first;
    const SigSpan* spans_end = span + f.span_count;
    for (uint32_t i = 0; i < f.len; i++) {
        while (span != spans_end && i >= span->off + span->len) span++;
        uint8_t b = (span != spans_end && i >= span->off) ? 0 : p[i];
        h = (h ^ b) * 0x100000001b3ull;
    }
    return h;
}

std::vector<PortRange> port_resolve_signatures(uintptr_t base, bool native)
{
    static_assert(kSigBuild == kPortToBuild, "port_sigs.h and port_table.h are from different builds");
    std::vector<PortRange> out;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* rf = reinterpret_cast<const RUNTIME_FUNCTION*>(base + ex.VirtualAddress);
    size_t n_rf = ex.Size / sizeof(RUNTIME_FUNCTION);
    std::unordered_multimap<uint32_t, uint32_t> by_len; // length -> start rva
    by_len.reserve(n_rf);
    for (size_t i = 0; i < n_rf; i++)
        if (rf[i].EndAddress > rf[i].BeginAddress) by_len.emplace(rf[i].EndAddress - rf[i].BeginAddress, rf[i].BeginAddress);

    std::vector<int64_t> live(std::size(kSigFuncs), -1);
    for (size_t k = 0; k < std::size(kSigFuncs); k++) {
        const SigFunc& f = kSigFuncs[k];
        std::vector<uint32_t> hits;
        auto [lo, hi] = by_len.equal_range(f.len);
        for (auto it = lo; it != hi; ++it) {
            auto* p = reinterpret_cast<const uint8_t*>(base + it->second);
            bool pre = true;
            for (int i = 0; i < 16 && pre; i++) pre = !(f.prefix_mask >> i & 1) || p[i] == f.prefix[i];
            if (pre && sig_hash(p, f) == f.hash) hits.push_back(it->second);
        }
        std::sort(hits.begin(), hits.end());
        if (hits.size() == f.equals) live[k] = hits[f.rank];
    }

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t text_lo = 0, text_hi = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 6)) text_lo = base + sec[i].VirtualAddress, text_hi = text_lo + sec[i].Misc.VirtualSize;

    const SigRange* first = native ? std::begin(kSigNative) : std::begin(kSigRanges);
    const SigRange* last = native ? std::end(kSigNative) : std::end(kSigRanges);
    for (const SigRange* it = first; it != last; ++it) {
        const SigRange& r = *it;
        if (!r.size) continue;
        int64_t at = -1;
        if (r.kind == 0) {
            at = r.a;
        } else if (r.kind == 1) {
            if (live[r.a] >= 0) at = live[r.a] + r.b;
        } else if (r.kind == 2) {
            const SigWindow& w = kSigWindows[r.a];
            const uint16_t* pat = kSigWindowBytes + w.first;
            int found = 0;
            for (uintptr_t a = text_lo; a + w.len <= text_hi && found < 2; a++) {
                auto* p = reinterpret_cast<const uint8_t*>(a);
                uint32_t i = 0;
                while (i < w.len && ((pat[i] & 0x100) || p[i] == pat[i])) i++;
                if (i == w.len) at = static_cast<int64_t>(a - base), found++;
            }
            if (found != 1) at = -1;
        } else if (live[r.a] >= 0) {
            uintptr_t fn = base + static_cast<uintptr_t>(live[r.a]);
            int32_t disp = *reinterpret_cast<const int32_t*>(fn + (r.b >> 16));
            at = static_cast<int64_t>(fn + (r.b & 0xffff) - base) + disp + r.c;
        }
        if (at >= 0 && at < nt->OptionalHeader.SizeOfImage) out.push_back({r.old_rva, r.size, static_cast<uint32_t>(at)});
    }
    return out;
}

namespace {
constexpr uint32_t kOriginalBuild = kPortFromBuild;
uint32_t g_ported_to = 0;
std::atomic<int> g_port_misses{0}; // game_live() calls that found no range (a part using one is switched off)

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

// The game exe mapped again from disk as an image: the bytes before any mod's hooks patched them (the mods load one
// after another, so the live image has the earlier mods' detours in it). Shared and read-only; never unmapped.
uintptr_t pristine_image()
{
    static const uintptr_t view = [] {
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(game_base()), path, MAX_PATH)) return uintptr_t(0);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return uintptr_t(0);
        HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
        CloseHandle(f);
        if (!m) return uintptr_t(0);
        void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(m);
        return reinterpret_cast<uintptr_t>(v);
    }();
    return view;
}

uintptr_t signature_image()
{
    if (uintptr_t p = pristine_image()) return p;
    swlog("port: could not map the game exe from disk; signatures read the live image (other mods' hooks may hide some)");
    return game_base();
}

std::vector<PortRange> resolve_by_signature() { return port_resolve_signatures(signature_image(), false); }

const std::vector<PortRange>& ranges()
{
    static const std::vector<PortRange> v = [] {
        if (is_table_build()) return std::vector<PortRange>(std::begin(kPortRanges), std::end(kPortRanges));
        return resolve_by_signature();
    }();
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
} // namespace

uint32_t game_build() { return read_build(nullptr); }

uintptr_t game_live_native(uintptr_t sig_build_address)
{
    uint32_t rva = static_cast<uint32_t>(sig_build_address - kGhidraImageBase);
    if (!g_api || game_build() == kOriginalBuild) return 0;
    if (is_table_build()) return game_base() + rva;
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
    if (is_table_build())
        swlog("running on build %08x through the port table (%zu ranges from %08x)%s", game_build(), std::size(kPortRanges),
              kPortFromBuild, build_timestamp == kPortToBuild ? "" : ": this mod is NOT ported to it");
    else
        swlog("running on build %08x, not ported yet: %zu of %zu ranges found by signature (from %08x), parts needing the "
              "others switch off%s", game_build(), ranges().size(), std::size(kSigRanges), kSigBuild,
              build_timestamp == kPortToBuild ? "" : "; this mod is NOT ported to the signatures' build");
}

uintptr_t game_live_if_mapped(uintptr_t ghidra_address)
{
    uint32_t rva = static_cast<uint32_t>(ghidra_address - kGhidraImageBase);
    if (!game_build_ported()) return game_base() + rva;
    // Ranges are few (one per address the sources name); the smallest containing range wins.
    const PortRange* best = nullptr;
    for (const PortRange& r : ranges())
        if (rva >= r.old_rva && rva - r.old_rva < r.size && (!best || r.size < best->size)) best = &r;
    return best ? game_base() + best->new_rva + (rva - best->old_rva) : 0;
}

uintptr_t game_live(uintptr_t ghidra_address)
{
    if (uintptr_t live = game_live_if_mapped(ghidra_address)) return live;
    if (g_port_misses.fetch_add(1) < 20) swlog("PORT: %llx is not in the port table", static_cast<unsigned long long>(ghidra_address));
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

void present_add_callback(PresentCallback cb) { g_api->on_present(g_mod, &present_trampoline, reinterpret_cast<void*>(cb)); }

extern "C" __declspec(dllexport) int swml_mod_init(const swml_api* api, swml_mod* mod)
{
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
