#pragma once
#include "game.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace swml4::properties {

struct NativeString {
    const char* data;
    uint32_t length, capacity;
};
inline NativeString borrowed(const char* s) {
    auto n = static_cast<uint32_t>(strlen(s));
    return {s, n, n + 1};
}

struct Option {
    const char* label;
    int value;
};

namespace detail {
inline constexpr size_t kCheckboxVtableSlots = 38, kCheckboxWriteSlot = 37;
inline constexpr size_t kBindingCount = 1024; // more than any open panel: old rows are gone before reuse

// A checkbox writes a bool. A binding gives it one and copies the result into mask bits [first, last].
struct FlagBinding {
    bool value;
    uint64_t* mask;
    uint32_t first, last;
};
inline bool read_range(const uint64_t* mask, uint32_t first, uint32_t last) {
    for (uint32_t bit = first; bit <= last; ++bit)
        if (mask[bit / 64] >> (bit % 64) & 1) return true;
    return false;
}
inline void write_range(uint64_t* mask, uint32_t first, uint32_t last, bool on) {
    for (uint32_t bit = first; bit <= last; ++bit) {
        const uint64_t one = uint64_t(1) << (bit % 64);
        if (on) mask[bit / 64] |= one;
        else mask[bit / 64] &= ~one;
    }
}

struct Bindings {
    using CheckboxWrite = void (*)(void*);
    using Section = void (*)(void*, void*, const NativeString*, void*);
    using Slider = void (*)(void*, void*, const NativeString*, NativeString*, float, float, float, float*, float);
    using Dropdown = char* (*)(void*, void*, const NativeString*, int*);
    using AddOption = void (*)(void*, void*, int, const NativeString*);
    using SetText = void (*)(void*, void*, const NativeString*);
    using Checkbox = void (*)(void*, void*, const NativeString*, bool*);
    using Label = void* (*)(void*, void*, const NativeString*, const NativeString*);
    using Reserve = void (*)(NativeString*, uint32_t);

    Section section = nullptr;
    Slider slider = nullptr;
    Dropdown dropdown = nullptr;
    AddOption add_option = nullptr;
    SetText set_text = nullptr;
    Checkbox checkbox = nullptr;
    Label label = nullptr;
    Reserve reserve = nullptr;
    CheckboxWrite checkbox_write = nullptr;
    void* flag_vtable[kCheckboxVtableSlots + 1] = {};
    bool flag_vtable_ready = false;
    FlagBinding flags[kBindingCount] = {};
    std::atomic<uint32_t> next_flag{0};
};
inline Bindings& bindings() {
    static Bindings b;
    return b;
}
inline void flag_write(void* element) {
    auto& b = bindings();
    b.checkbox_write(element);
    auto* binding = *reinterpret_cast<FlagBinding**>(static_cast<char*>(element) + 0x68);
    write_range(binding->mask, binding->first, binding->last, binding->value);
}
} // namespace detail

// Resolve the native builders once. False: leave the stock panel in place.
inline bool bind() {
    auto& b = detail::bindings();
    b.section = game_ptr<detail::Bindings::Section>(0x1408c57f0);
    b.slider = game_ptr<detail::Bindings::Slider>(0x1408b3890);
    b.dropdown = game_ptr<detail::Bindings::Dropdown>(0x14090b520);
    b.add_option = game_ptr<detail::Bindings::AddOption>(0x1408bb160);
    b.set_text = game_ptr<detail::Bindings::SetText>(0x140244e50);
    b.checkbox = game_ptr<detail::Bindings::Checkbox>(0x1408c5f40);
    b.label = game_ptr<detail::Bindings::Label>(0x1408e5e90);
    b.reserve = game_ptr<detail::Bindings::Reserve>(0x14017cbb0);
    b.checkbox_write = game_ptr<detail::Bindings::CheckboxWrite>(0x1402e0fa0);
    return b.section && b.slider && b.dropdown && b.add_option && b.set_text && b.checkbox && b.label &&
        b.reserve && b.checkbox_write;
}
inline bool available() {
    auto& b = detail::bindings();
    return b.section && b.slider && b.dropdown && b.add_option && b.set_text && b.checkbox && b.label &&
        b.reserve && b.checkbox_write;
}

// One panel build. Use it only inside the component's panel function.
class Panel {
    void* ctx_;
    char* native_;
    template <class T> T& at(size_t offset) const { return *reinterpret_cast<T*>(native_ + offset); }
    void* last_row() const {
        const uint32_t count = at<uint32_t>(0x18), capacity = at<uint32_t>(0x10), head = at<uint32_t>(0x14);
        if (!count || !capacity) return nullptr;
        return at<void**>(0x08)[(uint64_t(head) + count - 1) % capacity];
    }
public:
    Panel(void* ctx, void* native_panel) : ctx_(ctx), native_(static_cast<char*>(native_panel)) {}

    // Rows after this go inside a collapsible section, closed until the player clicks its title.
    void section(const char* title) {
        const auto name = borrowed(title);
        detail::bindings().section(native_, ctx_, &name, nullptr);
    }
    void end_section() { at<void*>(0x28) = at<void*>(0x30); }

    // The native slider, with "%.0f%%" style formatting. value = field * display_scale when shown.
    void slider(const char* label, const char* format, float low, float high, float step, float* value, float scale) {
        auto& b = detail::bindings();
        const auto name = borrowed(label);
        // The game frees this one, so it must come from the game's allocator.
        const auto length = static_cast<uint32_t>(strlen(format));
        NativeString owned{nullptr, length, 0};
        b.reserve(&owned, length + 1);
        memcpy(const_cast<char*>(owned.data), format, length + 1);
        b.slider(native_, ctx_, &name, &owned, low, high, step, value, scale);
    }

    // The native dropdown, bound to an int. A value with no option shows the first option.
    void dropdown(const char* label, int* value, std::initializer_list<Option> options) {
        auto& b = detail::bindings();
        const auto name = borrowed(label);
        char* list = b.dropdown(native_, ctx_, &name, value);
        for (const auto& option : options) {
            const auto text = borrowed(option.label);
            b.add_option(list, ctx_, option.value, &text);
        }
        uint32_t index = 0;
        for (const auto& option : options) {
            if (option.value == *value) {
                auto field = [&](size_t o) -> uint32_t& { return *reinterpret_cast<uint32_t*>(list + o); };
                field(0xd0) = index;
                const auto text = borrowed(option.label);
                b.set_text(*reinterpret_cast<void**>(list + 0x80), ctx_, &text);
                break;
            }
            ++index;
        }
    }

    // The native checkmark, bound to a bool.
    void checkbox(const char* label, bool* value) {
        const auto name = borrowed(label);
        detail::bindings().checkbox(native_, ctx_, &name, value);
    }

    // A checkmark bound to bit (or bits first..last) of a uint64_t[2] mask. It reads as checked if
    // any bit is set, and writes them all. Returns false, adding no row, if the game's checkbox has changed.
    bool flag(const char* label, uint64_t* mask, uint32_t first, uint32_t last = UINT32_MAX) {
        auto& b = detail::bindings();
        if (last == UINT32_MAX) last = first;
        if (first > last || last >= 128) return false;
        auto& binding = b.flags[b.next_flag.fetch_add(1, std::memory_order_relaxed) % detail::kBindingCount];
        binding = {detail::read_range(mask, first, last), mask, first, last};
        const auto name = borrowed(label);
        b.checkbox(native_, ctx_, &name, &binding.value);
        auto* row = static_cast<void***>(last_row());
        if (!row) return false;
        void** stock = *row;
        if (!b.flag_vtable_ready) {
            if (stock[detail::kCheckboxWriteSlot] != reinterpret_cast<void*>(b.checkbox_write)) return false;
            memcpy(b.flag_vtable, stock - 1, sizeof(b.flag_vtable));
            b.flag_vtable[detail::kCheckboxWriteSlot + 1] = reinterpret_cast<void*>(&detail::flag_write);
            b.flag_vtable_ready = true;
        }
        *row = b.flag_vtable + 1;
        return true;
    }

    // A description line, wrapped by the game: an optional coloured left caption, then the text.
    void text(const char* body, const char* caption = "") {
        const auto left = borrowed(caption), right = borrowed(body);
        detail::bindings().label(native_, ctx_, &left, &right);
    }
};

} // namespace swml4::properties
