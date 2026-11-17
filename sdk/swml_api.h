/* swml_api.h - the interface between swml_core.dll and mod DLLs.
 *
 * Plain C, versioned: the core passes a pointer to this table to each mod's swml_mod_init(). New
 * fields are only ever appended; a mod checks api->version / api->size before using newer fields.
 *
 * Shared hook points (one game function, many interested mods) are owned by the core and exposed
 * as events, because a function can only be hooked once:
 *   - present: render thread, just before each SwapBuffers, with the game's GL context current
 *   - frame:   render thread, right after each SwapBuffers, with the frame time
 *   - loop:    game-logic (update) thread: iteration begin, end (tick time), idle (before the game's
 *              spin-wait for the rest of its 16 ms slot)
 */
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

/* Process-local loader snapshot. Faults latch until restart; disabled mods do not
 * count. A fault indicates a detected mod/core failure, not a crash predictor. */
typedef struct swml_runtime_status {
    uint32_t size, loaded_mods, fault_found, ready;
} swml_runtime_status;

/* Game-update porting resolved once by the core from the port data it was built with (port_runtime.h). A mod uses
 * it instead of its own compiled-in data when the core's is newer, so a loader release ports older mods too. */
typedef struct swml_port_info {
    uint32_t size;
    uint32_t running_build;  /* PE timestamp of the game exe */
    uint32_t original_build;
    uint32_t table_build;
    uint32_t sig_build;
    uint32_t field_build;    /* port_fields.h's build */
} swml_port_info;
typedef struct swml_port_range { uint32_t old_rva, size, new_rva; } swml_port_range; /* new_rva ~0u: known, not found */
typedef struct swml_port_field { char owner[16]; uint32_t original, value; } swml_port_field; /* value ~0u: unresolved */

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

    /* v2 extension. The complete v1 prefix above is ABI-stable. Check size before reading. */
    const swml_api_v2* v2;
    const swml_api_v3* v3;
    const swml_api_v4* v4;
    /* Optional extension after the frozen v4 prefix; check size before use. */
    int (*runtime_status)(swml_runtime_status*);
    void (*report_fault)(swml_mod*);
    /* Optional: port data (check SWML_API_HAS_PORT). The ranges/fields calls copy up to cap entries into out and
     * return the total; native=1 gives the ranges keyed by sig_build's own rvas. */
    int (*port_info)(swml_port_info*);
    size_t (*port_ranges)(int native, swml_port_range* out, size_t cap);
    size_t (*port_fields)(swml_port_field* out, size_t cap);
    /* Optional (check SWML_API_HAS_STATUS_LINE): this mod's segment on SWML's bottom-left line; NULL or "" removes it.
     * color: native AABBGGRR, 0 = the label's own. Any thread; the core copies the text. See status_line.h. */
    void (*status_line)(swml_mod* mod, const char* text, uint32_t color);
} swml_api;

#define SWML_API_V1_SIZE ((uint32_t)offsetof(swml_api, v2))
#define SWML_API_V2_SIZE ((uint32_t)offsetof(swml_api, v3))
#define SWML_API_V3_SIZE ((uint32_t)(offsetof(swml_api, v3) + sizeof(void*)))
#define SWML_API_V4_SIZE ((uint32_t)(offsetof(swml_api, v4) + sizeof(void*)))
#define SWML_API_HAS_V2(api) ((api) && (api)->version >= 2 && (api)->size >= SWML_API_V2_SIZE && (api)->v2)
#define SWML_API_HAS_V3(api) ((api) && (api)->version >= 3 && (api)->size >= SWML_API_V3_SIZE && (api)->v3)
#define SWML_API_HAS_V4(api) ((api) && (api)->version >= 4 && (api)->size >= SWML_API_V4_SIZE && (api)->v4)
#define SWML_API_HAS_STATUS(api) ((api) && (api)->size >= offsetof(swml_api, report_fault) + sizeof(void*) && (api)->runtime_status && (api)->report_fault)
#define SWML_API_HAS_STATUS_LINE(api) ((api) && (api)->size >= offsetof(swml_api, status_line) + sizeof(void*) && (api)->status_line)
#define SWML_API_HAS_PORT(api) ((api) && (api)->size >= offsetof(swml_api, port_fields) + sizeof(void*) && (api)->port_info && (api)->port_ranges && (api)->port_fields)
static inline void swml_report_fault(const swml_api* api, swml_mod* mod) {
    if (SWML_API_HAS_STATUS(api)) api->report_fault(mod);
}

/* Every mod DLL exports this. Return 1 if the mod is running, 0 to be unloaded (its hooks are
 * removed; call set_status first to say why). */
typedef int (*swml_mod_init_fn)(const swml_api* api, swml_mod* mod);
#define SWML_MOD_INIT_NAME "swml_mod_init"

#ifdef __cplusplus
}
#endif
#endif
