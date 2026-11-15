#pragma once
#include <cstdint>

// The native UI context is mm_ui +50. Its +188 slot is mouse hover;
// +190 is keyboard focus (mm_ui +1e0). Native focus dispatch and cleanup
// establish this distinction; do not infer keyboard focus from mouse hover.
inline void* native_ui_keyboard_focus(uintptr_t application) {
    return application ? *reinterpret_cast<void**>(application + 0x190) : nullptr;
}
template<class IsA> inline bool native_ui_text_element(void* element, IsA is_a) {
    return element && (is_a(element, "mm_ui_element_editbox") || is_a(element, "c_ui_element_editbox") ||
        is_a(element, "c_ui_element_editbox_adv_text"));
}
template<class IsA> inline bool native_ui_text_entry(uintptr_t application, IsA is_a) {
    return native_ui_text_element(native_ui_keyboard_focus(application), is_a);
}
// Original and current PEs: focused-key dispatch in the vehicle editor.
// Only the two context/object offsets and branch destinations are variable.
inline constexpr const char* kEditorKeyboardFocusPattern =
    "48 8B 8F ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 4C 8D 8F ?? ?? ?? ?? "
    "4C 8D 87 ?? ?? ?? ?? 44 89 74 24 28 89 74 24 20 49 8B D4 FF 90 A0 00 00 00 84 C0 74 ??";
template<class IsA> inline bool native_editor_text_entry(void* editor, uint32_t focus_offset, IsA is_a) {
    return editor && focus_offset && native_ui_text_element(
        *reinterpret_cast<void**>(static_cast<char*>(editor) + focus_offset), is_a);
}
