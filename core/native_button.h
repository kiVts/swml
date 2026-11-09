#pragma once
#include <cstdint>

// The button owns a rectangle/rounded-rectangle child. Native method 35 resizes
// that child and rebuilds/uploads its geometry. Writing only the outer bounds
// makes ui_inspect report a new size while the visible background stays old.
template<class Resize>
bool resize_native_button(Resize resize, void* object, uintptr_t application,
                          float old_width, float old_height, float width, float height)
{
    if (old_width == width && old_height == height) return false;
    resize(object, application, width, height);
    return true;
}
