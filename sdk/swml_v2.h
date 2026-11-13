/* SWML SDK: C ABI; no STL objects or allocator ownership cross a DLL boundary.
 * See README.md for threading, cancellation, limits, and persistence contracts. */
#ifndef SWML_V2_H
#define SWML_V2_H
#include "swml_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SWML_SDK_VERSION "4.6.2"
#define SWML_V2_VERSION 1u
#define SWML_MAX_PAYLOAD 65536u
#define SWML_NET_MAX_PAYLOAD 896u
#define SWML_NAME_CAPACITY 96u
typedef uint64_t swml_handle;
typedef int32_t swml_result;
enum {
    SWML_OK = 0, SWML_PENDING = 1,
    SWML_INVALID = -1, SWML_UNAVAILABLE = -2, SWML_NOT_FOUND = -3,
    SWML_CONFLICT = -4, SWML_LIMIT = -5, SWML_BUFFER_SMALL = -6,
    SWML_IO_ERROR = -7, SWML_DENIED = -8
};
enum { SWML_LANE_GAME = 1, SWML_LANE_RENDER = 2 };
enum {
    SWML_CAP_GAME_LOOP = 1ull << 0, SWML_CAP_RENDER = 1ull << 1,
    SWML_CAP_EVENTS = 1ull << 2, SWML_CAP_TASKS = 1ull << 3,
    SWML_CAP_SERVICES = 1ull << 4, SWML_CAP_STORAGE = 1ull << 5,
    SWML_CAP_NETWORK = 1ull << 6
};

typedef struct swml_event {
    uint32_t size, lane;
    uint64_t time_ms;             /* monotonic wall time; NOT simulation time */
    const char* topic;            /* borrowed until callback returns */
    const char* sender;           /* mod id or "swml"; network data carries its real peer */
    const void* data;             /* copied input, borrowed until callback returns */
    uint32_t data_size;
} swml_event;
typedef void (*swml_callback)(const swml_event* event, void* context);
typedef struct swml_service_header { uint32_t version, size; } swml_service_header;
typedef struct swml_peer {
    uint8_t identity[16];
    uint64_t generation; /* local session generation; stale endpoints are rejected */
} swml_peer;
typedef struct swml_network_message {
    swml_peer peer;
    uint32_t data_size;
    uint8_t data[SWML_NET_MAX_PAYLOAD];
} swml_network_message;
typedef struct swml_network_result {
    swml_handle operation;
    swml_peer peer;
    swml_result result;           /* transport acceptance, NOT application acknowledgement */
} swml_network_result;
typedef struct swml_transport_packet {
    swml_handle operation;
    swml_peer peer;
    char topic[SWML_NAME_CAPACITY];
    uint32_t data_size;
    uint8_t data[SWML_NET_MAX_PAYLOAD];
} swml_transport_packet;
typedef struct swml_runtime_stats {
    uint32_t size, subscriptions, tasks, pending_events, pending_network;
    uint64_t queued_bytes, callbacks, rejected, callback_failures;
} swml_runtime_stats;

/* Built-in topics:
 * swml.mods.ready: game lane, no data (after all mods initialize)
 * swml.loop.begin/end: game lane, end data = double tick_ms
 * swml.render.present: render lane, data = void* HDC
 * swml.render.frame: render lane, data = double frame_ms
 * <mod-id>.net.result: game lane, swml_network_result
 * A network topic delivers swml_network_message, not a raw application payload.
 */
struct swml_api_v2 {
    uint32_t version, size;
    uint64_t (*capabilities)(void);
    uint64_t (*now_ms)(void);
    const char* (*mod_id)(swml_mod* mod);
    swml_result (*subscribe)(swml_mod*, const char* topic, swml_callback, void*, swml_handle*);
    swml_result (*publish)(swml_mod*, const char* topic, const void*, uint32_t);
    /* Tasks run at a safe lane callback, never a newly-created worker. period=0 is one-shot.
     * Repeating tasks skip missed periods. delay/period use monotonic wall milliseconds. */
    swml_result (*schedule)(swml_mod*, uint32_t lane, uint64_t delay_ms, uint64_t period_ms,
                            swml_callback, void*, const void*, uint32_t, swml_handle*);
    /* Nonblocking: PENDING means a callback has already started. Keep its context alive.
     * Retry cancel until OK/NOT_FOUND before freeing context from another thread. */
    swml_result (*cancel)(swml_mod*, swml_handle);
    swml_result (*service_publish)(swml_mod*, const char*, const swml_service_header*);
    swml_result (*service_query)(swml_mod*, const char*, uint32_t version, uint32_t min_size,
                                 const swml_service_header**);
    /* UTF-16 INI setting read; needed includes terminator. No writes/default mutation. */
    swml_result (*setting_read)(swml_mod*, const wchar_t* section, const wchar_t* key,
                                wchar_t* out, uint32_t capacity, uint32_t* needed);
    /* A single named file under the mod's data directory; no path separators or '..'.
     * read supports a size probe (out=NULL, capacity=0). write flushes then atomically replaces.
     * These are mod-local blobs, NOT a transaction with the native world save. */
    swml_result (*storage_read)(swml_mod*, const wchar_t* name, void*, uint32_t, uint32_t* needed);
    swml_result (*storage_write)(swml_mod*, const wchar_t* name, const void*, uint32_t);
    swml_result (*network_peers)(swml_mod*, swml_peer*, uint32_t capacity, uint32_t* needed);
    swml_result (*network_send)(swml_mod*, const swml_peer*, const char* topic,
                                const void*, uint32_t, swml_handle* operation);
    swml_result (*stats)(swml_mod*, swml_runtime_stats*);
    /* Provider API: ONE transport owns network IO. Ordinary mods use network_send/subscribe.
     * bind returns an owner-scoped provider token; provider must withdraw peers on disconnect.
     * peek does not remove; complete removes and emits the sending mod's net.result event. */
    swml_result (*transport_bind)(swml_mod*, swml_handle* provider);
    swml_result (*transport_peer)(swml_mod*, swml_handle, const swml_peer*, int connected);
    swml_result (*transport_peek)(swml_mod*, swml_handle, swml_transport_packet*);
    swml_result (*transport_complete)(swml_mod*, swml_handle, swml_handle operation, swml_result);
    swml_result (*transport_receive)(swml_mod*, swml_handle, const swml_peer*, const char*, const void*, uint32_t);
};

static inline const swml_api_v2* swml_get_v2(const swml_api* api) {
    return SWML_API_HAS_V2(api) && api->v2->version >= SWML_V2_VERSION &&
           api->v2->size >= sizeof(swml_api_v2) ? api->v2 : NULL;
}
#ifdef __cplusplus
}
#endif
#endif
