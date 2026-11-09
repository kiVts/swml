#pragma once
#include <windows.h>

// Real text on top of the game's OpenGL frame: the string is rendered once with a TrueType font through GDI
// (antialiased), kept as a texture, and drawn as a quad with a dark shadow. Call from a present callback (render
// thread, the game's GL context current). Saves and restores all GL state it touches. Not thread-safe.
namespace gltext {

struct Color {
    float r, g, b, a;
};

// Draws `text` (UTF-8) centred horizontally on x, its top `top_px` below the window's top edge; height_px is the font
// size. Returns false if GL isn't usable (the reason is logged once).
bool draw_top(HDC dc, const char* text, int height_px, int top_px, Color c);

} // namespace gltext
