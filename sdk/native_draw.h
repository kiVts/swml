#pragma once
#include "hooks.h"
#include "log.h"
#include "resolve.h"
#include <intrin.h>
#include <cstdint>

namespace swml4::draw {

struct Pass {
    void* renderer = nullptr;
    const void* camera = nullptr;
};
struct Color {
    uint8_t r, g, b;
};
inline constexpr Color dim(Color c, float k)
{
    return {uint8_t(c.r * k), uint8_t(c.g * k), uint8_t(c.b * k)};
}

inline constexpr const char* kLinePattern =
    "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00 "
    "F2 41 0F 10 09 48 8B F9 F2 41 0F 10 20 4C 8B F2 F3 0F 10 05 ?? ?? ?? ??";
inline constexpr const char* kFlightSitePattern = "E8 ?? ?? ?? ?? 4C 8B C8 48 8D 9E A0 01 00 00 8B 83 F8 00 00 00 8D 48 FF";
inline constexpr const char* kAspectPattern = "F3 0F 10 96 00 09 00 00 48 8D 4D ?? E8";

// clip = vp * (p + shift): pick shift near the points and pass points minus shift, so floats keep their precision.
struct CameraBlock {
    uint8_t unused[0x108];
    float vp[16];
};
inline void fill_camera(CameraBlock& block, const double vp[16], const double shift[3])
{
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 4; r++) block.vp[c * 4 + r] = static_cast<float>(vp[c * 4 + r]);
    for (int r = 0; r < 4; r++)
        block.vp[12 + r] = static_cast<float>(vp[r] * shift[0] + vp[4 + r] * shift[1] + vp[8 + r] * shift[2] + vp[12 + r]);
}

using NativeLine = void(__fastcall*)(void* renderer, const void* camera, const double* a, const double* b, const uint32_t* abgr);
inline NativeLine g_native_line = nullptr;

inline bool bind_line()
{
    if (!g_native_line) g_native_line = reinterpret_cast<NativeLine>(native_find_pattern(kLinePattern));
    return g_native_line != nullptr;
}

// Render thread, inside a pass callback. bind_line() must have succeeded.
inline void line(const Pass& pass, const double a[3], const double b[3], Color c)
{
    const uint32_t abgr = 0xff000000u | (uint32_t(c.b) << 16) | (uint32_t(c.g) << 8) | c.r;
    g_native_line(pass.renderer, pass.camera, a, b, &abgr);
}

inline float ui_aspect(const Pass& pass)
{
    return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(pass.renderer) + 0x900);
}
inline void ndc_to_ui(float aspect, float nx, float ny, float* x, float* y)
{
    if (aspect <= 1) *x = nx, *y = ny / aspect;
    else *x = nx * aspect, *y = ny;
}

using PassCallback = void (*)(const Pass&);
inline PassCallback g_flight_callback = nullptr;
using NativeSim = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using NativeScene = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using NativeTimer = uint64_t(__fastcall*)();
inline NativeSim g_native_sim = nullptr;
inline NativeScene g_native_scene = nullptr;
inline NativeTimer g_native_timer = nullptr;
inline uintptr_t g_flight_return = 0;
inline thread_local bool tl_flight_sim = false;
inline thread_local uintptr_t tl_flight_renderer = 0;

inline void __fastcall flight_sim_detour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
{
    tl_flight_sim = true;
    tl_flight_renderer = 0;
    g_native_sim(a, b, c, d, e, f);
    tl_flight_sim = false;
    tl_flight_renderer = 0;
}
inline void __fastcall flight_scene_detour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
    if (tl_flight_sim && !tl_flight_renderer) tl_flight_renderer = b; // first scene = the main view
    g_native_scene(a, b, c, d, e, f, g, h);
}
inline uint64_t __fastcall flight_timer_detour()
{
    const uint64_t result = g_native_timer();
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_flight_return && tl_flight_renderer && g_flight_callback)
        g_flight_callback(Pass{reinterpret_cast<void*>(tl_flight_renderer), nullptr});
    return result;
}
// One callback per mod, called on the render thread for each flight frame, with the renderer; the mod supplies
// its own CameraBlock as the pass's camera. False = unavailable.
inline bool on_flight(PassCallback callback)
{
    if (!bind_line()) {
        swlog("native draw: line primitive not recognised");
        return false;
    }
    const uintptr_t site = native_find_pattern(kFlightSitePattern);
    if (!site || !native_find_pattern(kAspectPattern)) {
        swlog("native draw: flight pass not recognised");
        return false;
    }
    g_flight_return = site + 5;
    const uintptr_t timer = g_flight_return + *reinterpret_cast<const int32_t*>(site + 1);
    g_flight_callback = callback;
    if (!hook_game(0x1407ad620, reinterpret_cast<void*>(&flight_sim_detour), reinterpret_cast<void**>(&g_native_sim)) ||
        !hook_game(0x1406d2030, reinterpret_cast<void*>(&flight_scene_detour), reinterpret_cast<void**>(&g_native_scene)) ||
        !hook_address(timer, reinterpret_cast<void*>(&flight_timer_detour), reinterpret_cast<void**>(&g_native_timer))) {
        swlog("native draw: flight pass hooks unavailable");
        return false;
    }
    return true;
}

} // namespace swml4::draw
