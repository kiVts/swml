// Shared hook points owned by the core (one game/OS function, many interested mods).
//
// present / frame: gdi32!SwapBuffers (what the game's GLFW/WGL renderer calls once per frame).
// loop: the game-logic thread's ms clock function. Its update loop is
// start = clock(); ...tick...; do { t = clock(); } while (t - start < 16);
// The two calls are told apart by their return addresses (resolve.cpp, group kResolveUpdateLoop):
// the first starts an iteration (begin), the first call from the spin ends it (end, then idle).

#include "core.h"
#include "core_v2.h"
#include "input_runtime.h"
#include "multiplayer_gate.h"

#include "resolve.h"

#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace {

template <typename Cb>
struct Entry {
    swml_mod* mod;
    Cb cb;
    void* ctx;
};
struct LoopEntry {
    swml_mod* mod;
    swml_loop_begin_cb begin;
    swml_loop_end_cb end;
    swml_loop_idle_cb idle;
    void* ctx;
};

// Callback lists are copied on change; readers retain snapshots without the registration mutex.
std::mutex g_mu;
using PresentList = std::vector<Entry<swml_present_cb>>;
using FrameList = std::vector<Entry<swml_frame_cb>>;
using LoopList = std::vector<LoopEntry>;
std::atomic<std::shared_ptr<const PresentList>> g_present{std::make_shared<const PresentList>()};
std::atomic<std::shared_ptr<const FrameList>> g_frame{std::make_shared<const FrameList>()};
std::atomic<std::shared_ptr<const LoopList>> g_loop{std::make_shared<const LoopList>()};

using SwapBuffers_t = BOOL(WINAPI*)(HDC);
SwapBuffers_t g_orig_swap = nullptr;
LARGE_INTEGER g_freq{};
LONGLONG g_last_frame = 0;

using Clock_t = int(__fastcall*)();
Clock_t g_orig_clock = nullptr;
uintptr_t g_ret_start = 0, g_ret_spin = 0;
LONGLONG g_loop_start = 0; // update thread only
bool g_armed = false;
bool g_loop_ok = false;

BOOL WINAPI swap_detour(HDC dc)
{
    core_multiplayer_tick();
    sdk_input::native_present(dc);
    core_v2_pump(SWML_LANE_RENDER);
    void* hdc = dc;
    core_v2_dispatch(SWML_LANE_RENDER, "swml.render.present", &hdc, sizeof(hdc));
    auto present = g_present.load();
    for (const auto& e : *present) e.cb(dc, e.ctx);
    BOOL r = g_orig_swap(dc);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_last_frame) {
        double ms = double(now.QuadPart - g_last_frame) * 1000.0 / double(g_freq.QuadPart);
        auto frame = g_frame.load();
        for (const auto& e : *frame) e.cb(ms, e.ctx);
        core_v2_dispatch(SWML_LANE_RENDER, "swml.render.frame", &ms, sizeof(ms));
    }
    g_last_frame = now.QuadPart;
    return r;
}

int __fastcall clock_detour()
{
    uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (ra == g_ret_spin && g_armed) {
        g_armed = false;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double tick = double(now.QuadPart - g_loop_start) * 1000.0 / double(g_freq.QuadPart);
        auto list = g_loop.load();
        for (const auto& e : *list)
            if (e.end) e.end(tick, e.ctx);
        core_v2_dispatch(SWML_LANE_GAME, "swml.loop.end", &tick, sizeof(tick));
        for (const auto& e : *list)
            if (e.idle) e.idle(g_loop_start, e.ctx);
    }
    int t = g_orig_clock();
    if (ra == g_ret_start) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_loop_start = now.QuadPart;
        g_armed = true;
        core_v2_pump(SWML_LANE_GAME);
        core_v2_dispatch(SWML_LANE_GAME, "swml.loop.begin");
        auto list = g_loop.load();
        for (const auto& e : *list)
            if (e.begin) e.begin(e.ctx);
    }
    return t;
}

template <typename V, typename T>
void append(std::atomic<std::shared_ptr<const V>>& list, const T& item)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto next = std::make_shared<V>(*list.load());
    next->push_back(item);
    list.store(next); // readers retain their snapshot; old snapshots are reclaimed safely
}

template <typename V>
void drop(std::atomic<std::shared_ptr<const V>>& list, swml_mod* m)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto next = std::make_shared<V>(*list.load());
    next->erase(std::remove_if(next->begin(), next->end(), [m](const auto& e) { return e.mod == m; }), next->end());
    list.store(next);
}

} // namespace

bool core_events_install()
{
    QueryPerformanceFrequency(&g_freq);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    void* swap = gdi ? reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers")) : nullptr;
    bool present = swap && MH_CreateHook(swap, reinterpret_cast<void*>(&swap_detour), reinterpret_cast<void**>(&g_orig_swap)) == MH_OK;
    corelog("events: present/frame (gdi32!SwapBuffers) %s", present ? "OK" : "FAILED");

    if (resolve(kResolveUpdateLoop) && addrs().update_loop_ok) {
        g_ret_start = addrs().ret_loop_start;
        g_ret_spin = addrs().ret_loop_spin;
        g_loop_ok = MH_CreateHook(reinterpret_cast<void*>(addrs().clock_fn), reinterpret_cast<void*>(&clock_detour),
                                  reinterpret_cast<void**>(&g_orig_clock)) == MH_OK;
    }
    corelog("events: game loop %s", g_loop_ok ? "OK" : "unavailable (loop not recognised)");
    core_v2_available(g_loop_ok, present);
    return present;
}

void core_on_present(swml_mod* m, swml_present_cb cb, void* ctx) { append(g_present, Entry<swml_present_cb>{m, cb, ctx}); }
void core_on_frame(swml_mod* m, swml_frame_cb cb, void* ctx) { append(g_frame, Entry<swml_frame_cb>{m, cb, ctx}); }

bool core_on_loop(swml_mod* m, swml_loop_begin_cb b, swml_loop_end_cb e, swml_loop_idle_cb i, void* ctx)
{
    if (!g_loop_ok) return false;
    append(g_loop, LoopEntry{m, b, e, i, ctx});
    return true;
}

void core_events_drop(swml_mod* m)
{
    drop(g_present, m);
    drop(g_frame, m);
    drop(g_loop, m);
}
