#include "gl_text.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gltext {
namespace {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLchar = char;
using GLsizeiptr = intptr_t;

constexpr GLenum GL_TRIANGLES = 0x0004, GL_FLOAT = 0x1406, GL_UNSIGNED_BYTE = 0x1401, GL_BLEND = 0x0BE2,
                 GL_DEPTH_TEST = 0x0B71, GL_CULL_FACE = 0x0B44, GL_SCISSOR_TEST = 0x0C11, GL_STENCIL_TEST = 0x0B90,
                 GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_VIEWPORT = 0x0BA2,
                 GL_DEPTH_WRITEMASK = 0x0B72, GL_COLOR_WRITEMASK = 0x0C23, GL_CURRENT_PROGRAM = 0x8B8D,
                 GL_VERTEX_ARRAY_BINDING = 0x85B5, GL_ARRAY_BUFFER_BINDING = 0x8894, GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6,
                 GL_ARRAY_BUFFER = 0x8892, GL_STREAM_DRAW = 0x88E0, GL_DRAW_FRAMEBUFFER = 0x8CA9,
                 GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81,
                 GL_LINK_STATUS = 0x8B82, GL_BLEND_SRC_RGB = 0x80C9, GL_BLEND_DST_RGB = 0x80C8,
                 GL_BLEND_SRC_ALPHA = 0x80CB, GL_BLEND_DST_ALPHA = 0x80CA, GL_BLEND_EQUATION_RGB = 0x8009,
                 GL_BLEND_EQUATION_ALPHA = 0x883D, GL_FUNC_ADD = 0x8006, GL_FRAMEBUFFER_SRGB = 0x8DB9,
                 GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_ACTIVE_TEXTURE = 0x84E0,
                 GL_TEXTURE_BINDING_2D = 0x8069, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                 GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_LINEAR = 0x2601,
                 GL_CLAMP_TO_EDGE = 0x812F, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_UNPACK_ALIGNMENT = 0x0CF5,
                 GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_PIXEL_UNPACK_BUFFER = 0x88EC,
                 GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF, GL_SAMPLER_BINDING = 0x8919;

#define GLFN(ret, name, ...) using name##_t = ret(WINAPI*)(__VA_ARGS__); name##_t p##name = nullptr;
GLFN(void, glGetIntegerv, GLenum, GLint*)
GLFN(GLboolean, glIsEnabled, GLenum)
GLFN(void, glEnable, GLenum)
GLFN(void, glDisable, GLenum)
GLFN(void, glViewport, GLint, GLint, GLsizei, GLsizei)
GLFN(void, glDrawArrays, GLenum, GLint, GLsizei)
GLFN(void, glDepthMask, GLboolean)
GLFN(void, glColorMask, GLboolean, GLboolean, GLboolean, GLboolean)
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
GLFN(GLint, glGetUniformLocation, GLuint, const GLchar*)
GLFN(void, glUniform1i, GLint, GLint)
GLFN(void, glUniform4f, GLint, float, float, float, float)
GLFN(void, glGenBuffers, GLsizei, GLuint*)
GLFN(void, glBindBuffer, GLenum, GLuint)
GLFN(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum)
GLFN(void, glGenVertexArrays, GLsizei, GLuint*)
GLFN(void, glBindVertexArray, GLuint)
GLFN(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)
GLFN(void, glEnableVertexAttribArray, GLuint)
GLFN(void, glBindFramebuffer, GLenum, GLuint)
GLFN(void, glGenTextures, GLsizei, GLuint*)
GLFN(void, glDeleteTextures, GLsizei, const GLuint*)
GLFN(void, glBindTexture, GLenum, GLuint)
GLFN(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)
GLFN(void, glTexParameteri, GLenum, GLenum, GLint)
GLFN(void, glActiveTexture, GLenum)
GLFN(void, glPixelStorei, GLenum, GLint)
GLFN(void, glBindSampler, GLuint, GLuint) // GL 3.3; optional
#undef GLFN

bool g_loaded = false, g_failed = false;
GLuint g_program = 0, g_vao = 0, g_vbo = 0, g_tex = 0;
GLint g_u_tex = -1, g_u_color = -1;
// The texture's contents: what was rendered, at what size.
std::string g_tex_text;
int g_tex_height = 0, g_tex_w = 0, g_tex_h = 0;

void* gl_proc(const char* name)
{
    using wglGetProcAddress_t = PROC(WINAPI*)(LPCSTR);
    static HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    static auto wgl_get = reinterpret_cast<wglGetProcAddress_t>(GetProcAddress(gl, "wglGetProcAddress"));
    void* p = wgl_get ? reinterpret_cast<void*>(wgl_get(name)) : nullptr;
    if (reinterpret_cast<uintptr_t>(p) <= 3 || reinterpret_cast<intptr_t>(p) == -1) p = reinterpret_cast<void*>(GetProcAddress(gl, name));
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
#define LOAD(name)                                                    \
    p##name = reinterpret_cast<name##_t>(gl_proc(#name));             \
    if (!p##name) {                                                   \
        swlog("text overlay: missing GL function %s - off", #name); \
        g_failed = true;                                              \
        return false;                                                 \
    }
    LOAD(glGetIntegerv) LOAD(glIsEnabled) LOAD(glEnable) LOAD(glDisable) LOAD(glViewport) LOAD(glDrawArrays)
    LOAD(glDepthMask) LOAD(glColorMask) LOAD(glBlendFuncSeparate) LOAD(glBlendEquationSeparate) LOAD(glCreateShader)
    LOAD(glShaderSource) LOAD(glCompileShader) LOAD(glGetShaderiv) LOAD(glCreateProgram) LOAD(glAttachShader)
    LOAD(glBindAttribLocation) LOAD(glLinkProgram) LOAD(glGetProgramiv) LOAD(glUseProgram) LOAD(glGetUniformLocation)
    LOAD(glUniform1i) LOAD(glUniform4f) LOAD(glGenBuffers) LOAD(glBindBuffer) LOAD(glBufferData) LOAD(glGenVertexArrays)
    LOAD(glBindVertexArray) LOAD(glVertexAttribPointer) LOAD(glEnableVertexAttribArray) LOAD(glBindFramebuffer)
    LOAD(glGenTextures) LOAD(glDeleteTextures) LOAD(glBindTexture) LOAD(glTexImage2D) LOAD(glTexParameteri)
    LOAD(glActiveTexture) LOAD(glPixelStorei)
#undef LOAD
    pglBindSampler = reinterpret_cast<glBindSampler_t>(gl_proc("glBindSampler"));

    const char* versions[] = {"#version 330 core\n", "#version 150\n", "#version 130\n"};
    for (const char* ver : versions) {
        std::string vs = std::string(ver) +
                         "in vec2 p; in vec2 t; out vec2 vt;\n"
                         "void main() { gl_Position = vec4(p, 0.0, 1.0); vt = t; }\n";
        std::string fs = std::string(ver) +
                         "in vec2 vt; out vec4 o; uniform sampler2D tex; uniform vec4 col;\n"
                         "void main() { o = vec4(col.rgb, col.a * texture(tex, vt).a); }\n";
        GLuint v = compile(GL_VERTEX_SHADER, vs.c_str()), f = compile(GL_FRAGMENT_SHADER, fs.c_str());
        if (!v || !f) continue;
        GLuint prog = pglCreateProgram();
        pglAttachShader(prog, v);
        pglAttachShader(prog, f);
        pglBindAttribLocation(prog, 0, "p");
        pglBindAttribLocation(prog, 1, "t");
        pglLinkProgram(prog);
        GLint ok = 0;
        pglGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (ok) {
            g_program = prog;
            break;
        }
    }
    if (!g_program) {
        swlog("text overlay: could not build shaders - off");
        g_failed = true;
        return false;
    }
    g_u_tex = pglGetUniformLocation(g_program, "tex");
    g_u_color = pglGetUniformLocation(g_program, "col");
    GLint old_vao = 0, old_vbo = 0;
    pglGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_vbo);
    pglGenVertexArrays(1, &g_vao);
    pglGenBuffers(1, &g_vbo);
    pglBindVertexArray(g_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    pglEnableVertexAttribArray(0);
    pglEnableVertexAttribArray(1);
    pglVertexAttribPointer(0, 2, GL_FLOAT, 0, 4 * sizeof(float), reinterpret_cast<void*>(0));
    pglVertexAttribPointer(1, 2, GL_FLOAT, 0, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    pglBindVertexArray(static_cast<GLuint>(old_vao));
    pglBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(old_vbo));
    swlog("text overlay: ready");
    return true;
}

// The string drawn by GDI in a bold TrueType face, white on black; alpha = coverage. Rows bottom-up (GL's order).
bool rasterize(const char* text, int height_px, std::vector<uint8_t>& rgba, int& w, int& h)
{
    wchar_t wide[256];
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 256);
    if (n <= 1) return false;
    n--; // without the terminator
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = nullptr;
    for (const wchar_t* face : {L"Segoe UI", L"Arial", L"Liberation Sans", L"DejaVu Sans"}) {
        font = CreateFontW(-height_px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
        if (!font) continue;
        HGDIOBJ old = SelectObject(dc, font);
        wchar_t got[64] = {};
        GetTextFaceW(dc, 64, got);
        SelectObject(dc, old);
        if (_wcsicmp(got, face) == 0) break; // that face exists
        DeleteObject(font);
        font = nullptr;
    }
    if (!font) font = CreateFontW(-height_px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, nullptr); // the system's sans
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, wide, n, &sz);
    int pad = std::max(2, height_px / 8);
    w = sz.cx + 2 * pad;
    h = sz.cy + 2 * pad;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h; // bottom-up, like GL
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = bmp && bits;
    if (ok) {
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        memset(bits, 0, size_t(w) * h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, pad, pad, wide, n);
        GdiFlush();
        rgba.assign(size_t(w) * h * 4, 255);
        const uint8_t* src = static_cast<const uint8_t*>(bits);
        for (size_t i = 0; i < size_t(w) * h; i++) {
            uint8_t a = std::max({src[i * 4], src[i * 4 + 1], src[i * 4 + 2]});
            rgba[i * 4 + 3] = a;
        }
        SelectObject(dc, old_bmp);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

} // namespace

bool draw_top(HDC dc, const char* text, int height_px, int top_px, Color c)
{
    if (g_failed) return false;
    if (!g_loaded && !load()) return false;
    RECT rc{};
    HWND wnd = WindowFromDC(dc);
    if (!wnd || !GetClientRect(wnd, &rc)) return false;
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    if (ww <= 0 || wh <= 0) return false;

    // Save.
    GLint s_program, s_vao, s_vbo, s_fbo, s_viewport[4], s_src_rgb, s_dst_rgb, s_src_a, s_dst_a, s_eq_rgb, s_eq_a;
    GLint s_depth_mask, s_color_mask[4], s_active, s_tex, s_unpack_buf, s_align, s_row, s_sampler = 0;
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
    if (pglBindSampler) pglGetIntegerv(GL_SAMPLER_BINDING, &s_sampler);
    pglGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s_unpack_buf);
    pglGetIntegerv(GL_UNPACK_ALIGNMENT, &s_align);
    pglGetIntegerv(GL_UNPACK_ROW_LENGTH, &s_row);
    GLboolean s_blend = pglIsEnabled(GL_BLEND), s_depth = pglIsEnabled(GL_DEPTH_TEST), s_cull = pglIsEnabled(GL_CULL_FACE),
              s_scissor = pglIsEnabled(GL_SCISSOR_TEST), s_stencil = pglIsEnabled(GL_STENCIL_TEST),
              s_srgb = pglIsEnabled(GL_FRAMEBUFFER_SRGB);

    // (Re)build the texture when the text or size changes.
    if (!g_tex) pglGenTextures(1, &g_tex);
    pglBindTexture(GL_TEXTURE_2D, g_tex);
    if (pglBindSampler) pglBindSampler(0, 0);
    bool have = !g_tex_text.empty();
    if (g_tex_text != text || g_tex_height != height_px) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        have = rasterize(text, height_px, rgba, w, h);
        if (have) {
            pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            pglPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            pglPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g_tex_text = text;
            g_tex_height = height_px;
            g_tex_w = w;
            g_tex_h = h;
        } else {
            g_tex_text.clear();
        }
    }

    if (have) {
        pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        pglViewport(0, 0, ww, wh);
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
        pglUniform1i(g_u_tex, 0);
        pglBindVertexArray(g_vao);
        pglBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        auto quad = [&](float x0, float y_top, Color col) {
            float x1 = x0 + g_tex_w, y1 = float(wh) - y_top, y0 = y1 - g_tex_h;
            auto nx = [&](float x) { return x / ww * 2.0f - 1.0f; };
            auto ny = [&](float y) { return y / wh * 2.0f - 1.0f; };
            float v[] = {nx(x0), ny(y0), 0, 0, nx(x1), ny(y0), 1, 0, nx(x1), ny(y1), 1, 1,
                         nx(x0), ny(y0), 0, 0, nx(x1), ny(y1), 1, 1, nx(x0), ny(y1), 0, 1};
            pglBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
            pglUniform4f(g_u_color, col.r, col.g, col.b, col.a);
            pglDrawArrays(GL_TRIANGLES, 0, 6);
        };
        float x = std::floor((ww - g_tex_w) * 0.5f);
        float shadow = std::max(1.0f, height_px / 16.0f);
        for (float dx : {-shadow, shadow})
            for (float dy : {-shadow, shadow}) quad(x + dx, float(top_px) + dy, {0, 0, 0, 0.75f * c.a});
        quad(x, float(top_px), c);
    }

    // Restore.
    pglBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s_tex));
    if (pglBindSampler) pglBindSampler(0, static_cast<GLuint>(s_sampler));
    pglActiveTexture(static_cast<GLenum>(s_active));
    pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(s_unpack_buf));
    pglPixelStorei(GL_UNPACK_ALIGNMENT, s_align);
    pglPixelStorei(GL_UNPACK_ROW_LENGTH, s_row);
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
    return have;
}

} // namespace gltext
