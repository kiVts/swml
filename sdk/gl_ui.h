#pragma once
// A small immediate-mode 2D renderer for custom menus on the game's OpenGL frame: antialiased rounded rectangles with
// borders and soft shadows, and TrueType text placed by its baseline and measured exactly. Coordinates are window
// pixels from the top left. Call from a present callback (render thread, the game's GL context current), between
// begin() and end(), which save and restore every piece of GL state they touch. Not thread-safe.
//
// It draws over the whole frame, cursor included, so a menu that covers the game's own cursor has to draw one.
#include <cstdint>

namespace glui {

struct Color {
    float r, g, b, a;
};
inline constexpr Color rgb(uint32_t hex, float a = 1.0f)
{
    return {((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a};
}

enum class Weight { Regular, Semibold };
enum class Align { Left, Center, Right };

bool begin(int window_width, int window_height); // false if GL isn't usable (the reason is logged once)
void end();

// A rounded rectangle, with an optional inner border.
void rect(float x, float y, float w, float h, float radius, Color fill, float border = 0.0f, Color border_color = {});
// A soft shadow around a rounded rectangle, blurred over `blur` pixels.
void shadow(float x, float y, float w, float h, float radius, float blur, Color color);
void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color color);
// An area graph filling a rounded rectangle from side to side: `count` values spread evenly over its width, `lo` at its
// bottom and `hi` at its top. At most kGraphMaxValues values.
inline constexpr int kGraphMaxValues = 96;
void area_graph(float x, float y, float w, float h, float radius, const float* values, int count, float lo, float hi, Color fill, Color line);

// `baseline` is the y of the text's baseline, so different sizes on one line stay aligned. UTF-8.
void text(const char* s, float x, float baseline, float size_px, Weight weight, Color color, Align align = Align::Left);
float text_width(const char* s, float size_px, Weight weight);

} // namespace glui
