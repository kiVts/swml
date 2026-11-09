#pragma once
#include "sdk_v2.hpp"
#include "swml_v3.h"
#include "native_layout.hpp"
namespace swml3 {
inline const swml_api_v3* api() { return swml_get_v3(swml()); }
inline swml_result log(uint32_t level, const char* text) {
    return api() ? api()->log(swml_self(), level, text) : SWML_UNAVAILABLE;
}
inline swml_result setting(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    return api() ? api()->setting_write(swml_self(), section, key, value) : SWML_UNAVAILABLE;
}
inline swml_result command(const char* name, const char* help, swml_callback callback, void* context, swml_handle& handle) {
    handle = 0;
    return api() ? api()->command_register(swml_self(), name, help, callback, context, &handle) : SWML_UNAVAILABLE;
}
inline swml_result run(const char* name, const char* arguments = "") {
    return api() ? api()->command_run(swml_self(), name, arguments) : SWML_UNAVAILABLE;
}
namespace ui {
inline swml_ui_desc description(uint32_t kind, swml_ui_rect rect, const char* text = "") {
    return {sizeof(swml_ui_desc), kind, rect, text, 0xffe6e6e6, 0xff27292a, 0xffb49140, 0xff5678c0, 0.8f, SWML_UI_CENTER, 1, 1};
}
inline swml_result watch(const char* native_class, swml_ui_callback callback, void* context, swml_handle& handle) {
    handle = 0;
    return api() ? api()->ui_watch(swml_self(), native_class, callback, context, &handle) : SWML_UNAVAILABLE;
}
inline swml_result unwatch(swml_handle& handle) {
    if (!handle) return SWML_OK;
    auto r = api() ? api()->ui_unwatch(swml_self(), handle) : SWML_UNAVAILABLE;
    if (r == SWML_OK || r == SWML_NOT_FOUND) handle = 0;
    return r;
}
inline swml_result create(swml_ui_handle parent, const swml_ui_desc& desc, swml_ui_handle& result) {
    result = 0;
    return api() ? api()->ui_create(swml_self(), parent, &desc, &result) : SWML_UNAVAILABLE;
}
inline swml_result destroy(swml_ui_handle handle) {
    return api() ? api()->ui_destroy(swml_self(), handle) : SWML_UNAVAILABLE;
}
inline swml_result text(swml_ui_handle handle, const char* value) {
    return api() ? api()->ui_set_text(swml_self(), handle, value) : SWML_UNAVAILABLE;
}
inline swml_result text_in_panel(swml_ui_handle panel, swml_ui_handle label, const char* value,
                                 float bottom_padding = .04f, float minimum_height = 0) {
    return fit_panel_text(api(), swml_self(), panel, label, value, bottom_padding, minimum_height);
}
inline swml_result visible(swml_ui_handle handle, bool value) {
    return api() ? api()->ui_set_visible(swml_self(), handle, value) : SWML_UNAVAILABLE;
}
inline swml_result enabled(swml_ui_handle handle, bool value) {
    return api() ? api()->ui_set_enabled(swml_self(), handle, value) : SWML_UNAVAILABLE;
}
inline swml_result rect(swml_ui_handle handle, const swml_ui_rect& value) {
    return api() ? api()->ui_set_rect(swml_self(), handle, &value) : SWML_UNAVAILABLE;
}
inline swml_result find(swml_ui_handle root, const char* native_class, swml_ui_handle& result, uint32_t occurrence = 0) {
    result = 0;
    return api() ? api()->ui_find_class(swml_self(), root, native_class, occurrence, &result) : SWML_UNAVAILABLE;
}

// Native text has no outline, so this draws one: four copies of the text, offset around it. Create it
// before the label it surrounds, because native children draw in creation order.
class TextOutline {
    static constexpr float kDirections[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    swml_ui_handle copies_[4] = {};
    float thickness_ = 0;
public:
    void forget() { *this = TextOutline{}; }
    // desc: as the label, with foreground set to the outline's colour. thickness is in native UI units.
    swml_result create(swml_ui_handle parent, const swml_ui_desc& desc, float thickness) {
        thickness_ = thickness;
        for (auto& copy : copies_) {
            const auto result = ui::create(parent, desc, copy);
            if (result != SWML_OK) return result;
        }
        return SWML_OK;
    }
    void text(const char* value) { for (auto copy : copies_) if (copy) ui::text(copy, value); }
    void visible(bool value) { for (auto copy : copies_) if (copy) ui::visible(copy, value); }
    void rect(swml_ui_rect value) {
        for (int i = 0; i < 4; i++) {
            if (!copies_[i]) continue;
            auto offset = value;
            offset.x += kDirections[i][0] * thickness_;
            offset.y += kDirections[i][1] * thickness_;
            ui::rect(copies_[i], offset);
        }
    }
};
}
}
