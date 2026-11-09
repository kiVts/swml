#include "gl_ui.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace glui {
namespace {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLchar = char;
using GLsizeiptr = intptr_t;
using GLfloat = float;

constexpr GLenum GL_TRIANGLES = 0x0004, GL_FLOAT = 0x1406, GL_BLEND = 0x0BE2, GL_DEPTH_TEST = 0x0B71, GL_CULL_FACE = 0x0B44,
                 GL_SCISSOR_TEST = 0x0C11, GL_STENCIL_TEST = 0x0B90, GL_SRC_ALPHA = 0x0302, GL_ONE = 1,
                 GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_VIEWPORT = 0x0BA2, GL_DEPTH_WRITEMASK = 0x0B72, GL_COLOR_WRITEMASK = 0x0C23,
                 GL_CURRENT_PROGRAM = 0x8B8D, GL_VERTEX_ARRAY_BINDING = 0x85B5, GL_ARRAY_BUFFER_BINDING = 0x8894,
                 GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6, GL_ARRAY_BUFFER = 0x8892, GL_STREAM_DRAW = 0x88E0,
                 GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81,
                 GL_LINK_STATUS = 0x8B82, GL_BLEND_SRC_RGB = 0x80C9, GL_BLEND_DST_RGB = 0x80C8, GL_BLEND_SRC_ALPHA = 0x80CB,
                 GL_BLEND_DST_ALPHA = 0x80CA, GL_BLEND_EQUATION_RGB = 0x8009, GL_BLEND_EQUATION_ALPHA = 0x883D, GL_FUNC_ADD = 0x8006,
                 GL_FRAMEBUFFER_SRGB = 0x8DB9, GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_ACTIVE_TEXTURE = 0x84E0,
                 GL_TEXTURE_BINDING_2D = 0x8069, GL_SAMPLER_BINDING = 0x8919, GL_PIXEL_UNPACK_BUFFER = 0x88EC,
                 GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_UNPACK_ROW_LENGTH = 0x0CF2,
                 GL_RGBA8 = 0x8058, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_TEXTURE_MIN_FILTER = 0x2801,
                 GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_LINEAR = 0x2601,
                 GL_NEAREST = 0x2600, GL_CLAMP_TO_EDGE = 0x812F;

#define GLUI_FUNCTIONS(X)                                                                                            \
    X(void, glGetIntegerv, GLenum, GLint*)                                                                           \
    X(GLboolean, glIsEnabled, GLenum)                                                                                \
    X(void, glEnable, GLenum)                                                                                        \
    X(void, glDisable, GLenum)                                                                                       \
    X(void, glViewport, GLint, GLint, GLsizei, GLsizei)                                                              \
    X(void, glDrawArrays, GLenum, GLint, GLsizei)                                                                    \
    X(void, glDepthMask, GLboolean)                                                                                  \
    X(void, glColorMask, GLboolean, GLboolean, GLboolean, GLboolean)                                                 \
    X(void, glBlendFuncSeparate, GLenum, GLenum, GLenum, GLenum)                                                     \
    X(void, glBlendEquationSeparate, GLenum, GLenum)                                                                 \
    X(GLuint, glCreateShader, GLenum)                                                                                \
    X(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)                                     \
    X(void, glCompileShader, GLuint)                                                                                 \
    X(void, glGetShaderiv, GLuint, GLenum, GLint*)                                                                   \
    X(void, glDeleteShader, GLuint)                                                                                  \
    X(GLuint, glCreateProgram)                                                                                       \
    X(void, glAttachShader, GLuint, GLuint)                                                                          \
    X(void, glBindAttribLocation, GLuint, GLuint, const GLchar*)                                                     \
    X(void, glLinkProgram, GLuint)                                                                                   \
    X(void, glGetProgramiv, GLuint, GLenum, GLint*)                                                                  \
    X(void, glUseProgram, GLuint)                                                                                    \
    X(GLint, glGetUniformLocation, GLuint, const GLchar*)                                                            \
    X(void, glUniform1i, GLint, GLint)                                                                               \
    X(void, glUniform2f, GLint, GLfloat, GLfloat)                                                                    \
    X(void, glUniform1f, GLint, GLfloat)                                                                             \
    X(void, glUniform1fv, GLint, GLsizei, const GLfloat*)                                                            \
    X(void, glGenVertexArrays, GLsizei, GLuint*)                                                                     \
    X(void, glBindVertexArray, GLuint)                                                                               \
    X(void, glGenBuffers, GLsizei, GLuint*)                                                                          \
    X(void, glBindBuffer, GLenum, GLuint)                                                                            \
    X(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum)                                                   \
    X(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)                           \
    X(void, glEnableVertexAttribArray, GLuint)                                                                       \
    X(void, glBindFramebuffer, GLenum, GLuint)                                                                       \
    X(void, glActiveTexture, GLenum)                                                                                 \
    X(void, glGenTextures, GLsizei, GLuint*)                                                                         \
    X(void, glDeleteTextures, GLsizei, const GLuint*)                                                                \
    X(void, glBindTexture, GLenum, GLuint)                                                                           \
    X(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)                \
    X(void, glTexParameteri, GLenum, GLenum, GLint)                                                                  \
    X(void, glPixelStorei, GLenum, GLint)

#define X(ret, name, ...)                       \
    using name##_t = ret(WINAPI*)(__VA_ARGS__); \
    name##_t p##name = nullptr;
GLUI_FUNCTIONS(X)
#undef X
using glBindSampler_t = void(WINAPI*)(GLuint, GLuint);
glBindSampler_t pglBindSampler = nullptr;

// Position in pixels; p, half, radius and softness in the rectangle's own pixels; fill and border colours.
struct Vertex {
    float x, y, px, py, hx, hy, radius, soft, border;
    float fill[4], line[4];
};
constexpr int kAttributes = 7;

const char* kRectVs = R"(
in vec2 a_pos; in vec2 a_p; in vec2 a_half; in vec3 a_shape; in vec4 a_fill; in vec4 a_line;
uniform vec2 u_size;
out vec2 v_p; out vec2 v_half; out vec3 v_shape; out vec4 v_fill; out vec4 v_line;
void main() {
    v_p = a_p; v_half = a_half; v_shape = a_shape; v_fill = a_fill; v_line = a_line;
    gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);
})";
const char* kRectFs = R"(
in vec2 v_p; in vec2 v_half; in vec3 v_shape; in vec4 v_fill; in vec4 v_line;
out vec4 o_color;
void main() {
    float radius = v_shape.x, soft = v_shape.y, border = v_shape.z;
    if (v_half.x <= 0.0) { // a triangle: distances to its edges, in v_p and v_half.y
        float edge = min(min(v_p.x, v_p.y), v_half.y);
        o_color = vec4(v_fill.rgb, v_fill.a * clamp(edge + 0.5, 0.0, 1.0));
        return;
    }
    vec2 q = abs(v_p) - v_half + vec2(radius);
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
    float alpha = 1.0 - smoothstep(-soft - 0.5, soft + 0.5, d);
    vec4 color = v_fill;
    if (border > 0.0) color = mix(v_fill, v_line, smoothstep(-border - 0.5, -border + 0.5, d));
    o_color = vec4(color.rgb, color.a * alpha);
})";
// Piecewise linear over the values. The distance to the line, not the height above it, keeps slopes evenly antialiased.
const char* kGraphFs = R"(
in vec2 v_p; in vec2 v_half; in vec3 v_shape; in vec4 v_fill; in vec4 v_line;
uniform float u_values[96]; uniform float u_count; uniform float u_lo; uniform float u_hi;
out vec4 o_color;
void main() {
    float radius = v_shape.x, thickness = v_shape.y;
    vec2 q = abs(v_p) - v_half + vec2(radius);
    float box = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
    float clip = 1.0 - smoothstep(-0.5, 0.5, box);
    float steps = max(u_count - 1.0, 1.0);
    float along = clamp((v_p.x + v_half.x) / (2.0 * v_half.x), 0.0, 1.0) * steps;
    int i = int(min(floor(along), steps - 1.0));
    float a = u_values[i], b = u_values[int(u_count) > 1 ? i + 1 : i];
    float scale = 2.0 * v_half.y / (u_hi - u_lo);
    float value = mix(a, b, along - float(i));
    float line_y = v_half.y - (value - u_lo) * scale;
    float slope = (b - a) * scale / (2.0 * v_half.x / steps);
    float d = (v_p.y - line_y) / sqrt(1.0 + slope * slope);
    float under = clamp(d + 0.5, 0.0, 1.0);
    float stroke = clamp(thickness * 0.5 + 0.5 - abs(d), 0.0, 1.0);
    float fill_a = v_fill.a * under;
    float line_a = v_line.a * stroke;
    float alpha = line_a + fill_a * (1.0 - line_a);
    vec3 color = alpha > 0.0 ? (v_line.rgb * line_a + v_fill.rgb * fill_a * (1.0 - line_a)) / alpha : v_fill.rgb;
    o_color = vec4(color, alpha * clip);
})";
const char* kTextVs = R"(
in vec2 a_pos; in vec2 a_p; in vec4 a_fill;
uniform vec2 u_size;
out vec2 v_uv; out vec4 v_color;
void main() {
    v_uv = a_p; v_color = a_fill;
    gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);
})";
const char* kTextFs = R"(
in vec2 v_uv; in vec4 v_color;
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    o_color = vec4(v_color.rgb, v_color.a * pow(texture(u_tex, v_uv).a, 0.85));
})";

struct Texture {
    GLuint id = 0;
    int width = 0, height = 0, pad = 0, ascent = 0;
    float advance = 0;
    uint64_t used = 0;
};
constexpr size_t kMaxTextures = 512;

bool g_loaded = false, g_failed = false;
GLuint g_rect_program = 0, g_text_program = 0, g_graph_program = 0, g_vao = 0, g_vbo = 0;
GLint g_rect_size = -1, g_text_size = -1, g_text_tex = -1, g_graph_size = -1, g_graph_values = -1, g_graph_count = -1, g_graph_lo = -1, g_graph_hi = -1;
std::unordered_map<std::string, Texture> g_textures;
std::vector<Vertex> g_batch;
uint64_t g_frame = 0;
int g_width = 0, g_height = 0;

// Saved around begin()..end().
GLint s_program, s_vao, s_vbo, s_fbo, s_viewport[4], s_src_rgb, s_dst_rgb, s_src_a, s_dst_a, s_eq_rgb, s_eq_a, s_depth_mask, s_color_mask[4];
GLint s_active, s_tex, s_unpack, s_align, s_row, s_sampler;
GLboolean s_blend, s_depth, s_cull, s_scissor, s_stencil, s_srgb;

void* gl_proc(const char* name)
{
    using wglGetProcAddress_t = PROC(WINAPI*)(LPCSTR);
    static HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    static auto wgl_get = reinterpret_cast<wglGetProcAddress_t>(GetProcAddress(gl, "wglGetProcAddress"));
    void* p = wgl_get ? reinterpret_cast<void*>(wgl_get(name)) : nullptr;
    if (reinterpret_cast<uintptr_t>(p) <= 3 || reinterpret_cast<intptr_t>(p) == -1) p = reinterpret_cast<void*>(GetProcAddress(gl, name));
    return p;
}
GLuint compile(GLenum type, const std::string& source)
{
    const GLuint shader = pglCreateShader(type);
    const char* text = source.c_str();
    pglShaderSource(shader, 1, &text, nullptr);
    pglCompileShader(shader);
    GLint ok = 0;
    pglGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    return ok ? shader : 0;
}
GLuint link(const char* version, const char* vs, const char* fs, const char* const* attributes, int count)
{
    const GLuint v = compile(GL_VERTEX_SHADER, std::string(version) + vs), f = compile(GL_FRAGMENT_SHADER, std::string(version) + fs);
    if (!v || !f) return 0;
    const GLuint program = pglCreateProgram();
    pglAttachShader(program, v);
    pglAttachShader(program, f);
    for (int i = 0; i < count; i++) pglBindAttribLocation(program, i, attributes[i]);
    pglLinkProgram(program);
    pglDeleteShader(v);
    pglDeleteShader(f);
    GLint ok = 0;
    pglGetProgramiv(program, GL_LINK_STATUS, &ok);
    return ok ? program : 0;
}
bool load()
{
    g_loaded = true;
    g_failed = true;
#define X(ret, name, ...)                                  \
    p##name = reinterpret_cast<name##_t>(gl_proc(#name));  \
    if (!p##name) {                                        \
        swlog("menu renderer: missing GL function %s - off", #name); \
        return false;                                      \
    }
    GLUI_FUNCTIONS(X)
#undef X
    pglBindSampler = reinterpret_cast<glBindSampler_t>(gl_proc("glBindSampler"));
    const char* rect_attributes[] = {"a_pos", "a_p", "a_half", "a_shape", "a_fill", "a_line"};
    const char* text_attributes[] = {"a_pos", "a_p", "a_unused", "a_unused2", "a_fill"};
    for (const char* version : {"#version 330 core\n", "#version 150\n"}) {
        g_rect_program = link(version, kRectVs, kRectFs, rect_attributes, 6);
        g_text_program = link(version, kTextVs, kTextFs, text_attributes, 5);
        g_graph_program = link(version, kRectVs, kGraphFs, rect_attributes, 6);
        if (g_rect_program && g_text_program && g_graph_program) {
            g_graph_size = pglGetUniformLocation(g_graph_program, "u_size");
            g_graph_values = pglGetUniformLocation(g_graph_program, "u_values");
            g_graph_count = pglGetUniformLocation(g_graph_program, "u_count");
            g_graph_lo = pglGetUniformLocation(g_graph_program, "u_lo");
            g_graph_hi = pglGetUniformLocation(g_graph_program, "u_hi");
            swlog("menu renderer: ready (%.*s)", static_cast<int>(strlen(version) - 1), version);
            g_rect_size = pglGetUniformLocation(g_rect_program, "u_size");
            g_text_size = pglGetUniformLocation(g_text_program, "u_size");
            g_text_tex = pglGetUniformLocation(g_text_program, "u_tex");
            pglGenVertexArrays(1, &g_vao);
            pglGenBuffers(1, &g_vbo);
            g_failed = false;
            return true;
        }
    }
    swlog("menu renderer: couldn't build shaders - off");
    return false;
}

void bind_vertices()
{
    pglBindVertexArray(g_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    const GLsizei stride = sizeof(Vertex);
    const auto offset = [](size_t bytes) { return reinterpret_cast<const void*>(bytes); };
    pglVertexAttribPointer(0, 2, GL_FLOAT, 0, stride, offset(offsetof(Vertex, x)));
    pglVertexAttribPointer(1, 2, GL_FLOAT, 0, stride, offset(offsetof(Vertex, px)));
    pglVertexAttribPointer(2, 2, GL_FLOAT, 0, stride, offset(offsetof(Vertex, hx)));
    pglVertexAttribPointer(3, 3, GL_FLOAT, 0, stride, offset(offsetof(Vertex, radius)));
    pglVertexAttribPointer(4, 4, GL_FLOAT, 0, stride, offset(offsetof(Vertex, fill)));
    pglVertexAttribPointer(5, 4, GL_FLOAT, 0, stride, offset(offsetof(Vertex, line)));
    for (GLuint i = 0; i < 6; i++) pglEnableVertexAttribArray(i);
}
void flush()
{
    if (g_batch.empty()) return;
    pglUseProgram(g_rect_program);
    pglUniform2f(g_rect_size, static_cast<float>(g_width), static_cast<float>(g_height));
    pglBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(g_batch.size() * sizeof(Vertex)), g_batch.data(), GL_STREAM_DRAW);
    pglDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(g_batch.size()));
    g_batch.clear();
}
void push_quad(float x, float y, float w, float h, float px, float py, float hx, float hy, float radius, float soft, float border, Color fill, Color line)
{
    const auto vertex = [&](float vx, float vy) {
        Vertex v{vx, vy, vx - (x + w * 0.5f) + px, vy - (y + h * 0.5f) + py, hx, hy, radius, soft, border, {fill.r, fill.g, fill.b, fill.a}, {line.r, line.g, line.b, line.a}};
        g_batch.push_back(v);
    };
    vertex(x, y);
    vertex(x + w, y);
    vertex(x, y + h);
    vertex(x + w, y);
    vertex(x + w, y + h);
    vertex(x, y + h);
}

std::wstring widen(const char* s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring result(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, result.data(), n);
    return result;
}
// Text is rasterized at four times its size, where hinting no longer bends it to the pixel grid, and averaged down.
constexpr int kTextOversample = 4;
HFONT make_font(int px, Weight weight)
{
    return CreateFontW(-px * kTextOversample, 0, 0, 0, weight == Weight::Semibold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
}
// Widths are asked for every frame, from either thread, and need GDI only the first time.
std::mutex g_width_mutex;
std::unordered_map<std::string, float> g_widths;
float measure(const char* s, int px, Weight weight)
{
    std::string key = std::to_string(px) + (weight == Weight::Semibold ? "s:" : "r:") + s;
    std::lock_guard lock(g_width_mutex);
    const auto found = g_widths.find(key);
    if (found != g_widths.end()) return found->second;
    const std::wstring text = widen(s);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = make_font(px, weight);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);
    const float width = static_cast<float>(size.cx) / kTextOversample;
    if (g_widths.size() >= 4096) g_widths.clear();
    g_widths.emplace(std::move(key), width);
    return width;
}
int rounded_px(float size_px) { return std::max(6, static_cast<int>(std::lround(size_px))); }

Texture* text_texture(const char* s, int px, Weight weight)
{
    std::string key = std::to_string(px) + (weight == Weight::Semibold ? "s:" : "r:") + s;
    auto found = g_textures.find(key);
    if (found != g_textures.end()) {
        found->second.used = g_frame;
        return &found->second;
    }
    if (g_textures.size() >= kMaxTextures) {
        for (auto it = g_textures.begin(); it != g_textures.end();) {
            if (g_frame - it->second.used > 600) {
                pglDeleteTextures(1, &it->second.id);
                it = g_textures.erase(it);
            } else ++it;
        }
        if (g_textures.size() >= kMaxTextures) return nullptr;
    }
    const std::wstring wide = widen(s);
    if (wide.empty()) return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = make_font(px, weight);
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &size);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    Texture texture;
    texture.pad = 2;
    texture.width = (size.cx + kTextOversample - 1) / kTextOversample + texture.pad * 2;
    texture.height = (size.cy + kTextOversample - 1) / kTextOversample + texture.pad * 2;
    texture.ascent = (metrics.tmAscent + kTextOversample / 2) / kTextOversample;
    texture.advance = static_cast<float>(size.cx) / kTextOversample;
    texture.used = g_frame;
    const int big_width = texture.width * kTextOversample, big_height = texture.height * kTextOversample;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = big_width;
    info.bmiHeader.biHeight = -big_height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = bitmap && bits;
    if (ok) {
        HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        memset(bits, 0, static_cast<size_t>(big_width) * big_height * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, texture.pad * kTextOversample, texture.pad * kTextOversample, wide.c_str(), static_cast<int>(wide.size()));
        GdiFlush();
        std::vector<uint8_t> rgba(static_cast<size_t>(texture.width) * texture.height * 4, 255);
        const auto* source = static_cast<const uint8_t*>(bits);
        for (int y = 0; y < texture.height; y++) {
            for (int x = 0; x < texture.width; x++) {
                int sum = 0;
                for (int sy = 0; sy < kTextOversample; sy++) {
                    const uint8_t* row = source + (static_cast<size_t>(y) * kTextOversample + sy) * big_width * 4 + static_cast<size_t>(x) * kTextOversample * 4;
                    for (int sx = 0; sx < kTextOversample; sx++) sum += row[sx * 4 + 1];
                }
                rgba[(static_cast<size_t>(y) * texture.width + x) * 4 + 3] = static_cast<uint8_t>(sum / (kTextOversample * kTextOversample));
            }
        }
        SelectObject(dc, old_bitmap);
        pglGenTextures(1, &texture.id);
        pglBindTexture(GL_TEXTURE_2D, texture.id);
        pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture.width, texture.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (bitmap) DeleteObject(bitmap);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    if (!ok) return nullptr;
    return &g_textures.emplace(std::move(key), texture).first->second;
}

} // namespace

bool begin(int window_width, int window_height)
{
    if (!g_loaded) load();
    if (g_failed || window_width <= 0 || window_height <= 0) return false;
    g_width = window_width;
    g_height = window_height;
    g_frame++;
    pglGetIntegerv(GL_CURRENT_PROGRAM, &s_program);
    pglGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s_vao);
    pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s_vbo);
    pglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s_fbo);
    pglGetIntegerv(GL_VIEWPORT, s_viewport);
    pglGetIntegerv(GL_BLEND_SRC_RGB, &s_src_rgb);
    pglGetIntegerv(GL_BLEND_DST_RGB, &s_dst_rgb);
    pglGetIntegerv(GL_BLEND_SRC_ALPHA, &s_src_a);
    pglGetIntegerv(GL_BLEND_DST_ALPHA, &s_dst_a);
    pglGetIntegerv(GL_BLEND_EQUATION_RGB, &s_eq_rgb);
    pglGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s_eq_a);
    pglGetIntegerv(GL_DEPTH_WRITEMASK, &s_depth_mask);
    pglGetIntegerv(GL_COLOR_WRITEMASK, s_color_mask);
    pglGetIntegerv(GL_ACTIVE_TEXTURE, &s_active);
    pglActiveTexture(GL_TEXTURE0);
    pglGetIntegerv(GL_TEXTURE_BINDING_2D, &s_tex);
    s_sampler = 0;
    if (pglBindSampler) pglGetIntegerv(GL_SAMPLER_BINDING, &s_sampler);
    pglGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s_unpack);
    pglGetIntegerv(GL_UNPACK_ALIGNMENT, &s_align);
    pglGetIntegerv(GL_UNPACK_ROW_LENGTH, &s_row);
    s_blend = pglIsEnabled(GL_BLEND);
    s_depth = pglIsEnabled(GL_DEPTH_TEST);
    s_cull = pglIsEnabled(GL_CULL_FACE);
    s_scissor = pglIsEnabled(GL_SCISSOR_TEST);
    s_stencil = pglIsEnabled(GL_STENCIL_TEST);
    s_srgb = pglIsEnabled(GL_FRAMEBUFFER_SRGB);

    static bool viewport_logged = false;
    if (!viewport_logged) {
        viewport_logged = true;
        swlog("menu renderer: window %d x %d, game viewport %d x %d, framebuffer %d", window_width, window_height, s_viewport[2], s_viewport[3], s_fbo);
    }
    pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    pglViewport(0, 0, window_width, window_height);
    pglDisable(GL_DEPTH_TEST);
    pglDisable(GL_CULL_FACE);
    pglDisable(GL_SCISSOR_TEST);
    pglDisable(GL_STENCIL_TEST);
    pglDisable(GL_FRAMEBUFFER_SRGB);
    pglEnable(GL_BLEND);
    pglBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    pglBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    pglDepthMask(0);
    pglColorMask(1, 1, 1, 1);
    if (pglBindSampler) pglBindSampler(0, 0);
    pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    pglPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    pglPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    bind_vertices();
    return true;
}

void end()
{
    flush();
    pglUseProgram(s_program);
    pglBindVertexArray(s_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    pglViewport(s_viewport[0], s_viewport[1], s_viewport[2], s_viewport[3]);
    pglBlendFuncSeparate(s_src_rgb, s_dst_rgb, s_src_a, s_dst_a);
    pglBlendEquationSeparate(s_eq_rgb, s_eq_a);
    pglDepthMask(static_cast<GLboolean>(s_depth_mask));
    pglColorMask(static_cast<GLboolean>(s_color_mask[0]), static_cast<GLboolean>(s_color_mask[1]), static_cast<GLboolean>(s_color_mask[2]), static_cast<GLboolean>(s_color_mask[3]));
    pglBindTexture(GL_TEXTURE_2D, s_tex);
    if (pglBindSampler) pglBindSampler(0, s_sampler);
    pglActiveTexture(s_active);
    pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, s_unpack);
    pglPixelStorei(GL_UNPACK_ALIGNMENT, s_align);
    pglPixelStorei(GL_UNPACK_ROW_LENGTH, s_row);
    const auto restore = [](GLenum what, GLboolean on) { if (on) pglEnable(what); else pglDisable(what); };
    restore(GL_BLEND, s_blend);
    restore(GL_DEPTH_TEST, s_depth);
    restore(GL_CULL_FACE, s_cull);
    restore(GL_SCISSOR_TEST, s_scissor);
    restore(GL_STENCIL_TEST, s_stencil);
    restore(GL_FRAMEBUFFER_SRGB, s_srgb);
}

void rect(float x, float y, float w, float h, float radius, Color fill, float border, Color border_color)
{
    if (w <= 0 || h <= 0) return;
    radius = std::min({radius, w * 0.5f, h * 0.5f});
    push_quad(x - 1, y - 1, w + 2, h + 2, 0, 0, w * 0.5f, h * 0.5f, radius, 0, border, fill, border_color);
}
void shadow(float x, float y, float w, float h, float radius, float blur, Color color)
{
    const float half = blur * 0.5f;
    // The rounded box is inset by half the blur, so the falloff is centred on its edge.
    push_quad(x - blur, y - blur, w + blur * 2, h + blur * 2, 0, 0, w * 0.5f, h * 0.5f, radius, half, 0, color, color);
}
void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color color)
{
    // Grown by a pixel so its edges have room to fade; each corner carries its signed distance to all three edges.
    const float cx = (x0 + x1 + x2) / 3, cy = (y0 + y1 + y2) / 3;
    const auto grow = [&](float& x, float& y) {
        const float dx = x - cx, dy = y - cy, length = std::sqrt(dx * dx + dy * dy);
        if (length > 0) {
            x += dx / length * 2;
            y += dy / length * 2;
        }
    };
    const float ax = x0, ay = y0, bx = x1, by = y1, ex = x2, ey = y2;
    const auto edge = [&](float x, float y, float px, float py, float qx, float qy) {
        const float nx = py - qy, ny = qx - px, length = std::sqrt(nx * nx + ny * ny);
        const float distance = length > 0 ? ((x - px) * nx + (y - py) * ny) / length : 0;
        const float side = length > 0 ? ((cx - px) * nx + (cy - py) * ny) : 1;
        return side < 0 ? -distance : distance;
    };
    grow(x0, y0);
    grow(x1, y1);
    grow(x2, y2);
    const auto vertex = [&](float vx, float vy) {
        Vertex v{vx, vy, edge(vx, vy, ax, ay, bx, by), edge(vx, vy, bx, by, ex, ey), -1, edge(vx, vy, ex, ey, ax, ay), 0, 0, 0, {color.r, color.g, color.b, color.a}, {}};
        g_batch.push_back(v);
    };
    vertex(x0, y0);
    vertex(x1, y1);
    vertex(x2, y2);
}
void area_graph(float x, float y, float w, float h, float radius, const float* values, int count, float lo, float hi, Color fill, Color line)
{
    if (w <= 0 || h <= 0 || count <= 0 || hi <= lo) return;
    flush();
    float points[kGraphMaxValues];
    const int used = std::min(count, kGraphMaxValues);
    std::memcpy(points, values + (count - used), static_cast<size_t>(used) * sizeof(float));
    // One value still draws across the whole width.
    int total = used;
    if (total == 1) {
        points[1] = points[0];
        total = 2;
    }
    radius = std::min({radius, w * 0.5f, h * 0.5f});
    push_quad(x, y, w, h, 0, 0, w * 0.5f, h * 0.5f, radius, 1.25f, 0, fill, line);
    pglUseProgram(g_graph_program);
    pglUniform2f(g_graph_size, static_cast<float>(g_width), static_cast<float>(g_height));
    pglUniform1fv(g_graph_values, total, points);
    pglUniform1f(g_graph_count, static_cast<float>(total));
    pglUniform1f(g_graph_lo, lo);
    pglUniform1f(g_graph_hi, hi);
    pglBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(g_batch.size() * sizeof(Vertex)), g_batch.data(), GL_STREAM_DRAW);
    pglDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(g_batch.size()));
    g_batch.clear();
}
float text_width(const char* s, float size_px, Weight weight)
{
    return measure(s, rounded_px(size_px), weight);
}
void text(const char* s, float x, float baseline, float size_px, Weight weight, Color color, Align align)
{
    const int px = rounded_px(size_px);
    Texture* texture = text_texture(s, px, weight);
    if (!texture) return;
    flush();
    float left = x;
    if (align == Align::Center) left -= texture->advance * 0.5f;
    else if (align == Align::Right) left -= texture->advance;
    // Whole pixels, so the glyphs stay as sharp as they were rasterized.
    left = std::floor(left + 0.5f) - static_cast<float>(texture->pad);
    const float top = std::floor(baseline + 0.5f) - static_cast<float>(texture->ascent + texture->pad);
    const float w = static_cast<float>(texture->width), h = static_cast<float>(texture->height);
    Vertex v{};
    v.fill[0] = color.r;
    v.fill[1] = color.g;
    v.fill[2] = color.b;
    v.fill[3] = color.a;
    const auto corner = [&](float cx, float cy, float u, float t) {
        v.x = cx;
        v.y = cy;
        v.px = u;
        v.py = t;
        return v;
    };
    const Vertex quad[6] = {corner(left, top, 0, 0), corner(left + w, top, 1, 0), corner(left, top + h, 0, 1),
                            corner(left + w, top, 1, 0), corner(left + w, top + h, 1, 1), corner(left, top + h, 0, 1)};
    pglUseProgram(g_text_program);
    pglUniform2f(g_text_size, static_cast<float>(g_width), static_cast<float>(g_height));
    pglUniform1i(g_text_tex, 0);
    pglBindTexture(GL_TEXTURE_2D, texture->id);
    pglBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
    pglDrawArrays(GL_TRIANGLES, 0, 6);
}

} // namespace glui
