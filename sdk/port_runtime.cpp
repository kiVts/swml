#include "port_runtime.h"

#include "port_fields.h"
#include "port_sigs.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <unordered_map>

static_assert(kSigBuild == kPortToBuild, "port_sigs.h and port_table.h are from different builds");

namespace {
const IMAGE_NT_HEADERS64* nt_of(uintptr_t base)
{
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
}

// FNV-1a 64 over the function's bytes, masked spans read as 0.
template <typename Fn, typename Span>
uint64_t sig_hash(const uint8_t* p, const Fn& f, const Span* spans)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const Span* span = spans + f.span_first;
    const Span* spans_end = span + f.span_count;
    for (uint32_t i = 0; i < f.len; i++) {
        while (span != spans_end && i >= span->off + span->len) span++;
        uint8_t b = (span != spans_end && i >= span->off) ? 0 : p[i];
        h = (h ^ b) * 0x100000001b3ull;
    }
    return h;
}

// .pdata functions (fragments included) by length.
std::unordered_multimap<uint32_t, uint32_t> functions_by_length(uintptr_t base)
{
    const IMAGE_DATA_DIRECTORY& ex = nt_of(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* rf = reinterpret_cast<const RUNTIME_FUNCTION*>(base + ex.VirtualAddress);
    size_t n = ex.Size / sizeof(RUNTIME_FUNCTION);
    std::unordered_multimap<uint32_t, uint32_t> by_len;
    by_len.reserve(n);
    for (size_t i = 0; i < n; i++)
        if (rf[i].EndAddress > rf[i].BeginAddress) by_len.emplace(rf[i].EndAddress - rf[i].BeginAddress, rf[i].BeginAddress);
    return by_len;
}

// Start rva of each signed function in the image, -1 where it isn't found exactly as often as in the signatures' build.
template <typename Fn, size_t N, typename Span>
std::vector<int64_t> find_functions(uintptr_t base, const std::unordered_multimap<uint32_t, uint32_t>& by_len,
                                    const Fn (&funcs)[N], const Span* spans)
{
    std::vector<int64_t> live(N, -1);
    for (size_t k = 0; k < N; k++) {
        const Fn& f = funcs[k];
        std::vector<uint32_t> hits;
        auto [lo, hi] = by_len.equal_range(f.len);
        for (auto it = lo; it != hi; ++it) {
            auto* p = reinterpret_cast<const uint8_t*>(base + it->second);
            bool pre = true;
            for (int i = 0; i < 16 && pre; i++) pre = !(f.prefix_mask >> i & 1) || p[i] == f.prefix[i];
            if (pre && sig_hash(p, f, spans) == f.hash) hits.push_back(it->second);
        }
        std::sort(hits.begin(), hits.end());
        if (hits.size() == f.equals) live[k] = hits[f.rank];
    }
    return live;
}

PortField make_field(const FieldAnchor& a, uint32_t value)
{
    PortField f{};
    strncpy_s(f.owner, kFieldOwners[a.owner], _TRUNCATE);
    f.original = a.original;
    f.value = value;
    return f;
}
} // namespace

std::vector<PortRange> port_resolve_signatures(uintptr_t base, bool native)
{
    std::vector<PortRange> out;
    const IMAGE_NT_HEADERS64* nt = nt_of(base);
    std::vector<int64_t> live = find_functions(base, functions_by_length(base), kSigFuncs, kSigSpans);

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

uintptr_t port_pristine_image(uintptr_t game_base)
{
    wchar_t path[MAX_PATH];
    if (!game_base || !GetModuleFileNameW(reinterpret_cast<HMODULE>(game_base), path, MAX_PATH)) return 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    CloseHandle(f);
    if (!m) return 0;
    void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);
    return reinterpret_cast<uintptr_t>(v);
}

std::vector<PortField> port_fields_compiled()
{
    std::vector<PortField> out;
    for (const FieldAnchor& a : kFieldAnchors) out.push_back(make_field(a, a.sig));
    return out;
}

std::vector<PortField> port_resolve_fields(uintptr_t base)
{
    std::vector<int64_t> live = find_functions(base, functions_by_length(base), kFieldFuncs, kFieldSpans);
    std::vector<PortField> out;
    for (const FieldAnchor& a : kFieldAnchors) {
        uint32_t value = kPortFieldUnresolved;
        bool conflict = false;
        for (uint32_t i = a.site_first; i < a.site_first + a.site_count; i++) {
            const FieldSite& s = kFieldSites[i];
            if (live[s.func] < 0) continue; // this anchor function changed: the others may still have it
            uint32_t v;
            memcpy(&v, reinterpret_cast<const void*>(base + static_cast<uintptr_t>(live[s.func]) + s.disp_off), sizeof(v));
            if (value == kPortFieldUnresolved) value = v;
            else if (v != value) conflict = true;
        }
        out.push_back(make_field(a, conflict ? kPortFieldUnresolved : value));
    }
    return out;
}

PortFieldLookup port_field_lookup(const std::vector<PortField>& fields, const char* owner, uint32_t original, uint32_t* out)
{
    const PortField* below = nullptr;
    const PortField* above = nullptr;
    for (const PortField& f : fields) {
        if (strncmp(f.owner, owner, sizeof(f.owner)) != 0) continue;
        if (f.original == original) {
            if (f.value == kPortFieldUnresolved) return PortFieldLookup::unresolved;
            *out = f.value;
            return PortFieldLookup::found;
        }
        if (f.original < original && (!below || f.original > below->original)) below = &f;
        if (f.original > original && (!above || f.original < above->original)) above = &f;
    }
    if (!below || !above) return PortFieldLookup::not_covered;
    if (below->value == kPortFieldUnresolved || above->value == kPortFieldUnresolved) return PortFieldLookup::unresolved;
    int64_t d_below = int64_t(below->value) - below->original, d_above = int64_t(above->value) - above->original;
    if (d_below != d_above) return PortFieldLookup::unresolved; // something was inserted or removed in between
    *out = static_cast<uint32_t>(original + d_below);
    return PortFieldLookup::found;
}

PortDataInfo port_data_info() { return {kPortFromBuild, kPortToBuild, kPortToSize, kSigBuild, kFieldBuild}; }
