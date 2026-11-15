#pragma once
// Keep native simple editors (save name, search, etc.) single-line. The native
// value, filters and editing callbacks stay in place; only presentation changes.
#include "native_text_fields.h"
#include "text_viewport.h"
#include <unordered_map>

namespace swml4::text_fields::single_line {
using detail::field;
struct String { const char* data; uint32_t length, capacity; };
using Update = void(*)(void*, uintptr_t, uintptr_t);
using Layout = void(*)(void*, uintptr_t, const String*);
using Draw = void(*)(void*, uintptr_t, uintptr_t, uintptr_t, float, float);
using Hit = uint32_t(*)(void*, const float*);
using Delete = void*(*)(void*, uint64_t);
using Glyph = const void*(*)(uintptr_t, uint32_t, int, int);
using Kerning = int32_t(*)(uintptr_t, uint32_t, uint32_t);
using Mesh = void(*)(void*, uintptr_t, const String*);
using Rectangle = void(*)(uintptr_t, const float*, uint32_t, float);
using Color = void(*)(void*, uintptr_t, uint32_t);
struct State {
    std::string source, rendered;
    TextViewport view;
    uintptr_t font = 0;
    float scale = 0, width = 0, height = 0;
    float scroll = 0;
    uint16_t format = 0;
    uintptr_t mesh = 0;      // mesh object our last rebuild left at +98
    bool mesh_ready = false;
    unsigned reported = 0;   // diagnostic kinds already logged for this field
    uint32_t color = 0;      // native editbox text color override (+d0)
    bool colored = false;
};
using Log = void(*)(const char*, ...);
struct Bindings {
    Update original_update = nullptr;
    Layout original_layout = nullptr;
    Layout original_base_layout = nullptr;
    Draw original_draw = nullptr;
    Draw original_base_draw = nullptr;
    Hit original_hit = nullptr;
    Delete original_delete = nullptr;
    Delete original_base_delete = nullptr;
    Rectangle original_rectangle = nullptr;
    Color original_color = nullptr;
    Glyph glyph = nullptr; Kerning kerning = nullptr; Mesh mesh = nullptr;
    uintptr_t table = 0, base_table = 0;
    float font_scale = .8f;
    bool active = false;
    Log log = nullptr;
    std::unordered_map<void*, State> states;
};
inline Bindings& bindings() { static Bindings b; return b; }

constexpr float padding = .0075f, caret_width = .005f; // padding: half the former .015
inline float line_height(const State& s, float height) {
    return std::clamp(field<int16_t>(reinterpret_cast<void*>(s.font), 0) * s.scale, .001f, height - .004f);
}
// Boxes with room for two lines are top-aligned like the multiline editor,
// with the same inset; one-line boxes stay vertically centered.
inline float lift(const State& s, float height) {
    const float line = line_height(s, height);
    return height >= 2 * line + 2 * detail::text_inset ? (height - line) * .5f - detail::text_inset : 0.f;
}
// One line per field and kind, so a reproduction leaves evidence in the mod log.
template<class... A> inline void report(State& s, unsigned kind, const char* format, A... args) {
    if (bindings().log && !(s.reported & kind)) { s.reported |= kind; bindings().log(format, args...); }
}
inline float caret_size(float scale) { return caret_width * scale / .0005f; }
inline bool eligible(void* p) {
    auto& b = bindings();
    // Flags +100/+101 select native syntax coloring, not a different object
    // layout. Function and number fields need the same viewport and hit mapping.
    return b.active && (field<uintptr_t>(p, 0) == b.table || (b.base_table && field<uintptr_t>(p, 0) == b.base_table)) &&
        std::isfinite(field<float>(p, 0x10)) && field<float>(p, 0x10) > 2 * padding + caret_width &&
        std::isfinite(field<float>(p, 0x14)) && field<float>(p, 0x14) > .01f && field<uintptr_t>(p, 0x90);
}
inline uint32_t active_caret(void* p) {
    const auto start = field<uint32_t>(p, 0x30), selected = field<uint32_t>(p, 0x44), length = field<uint32_t>(p, 0xc0);
    return static_cast<uint32_t>(std::min<uint64_t>(length, uint64_t(start) + (field<uint8_t>(p, 0x41) ? 0 : selected)));
}
inline State* metrics(void* p) {
    auto& b = bindings();
    const auto& value = field<String>(p, 0xb8);
    if (!value.data || value.length >= value.capacity || value.length > 1024 * 1024) {
        if (b.log && value.length < 1024 * 1024) report(b.states[p], 8, "field %p: value not usable (length %u, capacity %u)",
            p, value.length, value.capacity);
        return nullptr;
    }
    const auto font = field<uintptr_t>(p, 0x90);
    const auto count = field<uint32_t>(reinterpret_cast<void*>(font), 0x18);
    if (count > 65536) return nullptr;
    auto& state = b.states[p];
    const float scale = field<float>(p, 0xc8) * b.font_scale;
    if (!std::isfinite(scale) || scale <= 0 || scale > .01f) {
        report(state, 32, "field %p: glyph scale %.6f outside viewport range", p, field<float>(p, 0xc8));
        return nullptr;
    }
    const std::string_view source(value.data, value.length);
    if (source != state.source || state.font != font || state.scale != scale || state.view.bytes != value.length) {
        state.source.assign(source);
        state.font = font; state.scale = scale;
        state.view.assign(source, [&](uint32_t previous, uint32_t code) {
            const auto* glyph = count ? b.glyph(font, code, 0, int(count) - 1) : nullptr;
            const float advance = glyph ? *reinterpret_cast<const int16_t*>(static_cast<const char*>(glyph) + 12) * scale : 0;
            return std::pair{advance, glyph ? b.kerning(font, previous, code) * scale : 0.f};
        });
        state.mesh_ready = false;
    }
    state.view.follow(active_caret(p), field<float>(p, 0x10) - 2 * padding - caret_size(scale));
    return &state;
}
inline bool refresh(void* p, uintptr_t application) {
    auto* state = metrics(p);
    if (!state || !application) return false;
    const float width = field<float>(p, 0x10), height = field<float>(p, 0x14);
    const float inner = width - 2 * padding;
    // The base object is only 0x100 bytes; its derived syntax flags do not exist.
    const uint16_t format = field<uintptr_t>(p, 0) == bindings().table ? field<uint16_t>(p, 0x100) : 0;
    // Function fields (+100) colorize the complete expression, so scrolling does
    // not change token context: one unwrapped line, then translate/clip the mesh.
    // Number-only fields (+101) need no lexer context and take the plain sliced
    // path that ordinary fields use; the formatter left them wrapped in-game.
    const bool numeric = format && !(format & 0xff);
    if (numeric) report(*state, 1, "number field %p: plain viewport (flags %04x, width %.3f, text %.3f)",
        p, unsigned(format), width, state->view.prefix.back());
    const uint16_t syntax = numeric ? 0 : format;
    const auto visible = syntax ? state->source : state->view.visible();
    const float layout_width = syntax ? std::max(inner, state->view.prefix.back() + padding) : inner;
    const float scroll = syntax ? state->view.prefix[state->view.first] +
        (state->view.first < state->view.glyphs.size() ? state->view.glyphs[state->view.first].kerning : 0) : 0;
    const bool native_reset = field<float>(p, 0x70) != 1 || field<uint32_t>(p, 0x74) != 3 ||
        field<float>(p, 0x78) != layout_width || field<float>(p, 0x7c) != height || field<float>(p, 0x88) != state->scale ||
        (!visible.empty() && !field<uintptr_t>(p, 0x98)) ||
        // Another native path replaced our clipped mesh with its own geometry.
        (state->mesh_ready && field<uintptr_t>(p, 0x98) != state->mesh);
    if (state->mesh_ready && field<uintptr_t>(p, 0x98) != state->mesh)
        report(*state, 2, "field %p: mesh replaced outside the viewport (flags %04x); rebuilding", p, unsigned(format));
    if (!state->mesh_ready || native_reset || state->rendered != visible || state->width != width || state->height != height ||
        state->format != syntax || state->scroll != scroll) {
        state->mesh_ready = false;
        field<float>(p, 0x70) = 1;
        field<uint32_t>(p, 0x74) = 3; // middle left
        field<float>(p, 0x78) = layout_width;
        field<float>(p, 0x7c) = height;
        field<float>(p, 0x88) = state->scale;
        const String slice{visible.c_str(), static_cast<uint32_t>(visible.size()), static_cast<uint32_t>(visible.size() + 1)};
        if (syntax) bindings().original_layout(p, application, &slice);
        else bindings().mesh(static_cast<char*>(p) + 0x70, field<uintptr_t>(reinterpret_cast<void*>(application), 8), &slice);
        if (auto* mesh = field<void*>(p, 0x98)) {
            auto* vertices = field<TextVertex*>(mesh, 0x28);
            const auto count = field<uint32_t>(mesh, 0x4c), capacity = field<uint32_t>(mesh, 0x30);
            if (!vertices || count % 4 || count > visible.size() * 4 || uint64_t(count) * sizeof(TextVertex) > capacity) {
                report(*state, 4, "field %p: unexpected mesh (flags %04x, %u vertices, capacity %u, %zu bytes)",
                    p, unsigned(format), count, capacity, visible.size());
                return false;
            }
            // Clip native glyph quads and UVs before the queued native upload.
            // Includes glyph bearings/overhang and vertical extents, not just advances.
            const float dy = lift(*state, height);
            for (uint32_t i = 0; i < count; i += 4) {
                for (unsigned j = 0; j < 4; ++j) {
                    vertices[i + j].x += (layout_width - inner) * .5f - scroll; vertices[i + j].y += dy;
                    if (state->colored) vertices[i + j].color = state->color;
                }
                clip_text_quad(vertices + i, -inner * .5f, inner * .5f, -height * .5f + .002f, height * .5f - .002f);
            }
        }
        state->width = width; state->height = height; state->rendered = visible;
        state->format = syntax; state->scroll = scroll; state->mesh_ready = true;
        state->mesh = field<uintptr_t>(p, 0x98);
    }
    const auto start = std::min(field<uint32_t>(p, 0x30), field<uint32_t>(p, 0xc0));
    const auto end = static_cast<uint32_t>(std::min<uint64_t>(field<uint32_t>(p, 0xc0), uint64_t(start) + field<uint32_t>(p, 0x44)));
    const float budget = inner - caret_size(state->scale);
    const float left = std::clamp(state->view.position(state->view.index(start)), 0.f, budget);
    const float right = std::clamp(state->view.position(state->view.index(end)), left, budget);
    field<float>(p, 0x3c) = -width * .5f + padding + left;
    field<float>(p, 0x48) = right - left;
    return true;
}
inline void layout(void* p, uintptr_t application, const String* source) {
    if (!eligible(p) || !refresh(p, application)) bindings().original_layout(p, application, source);
}
inline void base_layout(void* p, uintptr_t application, const String* source) {
    if (!eligible(p) || !refresh(p, application)) bindings().original_base_layout(p, application, source);
}
inline void update(void* p, uintptr_t traversal, uintptr_t application) {
    if (!eligible(p) || !refresh(p, application)) { bindings().original_update(p, traversal, application); return; }
    // Retain the native blink/error timers. Its old width loop supplies suffixes
    // to layout and computes the caret over the full string, causing the escape.
    if (++field<uint32_t>(p, 0x34) == 32) { field<uint8_t>(p, 0x38) = !field<uint8_t>(p, 0x38); field<uint32_t>(p, 0x34) = 0; }
    if (field<uint8_t>(p, 0x59) && ++field<uint32_t>(p, 0x54) > 30) { field<uint8_t>(p, 0x59) = 0; field<uint32_t>(p, 0x54) = 0; }
}
struct DrawScope {
    uintptr_t renderer;
    float bottom, height, caret;
    bool selection = false;
    unsigned rectangles = 0;
};
inline thread_local DrawScope* current_draw = nullptr;
inline void rectangle(uintptr_t renderer, const float* transform, uint32_t color, float alpha) {
    auto* scope = current_draw;
    // The signed simple_draw issues background first, then selection/caret.
    // Only those foreground rectangles use text metrics; other UI is untouched.
    if (scope && scope->renderer == renderer && scope->rectangles++ > 0) {
        float adjusted[16];
        std::copy_n(transform, 16, adjusted);
        adjusted[5] = scope->height;
        adjusted[13] = scope->bottom;
        if (scope->rectangles == (scope->selection ? 3u : 2u)) adjusted[0] = scope->caret;
        bindings().original_rectangle(renderer, adjusted, color, alpha);
    } else bindings().original_rectangle(renderer, transform, color, alpha);
}
inline void draw_with(Draw original, void* p, uintptr_t traversal, uintptr_t application, uintptr_t pass, float x, float y) {
    const bool ours = eligible(p);
    const bool ready = !ours || refresh(p, application);
    if (ours && !ready && bindings().log) report(bindings().states[p], 16, "field %p: refresh failed in draw (flags %04x)", p,
        field<uintptr_t>(p, 0) == bindings().table ? unsigned(field<uint16_t>(p, 0x100)) : 0u);
    // A failed geometry contract draws the field background, not unbounded text.
    const auto mesh = field<uintptr_t>(p, 0x98);
    const auto selected = field<uint32_t>(p, 0x44);
    const auto focused = field<uint8_t>(p, 0xe8);
    if (!ready) { field<uintptr_t>(p, 0x98) = 0; field<uint32_t>(p, 0x44) = 0; field<uint8_t>(p, 0xe8) = 0; }
    DrawScope scope{};
    auto* previous = current_draw;
    if (ours && ready) {
        const auto& state = bindings().states.at(p);
        const float height = field<float>(p, 0x14), line = line_height(state, height);
        scope = {application + 0xa0, y + height * .5f - line * .5f + lift(state, height), line, caret_size(state.scale), selected != 0};
        current_draw = &scope;
    } else current_draw = nullptr;
    original(p, traversal, application, pass, x, y);
    current_draw = previous;
    if (!ready) { field<uintptr_t>(p, 0x98) = mesh; field<uint32_t>(p, 0x44) = selected; field<uint8_t>(p, 0xe8) = focused; }
}
inline void draw(void* p, uintptr_t traversal, uintptr_t application, uintptr_t pass, float x, float y) {
    draw_with(bindings().original_draw, p, traversal, application, pass, x, y);
}
inline void base_draw(void* p, uintptr_t traversal, uintptr_t application, uintptr_t pass, float x, float y) {
    draw_with(bindings().original_base_draw, p, traversal, application, pass, x, y);
}
inline uint32_t hit(void* p, const float* position) {
    if (!eligible(p)) return bindings().original_hit(p, position);
    auto* state = metrics(p);
    if (!state) return bindings().original_hit(p, position);
    if (position[0] < padding && state->view.first) --state->view.first;
    else if (position[0] > field<float>(p, 0x10) - padding && state->view.first < state->view.glyphs.size()) ++state->view.first;
    state->view.follow(state->view.byte(state->view.first), field<float>(p, 0x10) - 2 * padding - caret_size(state->scale));
    field<uint32_t>(p, 0x34) = 0; field<uint8_t>(p, 0x38) = 1;
    return state->view.hit(position[0] - padding);
}
inline void color(void* editbox, uintptr_t application, uint32_t value) {
    auto& b = bindings();
    auto* content = editbox ? field<void*>(editbox, 0x78) : nullptr;
    if (!content || !eligible(content)) { b.original_color(editbox, application, value); return; }
    field<uint32_t>(content, 0xd0) = value;
    auto& state = b.states[content];
    state.color = value; state.colored = true; state.mesh_ready = false;
    if (!refresh(content, application)) b.original_color(editbox, application, value);
}
inline void* destroy(void* p, uint64_t flags) {
    bindings().states.erase(p);
    return bindings().original_delete(p, flags);
}
inline void* base_destroy(void* p, uint64_t flags) {
    bindings().states.erase(p);
    return bindings().original_base_delete(p, flags);
}
inline bool prepare(float font_scale, std::map<std::string, uintptr_t> functions) {
    auto& b = bindings();
    b.table = native_find_vtable("editbox_content@c_ui_element_editbox");
    b.base_table = native_find_vtable("mm_ui_element_editbox");
    if (!b.table || !b.base_table) return false;
    const auto* table = reinterpret_cast<const uintptr_t*>(b.table);
    if (table[0] != functions["simple_delete"] || table[1] != functions["simple_init"] || table[3] != functions["simple_update"] ||
        table[4] != functions["simple_draw"] || table[28] != functions["simple_layout"]) return false;
    const auto* base = reinterpret_cast<const uintptr_t*>(b.base_table);
    if (base[0] != functions["base_simple_delete"] || base[1] != functions["simple_init"] ||
        base[3] != functions["simple_update"] || base[4] != functions["base_simple_draw"] ||
        base[28] != functions["base_simple_layout"]) return false;
    b.font_scale = font_scale;
    b.glyph = reinterpret_cast<Glyph>(functions["glyph"]);
    b.kerning = reinterpret_cast<Kerning>(functions["kerning"]);
    b.mesh = reinterpret_cast<Mesh>(functions["simple_mesh"]);
    return hook_address(functions["simple_layout"], reinterpret_cast<void*>(layout), reinterpret_cast<void**>(&b.original_layout)) &&
        hook_address(functions["base_simple_layout"], reinterpret_cast<void*>(base_layout), reinterpret_cast<void**>(&b.original_base_layout)) &&
        hook_address(functions["simple_update"], reinterpret_cast<void*>(update), reinterpret_cast<void**>(&b.original_update)) &&
        hook_address(functions["simple_draw"], reinterpret_cast<void*>(draw), reinterpret_cast<void**>(&b.original_draw)) &&
        hook_address(functions["base_simple_draw"], reinterpret_cast<void*>(base_draw), reinterpret_cast<void**>(&b.original_base_draw)) &&
        hook_address(functions["simple_rectangle"], reinterpret_cast<void*>(rectangle), reinterpret_cast<void**>(&b.original_rectangle)) &&
        hook_address(functions["editbox_color"], reinterpret_cast<void*>(color), reinterpret_cast<void**>(&b.original_color)) &&
        hook_address(functions["simple_hit"], reinterpret_cast<void*>(hit), reinterpret_cast<void**>(&b.original_hit)) &&
        hook_address(functions["simple_delete"], reinterpret_cast<void*>(destroy), reinterpret_cast<void**>(&b.original_delete)) &&
        hook_address(functions["base_simple_delete"], reinterpret_cast<void*>(base_destroy), reinterpret_cast<void**>(&b.original_base_delete));
}
} // namespace swml4::text_fields::single_line
