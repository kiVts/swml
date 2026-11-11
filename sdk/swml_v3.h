/* SDK: native game UI, commands, structured logging and writable settings.
 * tables remain unchanged. Native UI is NOT a SwapBuffers/GL overlay. */
#ifndef SWML_V3_H
#define SWML_V3_H
#include "swml_v2.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SWML_V3_VERSION 1u
enum { SWML_WRONG_THREAD = -9 };
enum { SWML_LOG_DEBUG = 0, SWML_LOG_INFO = 1, SWML_LOG_WARNING = 2, SWML_LOG_ERROR = 3 };
enum { SWML_LANE_UI = 3 };
enum {
    SWML_V3_CAP_COMMANDS = 1ull << 0, SWML_V3_CAP_SETTINGS_WRITE = 1ull << 1,
    SWML_V3_CAP_LOGGING = 1ull << 2, SWML_V3_CAP_NATIVE_UI = 1ull << 3,
    SWML_V3_CAP_UI_APPLICATION_CONTEXT = 1ull << 4,
    /* Labels created with height=0 use native wrapped-content height. */
    SWML_V3_CAP_UI_TEXT_AUTO_HEIGHT = 1ull << 5,
    /* Button bounds resize their native background geometry as well as captions. */
    SWML_V3_CAP_UI_BUTTON_RESIZE = 1ull << 6,
    SWML_V3_CAP_UI_STATIC_TEXT_OBSERVER = 1ull << 7
};
enum { SWML_UI_CONTAINER = 1, SWML_UI_LABEL = 2, SWML_UI_BUTTON = 3 };
enum { SWML_UI_MOUNT = 1, SWML_UI_UPDATE = 2, SWML_UI_CLICK = 3, SWML_UI_UNMOUNT = 4 };
enum {
    SWML_UI_TOP_LEFT = 0, SWML_UI_TOP_CENTER = 1, SWML_UI_TOP_RIGHT = 2,
    SWML_UI_MIDDLE_LEFT = 3, SWML_UI_CENTER = 4, SWML_UI_MIDDLE_RIGHT = 5,
    SWML_UI_BOTTOM_LEFT = 6, SWML_UI_BOTTOM_CENTER = 7, SWML_UI_BOTTOM_RIGHT = 8
};
typedef uint64_t swml_ui_handle;
typedef struct swml_ui_rect { float x, y, width, height; uint32_t anchor; } swml_ui_rect;
typedef struct swml_ui_desc {
    uint32_t size, kind;
    swml_ui_rect rect;                 /* native parent-relative units, not screen pixels */
    const char* text;                  /* UTF-8, copied into the game's heap */
    uint32_t foreground, background, hover, pressed; /* native AABBGGRR */
    float font_size;
    uint32_t text_align, visible, enabled;
} swml_ui_desc;
typedef struct swml_ui_info {
    uint32_t size, kind, owned, visible, enabled;
    swml_ui_rect rect;
} swml_ui_info;
typedef struct swml_ui_event {
    uint32_t size, kind;
    swml_handle watch;
    swml_ui_handle root, element;
} swml_ui_event;
typedef void (*swml_ui_callback)(const swml_ui_event*, void* context);
typedef struct swml_command_info {
    char name[SWML_NAME_CAPACITY];
    char help[256];
} swml_command_info;

struct swml_api_v3 {
    uint32_t version, size;
    uint64_t (*capabilities)(void);
    const char* (*result_name)(swml_result);
    swml_result (*log)(swml_mod*, uint32_t level, const char* message);
    swml_result (*setting_write)(swml_mod*, const wchar_t* section, const wchar_t* key, const wchar_t* value);
    /* Commands execute later on the game lane. event->sender identifies the local caller;
     * data is a copied, NUL-terminated UTF-8 argument string. Commands are not remote RPC. */
    swml_result (*command_register)(swml_mod*, const char* name, const char* help, swml_callback, void*, swml_handle*);
    swml_result (*command_remove)(swml_mod*, swml_handle);
    swml_result (*command_run)(swml_mod*, const char* name, const char* arguments);
    swml_result (*command_list)(swml_mod*, swml_command_info*, uint32_t capacity, uint32_t* needed);
    /* Lazy opt-in: verify bindings and install game-side UI lifecycle hooks. */
    swml_result (*ui_enable)(swml_mod*);
    swml_result (*ui_watch)(swml_mod*, const char* native_class, swml_ui_callback, void*, swml_handle*);
    swml_result (*ui_unwatch)(swml_mod*, swml_handle);
    /* The following operations require a MOUNT/UPDATE/CLICK callback for the same watch.
     * UNMOUNT handles are already invalid. Use ui_unwatch from any thread to stop a watch.
     * Existing widgets can be inspected/edited but not deleted. New widgets belong to one mod. */
    swml_result (*ui_create)(swml_mod*, swml_ui_handle parent, const swml_ui_desc*, swml_ui_handle*);
    swml_result (*ui_destroy)(swml_mod*, swml_ui_handle);
    swml_result (*ui_children)(swml_mod*, swml_ui_handle, swml_ui_handle*, uint32_t capacity, uint32_t* needed);
    swml_result (*ui_find_class)(swml_mod*, swml_ui_handle, const char* native_class, uint32_t occurrence, swml_ui_handle*);
    swml_result (*ui_inspect)(swml_mod*, swml_ui_handle, swml_ui_info*);
    swml_result (*ui_set_rect)(swml_mod*, swml_ui_handle, const swml_ui_rect*);
    swml_result (*ui_set_visible)(swml_mod*, swml_ui_handle, int);
    swml_result (*ui_set_enabled)(swml_mod*, swml_ui_handle, int);
    swml_result (*ui_set_text)(swml_mod*, swml_ui_handle, const char*);
    swml_result (*ui_get_text)(swml_mod*, swml_ui_handle, char*, uint32_t capacity, uint32_t* needed);
    /* Optional: match the native root's initial bounds before allocating handles.
       * Nonmatching generic composites consume no root slots. Later edits do not unmount.
       * With STATIC_TEXT_OBSERVER, height=-1 matches any measured height; other bounds remain exact. */
    swml_result (*ui_watch_rect)(swml_mod*, const char* native_class, const swml_ui_rect*, swml_ui_callback, void*, swml_handle*);
};
#define SWML_V3_BASE_SIZE ((uint32_t)offsetof(swml_api_v3, ui_watch_rect))
#define SWML_V3_HAS_WATCH_RECT(api) ((api) && (api)->size >= sizeof(swml_api_v3) && (api)->ui_watch_rect)
static inline const swml_api_v3* swml_get_v3(const swml_api* api) {
    return SWML_API_HAS_V3(api) && api->v3->version >= SWML_V3_VERSION &&
        api->v3->size >= SWML_V3_BASE_SIZE ? api->v3 : NULL;
}
#ifdef __cplusplus
}
#endif
#endif
