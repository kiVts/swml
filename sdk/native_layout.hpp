#pragma once
#include "swml_v3.h"
#include <algorithm>
#include <cmath>

namespace swml3::ui {
// Set top-anchored text and fit its containing panel to the native reflow result.
// Native UI y increases upwards; tooltip content extends down from the panel top.
// Call on UPDATE too: the game may restore its own panel bounds each frame. The
// absolute content extent makes repeat calls idempotent and permits text to shrink.
// Width/anchor/position remain native; text wraps at its existing width. Include
// all content above this label in its y offset; bottom_padding is in native units.
inline swml_result fit_panel_text(const swml_api_v3* api, swml_mod* owner,
                                 swml_ui_handle panel, swml_ui_handle label, const char* value,
                                 float bottom_padding = .04f, float minimum_height = 0) {
    if (!api || api->size < sizeof(swml_api_v3)) return SWML_UNAVAILABLE;
    if (!std::isfinite(bottom_padding) || bottom_padding < 0 ||
        !std::isfinite(minimum_height) || minimum_height < 0) return SWML_INVALID;
    swml_ui_info text{sizeof(swml_ui_info)}, background{sizeof(swml_ui_info)};
    auto result=api->ui_inspect(owner,label,&text);
    if (result!=SWML_OK) return result;
    if (text.kind!=SWML_UI_LABEL || text.rect.anchor>SWML_UI_TOP_RIGHT) return SWML_INVALID;
    result=api->ui_inspect(owner,panel,&background);
    if (result!=SWML_OK) return result;
    result=api->ui_set_text(owner,label,value); // native setter performs layout, including wrapping
    if (result!=SWML_OK) return result;
    result=api->ui_inspect(owner,label,&text);
    if (result!=SWML_OK) return result;
    // Native text setters skip layout for an unchanged string. Reapply the label's
    // bounds through the SDK to force reflow if native code reset its height/width.
    result=api->ui_set_rect(owner,label,&text.rect);
    if (result!=SWML_OK) return result;
    result=api->ui_inspect(owner,label,&text);
    if (result!=SWML_OK) return result;
    const float height=std::max(minimum_height,-text.rect.y+text.rect.height+bottom_padding);
    if (!std::isfinite(height) || height<0) return SWML_INVALID;
    if (std::abs(background.rect.height-height)<1e-6f) return SWML_OK;
    background.rect.height=height;
    return api->ui_set_rect(owner,panel,&background.rect);
}
} // namespace swml3::ui
