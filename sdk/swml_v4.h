/* SDK: versioned C ABI. Every returned text buffer uses UTF-8 and needed
 * includes its terminating NUL. JSON operations use the documented bounded
 * schema subset; see V4.md. No C++ object crosses this boundary. */
#ifndef SWML_V4_H
#define SWML_V4_H
#include "swml_v3.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SWML_V4_VERSION 1u
/* The value was committed, but the bounded notification queue could not accept
 * its change event. Re-read cached settings; do not retry a non-idempotent side effect. */
enum { SWML_APPLIED_WITHOUT_EVENT = 2 };
enum { SWML_RECOVERED_BACKUP = 3 };
enum { SWML_CORRUPT = -10 };
enum {
    SWML_V4_CAP_SETTINGS = 1ull << 0, SWML_V4_CAP_TRANSLATIONS = 1ull << 1,
    SWML_V4_CAP_REGISTRIES = 1ull << 2, SWML_V4_CAP_ASSETS = 1ull << 3,
    SWML_V4_CAP_DIAGNOSTICS = 1ull << 4, SWML_V4_CAP_NATIVE_CONTROLS = 1ull << 5,
    SWML_V4_CAP_CONTEXT = 1ull << 6, SWML_V4_CAP_WORLD_STORAGE = 1ull << 7, SWML_V4_CAP_INPUT = 1ull << 8
};
enum { SWML_INPUT_PRESS=1, SWML_INPUT_RELEASE=2, SWML_INPUT_HELD=3 };
enum { SWML_KEY_CTRL=1, SWML_KEY_SHIFT=2, SWML_KEY_ALT=4, SWML_KEY_SUPER=8 };
typedef struct swml_action_desc {
    uint32_t size;
    const char* name;
    const char* native_root_class; /* native UI context, not global desktop keys */
    uint32_t key, modifiers, repeat_ms; /* Windows virtual key; repeat=0 disables held events */
} swml_action_desc;
typedef struct swml_action_event {
    uint32_t size, phase, key, modifiers;
    swml_handle action;
    uint64_t input_generation; /* input focus/context generation, not a world identity */
} swml_action_event;
enum { SWML_CONTEXT_MODE=1, SWML_CONTEXT_WORLD_ID=2, SWML_CONTEXT_ROLE=4, SWML_CONTEXT_TIME=8, SWML_CONTEXT_SAVE_EPOCH=16 };
enum { SWML_ROLE_UNKNOWN=0, SWML_ROLE_HOST=1, SWML_ROLE_CLIENT=2, SWML_ROLE_LOCAL=3 };
typedef struct swml_world_context {
    uint32_t size, fields, mode_preview, role;
    uint8_t world_id[16];
    uint64_t generation, revision, simulation_ms, save_epoch;
    char mode[48];
} swml_world_context;
enum { SWML_UI_CHECKBOX = 4, SWML_UI_SLIDER = 5, SWML_UI_EDITBOX = 6, SWML_UI_CHANGE = 5 };
typedef struct swml_ui_control_desc {
    uint32_t size;
    swml_ui_desc widget;
    double minimum, maximum, step;
    const char* value_json; /* bool, number or string according to widget.kind */
    const char* allowed_characters; /* editbox native input filter; NULL uses printable ASCII */
    uint32_t max_characters; /* 1..4096; zero selects 255 */
} swml_ui_control_desc;
typedef struct swml_v4_stats {
    uint32_t size, settings, registries, entries, languages;
    uint64_t owned_bytes, profiled_callbacks, callback_nanoseconds, longest_callback_nanoseconds;
} swml_v4_stats;
struct swml_api_v4 {
    uint32_t version, size;
    uint64_t (*capabilities)(void);
    swml_result (*settings_register)(swml_mod*, const char* schema_json);
    swml_result (*settings_schema)(swml_mod*, char* out, uint32_t capacity, uint32_t* needed);
    swml_result (*setting_get)(swml_mod*, const char* id, char* out_json, uint32_t capacity, uint32_t* needed);
    swml_result (*setting_set)(swml_mod*, const char* id, const char* value_json);
    swml_result (*settings_reload)(swml_mod*);
    swml_result (*translations_register)(swml_mod*, const char* locale, const char* table_json);
    swml_result (*translate)(swml_mod*, const char* locale, const char* key, const char* arguments_json,
                             char* out, uint32_t capacity, uint32_t* needed);
    swml_result (*registry_define)(swml_mod*, const char* name, uint32_t version, const char* schema_json);
    swml_result (*registry_put)(swml_mod*, const char* registry, const char* id, const char* scope, const char* value_json);
    swml_result (*registry_get)(swml_mod*, const char* registry, const char* id, const char* scope,
                                char* out_json, uint32_t capacity, uint32_t* needed);
    swml_result (*registry_list)(swml_mod*, const char* registry, const char* scope,
                                 char* out_json, uint32_t capacity, uint32_t* needed);
    swml_result (*asset_read)(swml_mod*, const char* relative_path, void* out, uint32_t capacity, uint32_t* needed);
    swml_result (*log_threshold)(swml_mod*, uint32_t level);
    int (*log_enabled)(swml_mod*, uint32_t level);
    swml_result (*profile_callbacks)(swml_mod*, int enabled);
    swml_result (*stats)(swml_mod*, swml_v4_stats*);
    swml_result (*ui_control_create)(swml_mod*, swml_ui_handle parent, const swml_ui_control_desc*, swml_ui_handle*);
    swml_result (*ui_value_get)(swml_mod*, swml_ui_handle, char* out_json, uint32_t capacity, uint32_t* needed);
    swml_result (*ui_value_set)(swml_mod*, swml_ui_handle, const char* value_json);
    /* Native-provider contract: providers claim disjoint fields. Ordinary mods
     * query required fields; missing native evidence returns UNAVAILABLE. */
    swml_result (*context_bind)(swml_mod*, uint32_t fields, swml_handle* provider);
    swml_result (*context_update)(swml_mod*, swml_handle provider, const swml_world_context*);
    swml_result (*context_get)(swml_mod*, uint32_t required_fields, swml_world_context*);
    /* Host-owned per-world sidecars with schema and checksum/backup recovery.
     * NOT an atomic transaction with a native Stormworks world save. */
    swml_result (*world_data_read)(swml_mod*, uint64_t generation, const char* name, uint32_t expected_schema,
                                   uint32_t* stored_schema, void* out, uint32_t capacity, uint32_t* needed);
    swml_result (*world_data_write)(swml_mod*, uint64_t generation, const char* name, uint32_t schema,
                                    const void* data, uint32_t size);
    swml_result (*action_register)(swml_mod*, const swml_action_desc*, swml_callback, void*, swml_handle*);
    swml_result (*action_rebind)(swml_mod*, swml_handle, const char* chord);
    swml_result (*action_cancel)(swml_mod*, swml_handle);
    swml_result (*action_list)(swml_mod*, char* out_json, uint32_t capacity, uint32_t* needed);
};
static inline const swml_api_v4* swml_get_v4(const swml_api* api) {
    return SWML_API_HAS_V4(api) && api->v4->version >= SWML_V4_VERSION &&
        api->v4->size >= sizeof(swml_api_v4) ? api->v4 : NULL;
}
#ifdef __cplusplus
}
#endif
#endif
