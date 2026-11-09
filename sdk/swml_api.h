#ifndef SWML_API_H
#define SWML_API_H

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWML_API_VERSION 4

typedef struct swml_mod swml_mod; /* opaque, one per loaded mod */
typedef struct swml_api_v2 swml_api_v2;
typedef struct swml_api_v3 swml_api_v3;
typedef struct swml_api_v4 swml_api_v4;

typedef void (*swml_present_cb)(void* hdc, void* ctx);
typedef void (*swml_frame_cb)(double frame_ms, void* ctx);
typedef void (*swml_loop_begin_cb)(void* ctx);
typedef void (*swml_loop_end_cb)(double tick_ms, void* ctx);
/* start_qpc: QueryPerformanceCounter value at the start of this iteration. The game spins until 16
 * whole ms of its clock have passed (it can end as early as 15.0 ms); a callback may sleep. */
typedef void (*swml_loop_idle_cb)(int64_t start_qpc, void* ctx);

typedef struct swml_api {
    uint32_t version; /* SWML_API_VERSION of the core */
    uint32_t size;    /* sizeof(swml_api) of the core */

    /* logging: one line in logs\mods\<mod id>.log, tagged with the mod id */
    void (*log)(swml_mod* mod, const char* message);

    /* paths (no trailing backslash) */
    const wchar_t* (*mod_dir)(swml_mod* mod);   /* this mod's folder */
    const wchar_t* (*logs_dir)(void);           /* where output files go */
    const wchar_t* (*settings_path)(swml_mod* mod); /* this mod's settings.ini */

    /* game */
    uintptr_t (*game_base)(void);
    int (*game_build_known)(void); /* 1 if the exe is the build fixed addresses were taken from */

    /* hooks: created now, enabled together once every mod has initialised (or immediately, when
     * called later). Returns 1 on success. If another mod already hooks the same target, the hooks are chained:
     * the newest detour runs first and its *original is the previous mod's detour (the game code last). */
    int (*hook)(swml_mod* mod, void* target, void* detour, void** original, const char* what);
    int (*hook_export)(swml_mod* mod, const wchar_t* module, const char* proc, void* detour, void** original);

    /* events */
    void (*on_present)(swml_mod* mod, swml_present_cb cb, void* ctx);
    void (*on_frame)(swml_mod* mod, swml_frame_cb cb, void* ctx);
    int (*on_loop)(swml_mod* mod, swml_loop_begin_cb begin, swml_loop_end_cb end, swml_loop_idle_cb idle, void* ctx);

    /* status shown in the mod window ("OK", or why the mod is off) */
    void (*set_status)(swml_mod* mod, const char* text);

    /* extension. The complete prefix above is ABI-stable. Check size before reading. */
    const swml_api_v2* v2;
    const swml_api_v3* v3;
    const swml_api_v4* v4;
} swml_api;

#define SWML_API_V1_SIZE ((uint32_t)offsetof(swml_api, v2))
#define SWML_API_V2_SIZE ((uint32_t)offsetof(swml_api, v3))
#define SWML_API_V3_SIZE ((uint32_t)(offsetof(swml_api, v3) + sizeof(void*)))
#define SWML_API_V4_SIZE ((uint32_t)(offsetof(swml_api, v4) + sizeof(void*)))
#define SWML_API_HAS_V2(api) ((api) && (api)->version >= 2 && (api)->size >= SWML_API_V2_SIZE && (api)->v2)
#define SWML_API_HAS_V3(api) ((api) && (api)->version >= 3 && (api)->size >= SWML_API_V3_SIZE && (api)->v3)
#define SWML_API_HAS_V4(api) ((api) && (api)->version >= 4 && (api)->size >= SWML_API_V4_SIZE && (api)->v4)

/* Every mod DLL exports this. Return 1 if the mod is running, 0 to be unloaded (its hooks are
 * removed; call set_status first to say why). */
typedef int (*swml_mod_init_fn)(const swml_api* api, swml_mod* mod);
#define SWML_MOD_INIT_NAME "swml_mod_init"

#ifdef __cplusplus
}
#endif
#endif
