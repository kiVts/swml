#pragma once
#include "swml_v3.h"
// The signed native constructor's final byte controls measured-height reflow
// (+7c). Its layout function copies wrapped height +94 into the base height +14
// only when that byte is set. Height zero is the SDK opt-in for auto height.
template<class Constructor, class String>
void* construct_native_text(Constructor constructor, void* object, const swml_ui_desc& d, String* text)
{
    const auto& r = d.rect;
    return constructor(object, r.x, r.y, r.width, r.height, r.anchor, text, d.text_align,
        d.foreground, .0005f, d.font_size, r.height == 0 ? uint8_t(1) : uint8_t(0));
}
