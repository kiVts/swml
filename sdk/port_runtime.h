#pragma once
#include "port_table.h"

#include <cstddef>
#include <cstdint>
#include <vector>

std::vector<PortRange> port_resolve_signatures(uintptr_t base, bool native);

// The game exe mapped again from disk as an image: its bytes before any hook patched them; 0 if that failed.
uintptr_t port_pristine_image(uintptr_t game_base);

inline constexpr uint32_t kPortFieldUnresolved = 0xffffffffu;
struct PortField {
    char owner[16];
    uint32_t original; // offset in the original build (the key)
    uint32_t value;    // offset in the image it was resolved on, or kPortFieldUnresolved
};

// port_fields.h's anchors with their values on kFieldBuild itself (no resolution needed there).
std::vector<PortField> port_fields_compiled();
// port_fields.h's anchors read from the image at base. Sites of one field that disagree leave it unresolved.
std::vector<PortField> port_resolve_fields(uintptr_t base);

enum class PortFieldLookup { found, unresolved, not_covered };
// The rule for an owner's offset: an anchor gives its value; an offset strictly between the nearest anchors below and
// above it moves with them when both resolved and moved by the same amount (nothing was inserted between them).
// not_covered: the data has no anchor on one side (another data set may know more).
PortFieldLookup port_field_lookup(const std::vector<PortField>& fields, const char* owner, uint32_t original, uint32_t* out);

// Build identities of the data compiled into this binary.
struct PortDataInfo {
    uint32_t original_build, table_build, table_size, sig_build, field_build;
};
PortDataInfo port_data_info();
