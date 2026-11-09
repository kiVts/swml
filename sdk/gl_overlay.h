#pragma once
#include <windows.h>
#include <cstdint>

// Minimal 2D overlay renderer on top of the game's OpenGL context (core or compatibility).
// Coordinates are window pixels, origin bottom-left. Lines are drawn as triangles so the width
// works on every driver. Saves and restores all GL state it touches.
namespace overlay {

struct Color {
    float r, g, b, a;
};

bool begin(HDC dc, int* width, int* height); // false if GL isn't usable this frame
void line(float x0, float y0, float x1, float y1, float width_px, Color c);
void end(); // draws everything queued since begin() and restores GL state

// Stroke-font glyph (A-Z, 0-9, / - = ? .; lower case drawn as upper case), centred on (x, y).
void glyph(char ch, float x, float y, float size_px, float width_px, Color c);
// A short string of those glyphs, centred on (x, y).
void text(const char* s, float x, float y, float size_px, float width_px, Color c);

} // namespace overlay
