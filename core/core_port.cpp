// Port data hosted by the core (swml_api port_*): resolved once, before any mod hooks the game, and handed to every
// mod. A loader release with new port data then ports mods built before it (their SDK prefers newer core data).
#include "core.h"
#include "core_port.h"

#include "port_runtime.h"
#include "port_sigs.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>

namespace {
struct Resolved {
    const char* mode = "original";
    std::vector<swml_port_range> ranges, native;
    std::vector<PortField> fields;
};

uint32_t running_build(uint32_t* size)
{
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (size) *size = nt->OptionalHeader.SizeOfImage;
    return nt->FileHeader.TimeDateStamp;
}

bool force_signatures()
{
    return GetPrivateProfileIntW(L"port", L"force_signatures", 0, (core_root() + L"\\swml.ini").c_str()) != 0;
}

// Every key of `keys`, with the address found for it or ~0u.
template <typename Key>
std::vector<swml_port_range> with_keys(const Key* first, const Key* last, const std::vector<PortRange>& found)
{
    std::vector<swml_port_range> out;
    for (const Key* k = first; k != last; ++k) {
        if (!k->size) continue;
        swml_port_range r{k->old_rva, k->size, ~0u};
        for (const PortRange& f : found)
            if (f.old_rva == k->old_rva && f.size == k->size) r.new_rva = f.new_rva;
        out.push_back(r);
    }
    return out;
}

const Resolved& resolved()
{
    static Resolved r;
    static std::once_flag once;
    std::call_once(once, [] {
        uint32_t size = 0;
        const uint32_t build = running_build(&size);
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (build == kPortFromBuild) return; // mods use their addresses as they are
        if (build == kPortToBuild && size == kPortToSize && !force_signatures()) {
            r.mode = "port table";
            for (const PortRange& p : kPortRanges) r.ranges.push_back({p.old_rva, p.size, p.new_rva});
            for (const SigRange& s : kSigNative)
                if (s.size) r.native.push_back({s.old_rva, s.size, s.old_rva});
            r.fields = port_fields_compiled();
        } else {
            r.mode = "signatures";
            uintptr_t image = port_pristine_image(base);
            if (!image) {
                corelog("port: could not map the game exe from disk; signatures read the live image");
                image = base;
            }
            r.ranges = with_keys(std::begin(kSigRanges), std::end(kSigRanges), port_resolve_signatures(image, false));
            r.native = with_keys(std::begin(kSigNative), std::end(kSigNative), port_resolve_signatures(image, true));
            r.fields = port_resolve_fields(image);
        }
        auto found = [](const auto& v) { return std::count_if(v.begin(), v.end(), [](const auto& x) { return x.new_rva != ~0u; }); };
        size_t fields_found = std::count_if(r.fields.begin(), r.fields.end(), [](const PortField& f) { return f.value != kPortFieldUnresolved; });
        corelog("port: game build %08x via %s (data: table %08x, signatures %08x, fields %08x): %zu/%zu ranges, %zu/%zu native, "
                "%zu/%zu fields", build, r.mode, kPortToBuild, kSigBuild, port_data_info().field_build, size_t(found(r.ranges)),
                r.ranges.size(), size_t(found(r.native)), r.native.size(), fields_found, r.fields.size());
        for (const PortField& f : r.fields)
            if (f.value == kPortFieldUnresolved) corelog("port: field %s+%x unresolved (its anchor functions changed)", f.owner, f.original);
    });
    return r;
}

int api_port_info(swml_port_info* out)
{
    if (!out || out->size < sizeof(swml_port_info)) return 0;
    PortDataInfo d = port_data_info();
    *out = {sizeof(swml_port_info), running_build(nullptr), d.original_build, d.table_build, d.sig_build, d.field_build};
    return 1;
}

size_t api_port_ranges(int native, swml_port_range* out, size_t cap)
{
    const auto& v = native ? resolved().native : resolved().ranges;
    if (out) std::copy_n(v.begin(), std::min(cap, v.size()), out);
    return v.size();
}

size_t api_port_fields(swml_port_field* out, size_t cap)
{
    const auto& v = resolved().fields;
    for (size_t i = 0; out && i < std::min(cap, v.size()); i++) {
        static_assert(sizeof(out[i].owner) == sizeof(v[i].owner));
        memcpy(out[i].owner, v[i].owner, sizeof(out[i].owner));
        out[i].original = v[i].original;
        out[i].value = v[i].value;
    }
    return v.size();
}
} // namespace

void core_port_init() { resolved(); }
int (*core_port_info())(swml_port_info*) { return &api_port_info; }
size_t (*core_port_ranges())(int, swml_port_range*, size_t) { return &api_port_ranges; }
size_t (*core_port_fields())(swml_port_field*, size_t) { return &api_port_fields; }
