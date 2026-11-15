#pragma once
// Opt-in native property editors and shared advanced-text sizing.
// Strings use the native multiline row and its write-back slot. Scalars retain
// their native single-line object and parsing/write-back contracts while their
// property rows expand below the label. See native_single_line.h for viewports.
#include "hooks.h"
#include "resolve.h"
#include "native_text_field_signatures.h"
#include "native_ui_focus.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace swml4::text_fields {
struct Options {
    float font_scale = .8f;
    float box_height = .21f;
};
inline bool valid(Options o) {
    return std::isfinite(o.font_scale) && o.font_scale >= .65f && o.font_scale <= 1.f &&
        std::isfinite(o.box_height) && o.box_height >= .125f && o.box_height <= .8f;
}
namespace detail {
template<class T> inline T& field(void* p, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(p) + offset);
}
using Init = void(*)(void*, uintptr_t);
// The extra eighth editor argument is forwarded conservatively: the native
// prologue uses that stack slot as constructor scratch (check_hook_args audit).
using Editor = void*(*)(void*, float, float, float, float, uint32_t, uint8_t, uintptr_t);
using Text = void*(*)(void*, float, float, float, float, uint32_t, uint8_t, uint8_t);
using SimpleEditor = void*(*)(void*, float, float, float, float, uint32_t, void*, uint32_t);
using Key = uint64_t(*)(void*, uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t);
using Repeat = uint64_t(*)(void*, uintptr_t, uintptr_t, uint32_t);
using TextDelete = void*(*)(void*, uint64_t);
struct Bindings {
    Init original_row = nullptr, multiline_init = nullptr;
    Init original_float = nullptr, original_expression = nullptr, original_bool_expression = nullptr;
    Init original_text_init = nullptr;
    Editor original_editor = nullptr;
    SimpleEditor original_simple_editor = nullptr;
    Text original_text = nullptr;
    Key original_key = nullptr;
    Repeat original_repeat = nullptr;
    TextDelete original_text_delete = nullptr;
    std::set<void*> owned_text;
    std::map<void*, float> scaled_text;
    uintptr_t row_table = 0, multiline_table = 0;
    uintptr_t float_table = 0, expression_table = 0, bool_expression_table = 0;
    Options options;
    void (*log)(const char*, ...) = nullptr;
    bool active = false;
};
inline Bindings& bindings() { static Bindings b; return b; }
struct Request {
    void* row = nullptr;
    bool editor_pending = true, text_pending = false;
    bool single_line = false;
    bool native_height = false; // number-only rows keep the one-line box
    float height = 0;           // editor height actually constructed
};
// The advanced editor insets its text by this much on every side (native
// width/height - 0.05, centered). Property editors use half of it.
constexpr float native_text_inset = .025f, text_inset = native_text_inset * .5f;
inline thread_local Request* current_request = nullptr;
struct Scope {
    Request* previous;
    explicit Scope(Request& request) : previous(current_request) { current_request = &request; }
    ~Scope() { current_request = previous; }
};
inline void* text_ctor(void* object, float x, float y, float width, float height,
                       uint32_t anchor, uint8_t syntax, uint8_t readonly) {
    auto& b = bindings();
    auto* request = current_request;
    const bool ours = b.active && request && request->text_pending;
    if (ours) {
        request->text_pending = false;
        // Halve the native inset around the text element inside the editor.
        if (anchor == 4 && width > 0 && height > 0) {
            width += 2 * (native_text_inset - text_inset);
            height += 2 * (native_text_inset - text_inset);
        }
    }
    void* result = b.original_text(object, x, y, width, height, anchor, syntax, readonly);
    if (b.active && (ours || !readonly)) {
        b.owned_text.insert(result);
        // Set BEFORE native initialization builds the paragraph/caret geometry.
        field<float>(result, 0x70) *= b.options.font_scale;
        field<float>(result, 0x74) = 1.f; // line spacing, not glyph size
        b.scaled_text[result] = field<float>(result, 0x70);
    }
    if (ours) {
        // Preserve an already stored value even when it exceeds the editor's
        // usual 4096-byte typing limit. Native text_set otherwise truncates it.
        auto* value = field<void*>(request->row, 0x68);
        if (value) field<uint32_t>(result, 0x1f8) = std::max(4096u, field<uint32_t>(value, 8));
    }
    return result;
}
inline void text_init(void* object, uintptr_t application) {
    auto& b = bindings();
    if (b.active && !field<uint8_t>(object, 0x1f0)) {
        const float scale = field<float>(object, 0x70);
        const auto previous = b.scaled_text.find(object);
        // Honor custom native font sizes supplied after construction, without
        // multiplying again when a field is reinitialized at the same size.
        if (previous == b.scaled_text.end() || previous->second != scale)
            field<float>(object, 0x70) = scale * b.options.font_scale;
        field<float>(object, 0x74) = 1.f;
        b.scaled_text[object] = field<float>(object, 0x70);
        b.owned_text.insert(object);
    }
    b.original_text_init(object, application);
}
inline bool typing(void* object, uintptr_t application) {
    auto& b = bindings();
    return b.active && b.owned_text.contains(object) && native_ui_keyboard_focus(application) == object &&
        field<uint8_t>(object, 0x1c) && field<uint8_t>(object, 0x1db) && !field<uint8_t>(object, 0x1f0);
}
inline uint64_t key(void* object, uintptr_t input, uintptr_t application, uintptr_t events, uint32_t code, uint32_t state) {
    const bool captured = typing(object, application);
    // Native handling runs first, including selection, clipboard, modifiers and
    // key releases. Ordinary letters return false natively and would reach WASD.
    const auto result = bindings().original_key(object, input, application, events, code, state);
    return captured ? (result & ~uint64_t(255)) | 1 : result;
}
inline uint64_t repeat(void* object, uintptr_t application, uintptr_t events, uint32_t code) {
    const bool captured = typing(object, application);
    const auto result = bindings().original_repeat(object, application, events, code);
    return captured ? (result & ~uint64_t(255)) | 1 : result;
}
inline void* text_delete(void* object, uint64_t flags) {
    bindings().owned_text.erase(object);
    bindings().scaled_text.erase(object);
    return bindings().original_text_delete(object, flags);
}
inline void* editor_ctor(void* object, float x, float y, float width, float height,
                         uint32_t anchor, uint8_t syntax, uintptr_t scratch) {
    auto& b = bindings();
    auto* request = current_request;
    const bool ours = b.active && request && !request->single_line && request->editor_pending;
    if (ours) {
        request->editor_pending = false;
        request->text_pending = true;
        auto* label = field<void*>(request->row, 0x80);
        const float label_height = std::max(.075f, field<float>(label, 0x14));
        x = 0; y = -label_height - .025f;
        width = field<float>(request->row, 0x10);
        height = b.options.box_height;
        anchor = 0; // top left
    }
    return b.original_editor(object, x, y, width, height, anchor, syntax, scratch);
}
inline void* simple_editor_ctor(void* object, float x, float y, float width, float height,
                                uint32_t anchor, void* value, uint32_t flags) {
    auto& b = bindings();
    auto* request = current_request;
    if (b.active && request && request->single_line && request->editor_pending) {
        request->editor_pending = false;
        auto* label = field<void*>(request->row, 0x80);
        x = 0; y = -std::max(.075f, field<float>(label, 0x14)) - .025f;
        width = field<float>(request->row, 0x10);
        if (!request->native_height || !std::isfinite(height) || height <= 0) height = b.options.box_height;
        anchor = 0;
        request->height = height;
    }
    return b.original_simple_editor(object, x, y, width, height, anchor, value, flags);
}
inline void scalar_row_init(void* row, uintptr_t application, Init original, bool expression, bool number = false) {
    auto& b = bindings();
    if (!b.active || !std::isfinite(field<float>(row, 0x10)) || field<float>(row, 0x10) <= .035f) {
        original(row, application);
        return;
    }
    field<float>(row, 0x58) = field<float>(row, 0x10);
    Request request{row, true, false, true, number};
    Scope scope(request);
    original(row, application);
    auto* label = field<void*>(row, 0x80);
    field<float>(label, 8) = field<float>(label, 12) = 0;
    field<uint32_t>(label, 0x18) = 0;
    const float box = request.height > 0 ? request.height : b.options.box_height;
    float extent = std::max(.075f, field<float>(label, 0x14)) + .025f + box;
    if (expression) {
        auto* error = field<void*>(row, 0x88);
        field<float>(error, 8) = 0;
        field<float>(error, 12) = -extent - .025f;
        field<uint32_t>(error, 0x18) = 0;
        extent += .025f + field<float>(error, 0x14);
    }
    field<float>(row, 0x14) = extent;
}
inline void float_row_init(void* row, uintptr_t application) {
    // Number-only rows: full width below the label, native one-line height.
    scalar_row_init(row, application, bindings().original_float, false, true);
}
inline void expression_row_init(void* row, uintptr_t application) {
    scalar_row_init(row, application, bindings().original_expression, true);
}
inline void bool_expression_row_init(void* row, uintptr_t application) {
    scalar_row_init(row, application, bindings().original_bool_expression, true);
}
inline void row_init(void* row, uintptr_t application) {
    auto& b = bindings();
    const auto table = field<uintptr_t>(row, 0);
    const bool multiline = table == b.multiline_table;
    if (!b.active || (table != b.row_table && !multiline) ||
        !std::isfinite(field<float>(row, 0x10)) || field<float>(row, 0x10) <= .035f) {
        (multiline ? b.multiline_init : b.original_row)(row, application);
        return;
    }
    field<uintptr_t>(row, 0) = b.multiline_table;
    field<float>(row, 0x58) = field<float>(row, 0x10); // full-width label
    Request request{row};
    Scope scope(request);
    b.multiline_init(row, application);
    auto* label = field<void*>(row, 0x80);
    field<float>(label, 8) = 0;
    field<float>(label, 12) = 0;
    field<uint32_t>(label, 0x18) = 0;
    // Parent layout sees the total extent before placing subsequent rows.
    field<float>(row, 0x14) = std::max(.075f, field<float>(label, 0x14)) + .025f + b.options.box_height;
}
inline bool matches(uintptr_t address, const char* pattern) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    while (*pattern) {
        if (*pattern == ' ') { ++pattern; continue; }
        if (*pattern != '?' && *bytes != ((hex(pattern[0]) << 4) | hex(pattern[1]))) return false;
        ++bytes; pattern += 2;
    }
    return true;
}
inline bool resolve_contracts(std::map<std::string, uintptr_t>& functions) {
    for (const auto& sig : kNativeTextFieldSignatures) {
        const auto table = sig.rtti ? native_find_vtable(sig.rtti) : 0;
        const auto address = sig.rtti ? (table ? reinterpret_cast<const uintptr_t*>(table)[sig.slot] : 0) : native_find_pattern(sig.scan);
        if (!address) return false;
        const auto bytes = (strlen(sig.full) + 1) / 3;
        const auto limit = address + bytes;
        // MSVC splits editor_update and text_wrap into contiguous chained
        // unwind fragments. Check the complete readable code extent, not only
        // the first .pdata entry. The byte comparison covers every fragment.
        // Short contracts are scanned in full inside .text, including leaf
        // focus/repeat methods which legitimately have no unwind entry.
        auto cursor = bytes <= 512 ? limit : address;
        for (unsigned fragment = 0; cursor < limit && fragment < 16; ++fragment) {
            uintptr_t begin = 0, end = 0;
            if (!native_function_bounds(cursor, &begin, &end) || begin != cursor || end <= cursor) return false;
            cursor = end;
        }
        if (cursor < limit || !matches(address, sig.full)) return false;
        functions[sig.name] = address;
    }
    for (const auto& call : kNativeTextFieldCalls) {
        const auto at = functions[call.caller] + call.offset;
        int32_t displacement = 0;
        memcpy(&displacement, reinterpret_cast<const void*>(at + 1), sizeof(displacement));
        if (*reinterpret_cast<const uint8_t*>(at) != 0xe8 || at + 5 + displacement != functions[call.callee]) return false;
    }
    return true;
}
} // namespace detail

inline bool install(Options options = {}, std::map<std::string, uintptr_t> functions = {}) {
    using namespace detail;
    auto& b = bindings();
    if (b.active || !valid(options)) return false;
    if (functions.empty() && !resolve_contracts(functions)) return false;
    b.row_table = native_find_vtable("c_ui_properties_element_text_string");
    b.multiline_table = native_find_vtable("c_ui_properties_element_text_string_multiline");
    b.float_table = native_find_vtable("c_ui_properties_element_text_float");
    b.expression_table = native_find_vtable("c_ui_properties_element_text_infix_expression");
    b.bool_expression_table = native_find_vtable("c_ui_properties_element_text_infix_expression_bool");
    if (!b.row_table || !b.multiline_table || !b.float_table || !b.expression_table || !b.bool_expression_table) return false;
    const auto* row = reinterpret_cast<const uintptr_t*>(b.row_table);
    const auto* multiline = reinterpret_cast<const uintptr_t*>(b.multiline_table);
    if (row[0] != functions["row_delete"] || row[1] != functions["row_init"] ||
        row[37] != functions["row_write"] || multiline[0] != row[0] ||
        multiline[1] != functions["multiline_init"] || multiline[37] != functions["multiline_write"]) return false;
    for (size_t slot = 2; slot < 37; ++slot) if (row[slot] != multiline[slot]) return false;
    for (const auto& [table, init, write] : {
        std::tuple{b.float_table, "float_init", "float_write"},
        std::tuple{b.expression_table, "expression_init", "expression_write"},
        std::tuple{b.bool_expression_table, "bool_expression_init", "bool_expression_write"}}) {
        const auto* methods = reinterpret_cast<const uintptr_t*>(table);
        if (methods[1] != functions[init] || methods[37] != functions[write]) return false;
    }
    const auto advanced = native_find_vtable("c_ui_element_editbox_adv");
    const auto text = native_find_vtable("c_ui_element_editbox_adv_text");
    if (!advanced || !text || reinterpret_cast<const uintptr_t*>(advanced)[1] != functions["editor_init"] ||
        reinterpret_cast<const uintptr_t*>(advanced)[3] != functions["editor_update"] ||
        reinterpret_cast<const uintptr_t*>(text)[1] != functions["text_init"] ||
        reinterpret_cast<const uintptr_t*>(text)[0] != functions["text_delete"] ||
        reinterpret_cast<const uintptr_t*>(text)[20] != functions["text_key"] ||
        reinterpret_cast<const uintptr_t*>(text)[23] != functions["text_repeat"]) return false;
    b.options = options;
    // Activate only after the complete bridge is installed. Partial hooks pass
    // through to their originals without reading or changing native fields.
    if (!hook_address(functions["editor_ctor"], reinterpret_cast<void*>(editor_ctor), reinterpret_cast<void**>(&b.original_editor)) ||
        !hook_address(functions["text_ctor"], reinterpret_cast<void*>(text_ctor), reinterpret_cast<void**>(&b.original_text)) ||
        !hook_address(functions["text_init"], reinterpret_cast<void*>(text_init), reinterpret_cast<void**>(&b.original_text_init)) ||
        !hook_address(functions["simple_editor_ctor"], reinterpret_cast<void*>(simple_editor_ctor), reinterpret_cast<void**>(&b.original_simple_editor)) ||
        !hook_address(functions["float_init"], reinterpret_cast<void*>(float_row_init), reinterpret_cast<void**>(&b.original_float)) ||
        !hook_address(functions["expression_init"], reinterpret_cast<void*>(expression_row_init), reinterpret_cast<void**>(&b.original_expression)) ||
        !hook_address(functions["bool_expression_init"], reinterpret_cast<void*>(bool_expression_row_init), reinterpret_cast<void**>(&b.original_bool_expression)) ||
        !hook_address(functions["text_key"], reinterpret_cast<void*>(key), reinterpret_cast<void**>(&b.original_key)) ||
        !hook_address(functions["text_repeat"], reinterpret_cast<void*>(repeat), reinterpret_cast<void**>(&b.original_repeat)) ||
        !hook_address(functions["text_delete"], reinterpret_cast<void*>(text_delete), reinterpret_cast<void**>(&b.original_text_delete)) ||
        !hook_address(functions["multiline_init"], reinterpret_cast<void*>(row_init), reinterpret_cast<void**>(&b.multiline_init)) ||
        !hook_address(functions["row_init"], reinterpret_cast<void*>(row_init), reinterpret_cast<void**>(&b.original_row))) return false;
    b.active = true;
    return true;
}
} // namespace swml4::text_fields
