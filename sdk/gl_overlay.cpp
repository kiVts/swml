#include "gl_overlay.h"
#include "log.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace overlay {
namespace {

// --- GL types/constants (no GL headers beyond what opengl32 exports) ----------------------
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLchar = char;
using GLsizeiptr = intptr_t;

constexpr GLenum GL_TRIANGLES = 0x0004, GL_FLOAT = 0x1406, GL_BLEND = 0x0BE2, GL_DEPTH_TEST = 0x0B71,
                 GL_CULL_FACE = 0x0B44, GL_SCISSOR_TEST = 0x0C11, GL_STENCIL_TEST = 0x0B90, GL_SRC_ALPHA = 0x0302,
                 GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_VIEWPORT = 0x0BA2, GL_DEPTH_WRITEMASK = 0x0B72,
                 GL_COLOR_WRITEMASK = 0x0C23, GL_CURRENT_PROGRAM = 0x8B8D, GL_VERTEX_ARRAY_BINDING = 0x85B5,
                 GL_ARRAY_BUFFER_BINDING = 0x8894, GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6, GL_ARRAY_BUFFER = 0x8892,
                 GL_STREAM_DRAW = 0x88E0, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_VERTEX_SHADER = 0x8B31,
                 GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
                 GL_BLEND_SRC_RGB = 0x80C9, GL_BLEND_DST_RGB = 0x80C8, GL_BLEND_SRC_ALPHA = 0x80CB,
                 GL_BLEND_DST_ALPHA = 0x80CA, GL_BLEND_EQUATION_RGB = 0x8009, GL_BLEND_EQUATION_ALPHA = 0x883D,
                 GL_FUNC_ADD = 0x8006, GL_FRAMEBUFFER_SRGB = 0x8DB9;

#define GLFN(ret, name, ...) using name##_t = ret(WINAPI*)(__VA_ARGS__); name##_t p##name = nullptr;
GLFN(void, glGetIntegerv, GLenum, GLint*)
GLFN(GLboolean, glIsEnabled, GLenum)
GLFN(void, glEnable, GLenum)
GLFN(void, glDisable, GLenum)
GLFN(void, glViewport, GLint, GLint, GLsizei, GLsizei)
GLFN(void, glDrawArrays, GLenum, GLint, GLsizei)
GLFN(void, glDepthMask, GLboolean)
GLFN(void, glColorMask, GLboolean, GLboolean, GLboolean, GLboolean)
GLFN(GLenum, glGetError)
GLFN(void, glBlendFuncSeparate, GLenum, GLenum, GLenum, GLenum)
GLFN(void, glBlendEquationSeparate, GLenum, GLenum)
GLFN(GLuint, glCreateShader, GLenum)
GLFN(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)
GLFN(void, glCompileShader, GLuint)
GLFN(void, glGetShaderiv, GLuint, GLenum, GLint*)
GLFN(GLuint, glCreateProgram)
GLFN(void, glAttachShader, GLuint, GLuint)
GLFN(void, glBindAttribLocation, GLuint, GLuint, const GLchar*)
GLFN(void, glLinkProgram, GLuint)
GLFN(void, glGetProgramiv, GLuint, GLenum, GLint*)
GLFN(void, glUseProgram, GLuint)
GLFN(void, glGenBuffers, GLsizei, GLuint*)
GLFN(void, glBindBuffer, GLenum, GLuint)
GLFN(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum)
GLFN(void, glGenVertexArrays, GLsizei, GLuint*)
GLFN(void, glBindVertexArray, GLuint)
GLFN(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)
GLFN(void, glEnableVertexAttribArray, GLuint)
GLFN(void, glBindFramebuffer, GLenum, GLuint)
#undef GLFN

struct Vertex {
    float x, y, r, g, b, a;
};

bool g_loaded = false, g_failed = false;
GLuint g_program = 0, g_vao = 0, g_vbo = 0;
std::vector<Vertex> g_verts;
int g_w = 0, g_h = 0;

// Saved state.
GLint s_program, s_vao, s_vbo, s_fbo, s_viewport[4], s_src_rgb, s_dst_rgb, s_src_a, s_dst_a, s_eq_rgb, s_eq_a;
GLint s_depth_mask, s_color_mask[4];
GLboolean s_blend, s_depth, s_cull, s_scissor, s_stencil, s_srgb;

void* gl_proc(const char* name)
{
    using wglGetProcAddress_t = PROC(WINAPI*)(LPCSTR);
    static HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    static auto wgl_get = reinterpret_cast<wglGetProcAddress_t>(GetProcAddress(gl, "wglGetProcAddress"));
    void* p = wgl_get ? reinterpret_cast<void*>(wgl_get(name)) : nullptr;
    // wglGetProcAddress returns small sentinel values for unsupported/1.1 functions.
    if (reinterpret_cast<uintptr_t>(p) <= 3 || reinterpret_cast<intptr_t>(p) == -1) {
        p = reinterpret_cast<void*>(GetProcAddress(gl, name));
    }
    return p;
}

GLuint compile(GLenum type, const char* src)
{
    GLuint s = pglCreateShader(type);
    pglShaderSource(s, 1, &src, nullptr);
    pglCompileShader(s);
    GLint ok = 0;
    pglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    return ok ? s : 0;
}

bool load()
{
    g_loaded = true;
#define LOAD(name)                                                       \
    p##name = reinterpret_cast<name##_t>(gl_proc(#name));                \
    if (!p##name) {                                                      \
        swlog("overlay: missing GL function %s - overlay disabled", #name); \
        g_failed = true;                                                 \
        return false;                                                    \
    }
    LOAD(glGetIntegerv) LOAD(glIsEnabled) LOAD(glEnable) LOAD(glDisable) LOAD(glViewport) LOAD(glDrawArrays)
    LOAD(glDepthMask) LOAD(glColorMask) LOAD(glGetError) LOAD(glBlendFuncSeparate) LOAD(glBlendEquationSeparate)
    LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader) LOAD(glGetShaderiv) LOAD(glCreateProgram)
    LOAD(glAttachShader) LOAD(glBindAttribLocation) LOAD(glLinkProgram) LOAD(glGetProgramiv) LOAD(glUseProgram)
    LOAD(glGenBuffers) LOAD(glBindBuffer) LOAD(glBufferData) LOAD(glGenVertexArrays) LOAD(glBindVertexArray)
    LOAD(glVertexAttribPointer) LOAD(glEnableVertexAttribArray) LOAD(glBindFramebuffer)
#undef LOAD

    // Try GLSL versions from core-profile friendly to older contexts.
    const char* versions[] = {"#version 330 core\n", "#version 150\n", "#version 130\n"};
    for (const char* ver : versions) {
        std::string vs = std::string(ver) +
                         "in vec2 p; in vec4 c; out vec4 vc;\n"
                         "void main() { gl_Position = vec4(p, 0.0, 1.0); vc = c; }\n";
        std::string fs = std::string(ver) +
                         "in vec4 vc; out vec4 o;\n"
                         "void main() { o = vc; }\n";
        GLuint v = compile(GL_VERTEX_SHADER, vs.c_str());
        GLuint f = compile(GL_FRAGMENT_SHADER, fs.c_str());
        if (!v || !f) continue;
        GLuint prog = pglCreateProgram();
        pglAttachShader(prog, v);
        pglAttachShader(prog, f);
        pglBindAttribLocation(prog, 0, "p");
        pglBindAttribLocation(prog, 1, "c");
        pglLinkProgram(prog);
        GLint ok = 0;
        pglGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (ok) {
            g_program = prog;
            swlog("overlay: GL shaders ready (%.*s)", static_cast<int>(strlen(ver) - 1), ver);
            break;
        }
    }
    if (!g_program) {
        swlog("overlay: could not build shaders - overlay disabled");
        g_failed = true;
        return false;
    }
    GLint old_vao = 0, old_vbo = 0;
    pglGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_vbo);
    pglGenVertexArrays(1, &g_vao);
    pglGenBuffers(1, &g_vbo);
    pglBindVertexArray(g_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    pglEnableVertexAttribArray(0);
    pglEnableVertexAttribArray(1);
    pglVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(Vertex), reinterpret_cast<void*>(0));
    pglVertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(Vertex), reinterpret_cast<void*>(2 * sizeof(float)));
    pglBindVertexArray(static_cast<GLuint>(old_vao));
    pglBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(old_vbo));
    return true;
}

void push(float px, float py, Color c)
{
    g_verts.push_back({px / g_w * 2.0f - 1.0f, py / g_h * 2.0f - 1.0f, c.r, c.g, c.b, c.a});
}

} // namespace

bool begin(HDC dc, int* width, int* height)
{
    if (g_failed) return false;
    if (!g_loaded && !load()) return false;
    RECT rc{};
    HWND wnd = WindowFromDC(dc);
    if (!wnd || !GetClientRect(wnd, &rc)) return false;
    g_w = rc.right - rc.left;
    g_h = rc.bottom - rc.top;
    if (g_w <= 0 || g_h <= 0) return false;
    *width = g_w;
    *height = g_h;
    g_verts.clear();
    return true;
}

void line(float x0, float y0, float x1, float y1, float width_px, Color c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    // Extend by half the width so joined segments overlap without gaps.
    float ux = dx / len, uy = dy / len, h = width_px * 0.5f;
    float nx = -uy * h, ny = ux * h;
    x0 -= ux * h; y0 -= uy * h; x1 += ux * h; y1 += uy * h;
    push(x0 + nx, y0 + ny, c); push(x1 + nx, y1 + ny, c); push(x1 - nx, y1 - ny, c);
    push(x0 + nx, y0 + ny, c); push(x1 - nx, y1 - ny, c); push(x0 - nx, y0 - ny, c);
}

void end()
{
    if (g_verts.empty()) return;
    // Save.
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
    s_blend = pglIsEnabled(GL_BLEND);
    s_depth = pglIsEnabled(GL_DEPTH_TEST);
    s_cull = pglIsEnabled(GL_CULL_FACE);
    s_scissor = pglIsEnabled(GL_SCISSOR_TEST);
    s_stencil = pglIsEnabled(GL_STENCIL_TEST);
    s_srgb = pglIsEnabled(GL_FRAMEBUFFER_SRGB);

    // Draw.
    pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    pglViewport(0, 0, g_w, g_h);
    pglEnable(GL_BLEND);
    pglBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    pglBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    pglDisable(GL_DEPTH_TEST);
    pglDisable(GL_CULL_FACE);
    pglDisable(GL_SCISSOR_TEST);
    pglDisable(GL_STENCIL_TEST);
    pglDisable(GL_FRAMEBUFFER_SRGB);
    pglDepthMask(0);
    pglColorMask(1, 1, 1, 1);
    pglUseProgram(g_program);
    pglBindVertexArray(g_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    pglBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(g_verts.size() * sizeof(Vertex)), g_verts.data(),
                  GL_STREAM_DRAW);
    pglDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(g_verts.size()));

    // Restore.
    pglBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(s_vbo));
    pglBindVertexArray(static_cast<GLuint>(s_vao));
    pglUseProgram(static_cast<GLuint>(s_program));
    pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(s_fbo));
    pglViewport(s_viewport[0], s_viewport[1], s_viewport[2], s_viewport[3]);
    pglBlendEquationSeparate(static_cast<GLenum>(s_eq_rgb), static_cast<GLenum>(s_eq_a));
    pglBlendFuncSeparate(static_cast<GLenum>(s_src_rgb), static_cast<GLenum>(s_dst_rgb), static_cast<GLenum>(s_src_a),
                         static_cast<GLenum>(s_dst_a));
    pglDepthMask(static_cast<GLboolean>(s_depth_mask));
    pglColorMask(static_cast<GLboolean>(s_color_mask[0]), static_cast<GLboolean>(s_color_mask[1]),
                 static_cast<GLboolean>(s_color_mask[2]), static_cast<GLboolean>(s_color_mask[3]));
    auto set = [](GLenum cap, GLboolean on) { on ? pglEnable(cap) : pglDisable(cap); };
    set(GL_BLEND, s_blend);
    set(GL_DEPTH_TEST, s_depth);
    set(GL_CULL_FACE, s_cull);
    set(GL_SCISSOR_TEST, s_scissor);
    set(GL_STENCIL_TEST, s_stencil);
    set(GL_FRAMEBUFFER_SRGB, s_srgb);
    g_verts.clear();
}

void glyph(char ch, float x, float y, float size, float width, Color c)
{
    // Stroke font on a 9x9 grid: "xy" digit pairs (0..8, y up), points joined into polylines,
    // '|' starts a new stroke. Lower case is drawn as upper case.
    struct Glyph {
        char ch;
        const char* strokes;
    };
    static const Glyph kFont[] = {
        {'A', "00 48 80|23 63"},
        {'B', "00 08 58 77 75 54 04|54 73 71 50 00"},
        {'C', "87 68 28 07 01 20 60 81"},
        {'D', "00 08 58 86 82 50 00"},
        {'E', "80 00 08 88|04 64"},
        {'F', "00 08 88|04 64"},
        {'G', "87 68 28 07 01 20 60 81 84 54"},
        {'H', "00 08|80 88|04 84"},
        {'I', "20 60|28 68|40 48"},
        {'J', "28 78|58 51 30 10 02"},
        {'K', "00 08|88 03|25 80"},
        {'L', "08 00 80"},
        {'M', "00 08 44 88 80"},
        {'N', "00 08 80 88"},
        {'O', "28 68 87 81 60 20 01 07 28"},
        {'P', "00 08 68 87 85 64 04"},
        {'Q', "28 68 87 81 60 20 01 07 28|53 80"},
        {'R', "00 08 68 87 85 64 04|44 80"},
        {'S', "87 68 28 07 05 24 64 83 81 60 20 01"},
        {'T', "08 88|48 40"},
        {'U', "08 01 20 60 81 88"},
        {'V', "08 40 88"},
        {'W', "08 20 44 60 88"},
        {'X', "08 80|00 88"},
        {'Y', "08 44 88|44 40"},
        {'Z', "08 88 00 80"},
        {'0', "28 68 87 81 60 20 01 07 28|21 67"},
        {'1', "26 48 40|20 60"},
        {'2', "07 28 68 87 85 00 80"},
        {'3', "07 28 68 87 85 64 83 81 60 20 01|34 64"},
        {'4', "60 68 03 83"},
        {'5', "88 08 05 65 84 81 60 20 01"},
        {'6', "87 68 28 07 01 20 60 81 83 64 24 03"},
        {'7', "08 88 30"},
        {'8', "24 05 07 28 68 87 85 64 24 03 01 20 60 81 83 64"},
        {'9', "05 24 64 85 87 68 28 07 05|87 81 60 20 01"},
        {'/', "20 68"},
        {'-', "24 64"},
        {'=', "23 63|25 65"},
        {'?', "07 28 68 87 85 64 43|41 40"},
        {'.', "40 41"},
    };
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    const char* def = nullptr;
    for (const Glyph& g : kFont) {
        if (g.ch == ch) {
            def = g.strokes;
            break;
        }
    }
    if (!def) return;
    auto px = [&](char d) { return x + ((d - '0') / 8.0f - 0.5f) * size * 0.7f; };
    auto py = [&](char d) { return y + ((d - '0') / 8.0f - 0.5f) * size; };
    bool have_prev = false;
    float lx = 0, ly = 0;
    for (const char* p = def; *p;) {
        if (*p == ' ') { p++; continue; }
        if (*p == '|') { have_prev = false; p++; continue; }
        float nx = px(p[0]), ny = py(p[1]);
        if (have_prev) line(lx, ly, nx, ny, width, c);
        lx = nx;
        ly = ny;
        have_prev = true;
        p += 2;
    }
}

void text(const char* s, float x, float y, float size, float width, Color c)
{
    int n = static_cast<int>(strlen(s));
    float advance = size * 0.85f;
    float start = x - advance * (n - 1) * 0.5f;
    for (int i = 0; i < n; i++) glyph(s[i], start + advance * i, y, size, width, c);
}

} // namespace overlay
