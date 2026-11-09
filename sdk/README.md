# SWML SDK

Native buttons now resize their background geometry through the game's size
method. Require `SWML_V3_CAP_UI_BUTTON_RESIZE` when fitting dynamic backgrounds.

Native labels can opt into measured wrapped height with a creation height of
zero; require `SWML_V3_CAP_UI_TEXT_AUTO_HEIGHT`. Nonzero heights retain their
existing behavior. See [native label sizing](V3.md#resizing-panels-after-editing-text).

SDK adds a compatible C API, C++ helpers, a standalone build workflow and shared runtime services for native Stormworks mods. It targets Windows x64/MSVC, like the existing loader. It is this project's SDK, not an official Stormworks SDK.

## History

`native_draw.h` (source-level, C ABI unchanged) draws through the game's own line primitive in its flight pass and
editor marker helper, found by run-time signature and failing closed. Prefer it, with native labels for text, over
`gl_overlay.h`.

## What is implemented

## Create and build a mod

From the repository root:

```powershell
python swml/tools/new_mod.py my_mod --output my_mod
cmake -S my_mod -B my_mod/build -G "Visual Studio 17 2022" -A x64
cmake --build my_mod/build --config Release -- /m:1 /nodeReuse:false
```

In a packaged SDK, use `tools/new_mod.py` at the archive root. The generated project points at that SDK, and accepts `-DSWML_SDK_DIR=...` if it moves. It builds only the SDK libraries and your mod; it does not fetch MinHook/libdeflate or rebuild the loader.

The generated `build/Release/` contains the DLL, `mod.ini` and `settings.default.ini`. Put them in a loader mod directory matching the mod ID. New example/generated mods are disabled by default. The generator refuses to overwrite an existing path. Mods needing networking must be installed on both communicating machines with a compatible core and NetworkFixes transport.

The package's `runtime/` tree mirrors the loader root: `swml_core.dll` and `mods/network_fixes/` with the matching DLL, version manifest and guide. Preserve user settings and enabled-state configuration when copying those runtime files. Restart the game after updating; replacing a file does not upgrade an already loaded DLL. The SDK package is not a standalone game launcher: retain your existing SWML installation.

## Minimal C++ mod

```cpp
#include "sdk_v2.hpp"

namespace {
void tick(const swml_event* event, void*) {
    // The game-logic thread. Keep this short.
}
bool setup() {
    if (!swml2::require(SWML_CAP_GAME_LOOP, "My feature")) return false;
    swml_handle handle = 0;
    return swml2::on("swml.loop.begin", tick, nullptr, handle) == SWML_OK;
}
ModSetup registration{setup};
}
```

The SDK exports `swml_mod_init` and runs the registered setup functions. A mod no longer needs to manufacture a private hook just to stay loaded. Existing `HookRegistrar`, `hook_game`, `hook_address`, `present_add_callback`, logging and overlay helpers remain available.

## ABI and capabilities

A binary continues using that prefix on a core. Newly compiled -style mods can run on a core when they require no services. A mod requiring should fail with a clear status when `swml_get_v2` returns null. Version/size checks are part of the contract, not optional boilerplate.

`capabilities()` reports available core facilities. In particular, game-loop and render bits depend on successfully installed native hooks. The network bit means at least one compatible transport peer is currently registered; it does not identify the host or grant authority over the world. Network-capable mods should wait for peers rather than require that bit during startup.

## Events and threads

| Topic | Lane | Payload |
|---|---|---|
| `swml.mods.ready` | Game | None; once after all mods initialize. |
| `swml.loop.begin` | Game | None. |
| `swml.loop.end` | Game | `double` measured tick milliseconds. |
| `swml.render.present` | Render | `void*` HDC; before SwapBuffers, GL context current. |
| `swml.render.frame` | Render | `double` frame milliseconds. |
| `<id>.net.result` | Game | `swml_network_result`. |

`mods.ready` is **loader initialization**, not “world loaded.” It is the right time to query another mod's service after alphabetical initialization has finished. Real world/save/vehicle lifecycle events need an audited provider; do not reinterpret a render or loader event as one.

Mods publish only inside their own `<mod-id>.` namespace. Other mods can subscribe to those topics. Topic names are 1–95 ASCII letters, digits, underscores, hyphens or dots. There are no wildcard subscriptions.

```cpp
swml_handle listener = 0;
swml2::on("my_mod.changed", callback, context, listener);
uint32_t value = 42;
swml_result result = swml2::emit("my_mod.changed", &value, sizeof(value));
```

Custom events are copied and queued to the game lane. They are not synchronous or latched; an event with no subscribers is discarded successfully. A callback's event/topic/sender/data pointers expire when it returns. Put stable IDs and serialized values in payloads, not unowned pointers to native objects. Custom local events and network messages should use different topic names because their payload envelopes differ.

Callbacks may register/cancel work without holding a core callback-list lock. Built-in listeners run synchronously at their native hook; custom event fanout runs during the bounded game pump. A C++ exception or hardware fault in a callback disables that registration and increments diagnostics. This contains a callback failure; it cannot roll back native state already changed by the callback.

Lane ownership does not make every native write safe. Physics/editing can have worker jobs outstanding. Use the game's verified event/fence boundary for actual object mutation, and immutable snapshots for UI.

## Tasks, timers and cancellation

```cpp
swml_handle timer = 0;
swml2::task(SWML_LANE_GAME, callback, context, timer, 1000, 5000);
// First run after at least 1 s; repeat with at least 5 s between completed runs.
```

Tasks are deferred to the selected lane; no SDK worker thread is created. Task input bytes are copied. Time is monotonic **wall time**, not simulated world time. Pausing, loading and time acceleration therefore need explicit handling for gameplay timers. Timers skip missed intervals, avoiding catch-up storms. Repeating work is scheduled from completion time and may drift; it is not a fixed-step physics clock.

Cancel with `swml2::cancel(handle)` or the C function. `OK`/`NOT_FOUND` mean no future callback can start for that registration and no callback is still using its context. `PENDING` means a callback already started; keep its context alive and retry later. Cancellation is nonblocking, including self-cancellation, so game/render threads cannot deadlock waiting on one another. A self-cancelling callback can finish normally.

There is intentionally no RAII wrapper that silently frees callback context while a different thread is still executing it. Native mod DLLs remain pinned by the loader; runtime DLL hot-unloading is not supported.

## Shared services

Providers publish a static C-compatible table with `swml_service_header {version, size}` first:

```cpp
struct InventoryService {
    swml_service_header header;
    int (*count)(uint64_t container, uint32_t material, int64_t* quantity);
};
// Provider owns the table and documents thread, authority and handle-lifetime rules.
static const InventoryService service{{1, sizeof(InventoryService)}, count_impl};
swml2::api()->service_publish(swml_self(), "my_mod.inventory", &service.header);

// Consumer, typically from swml.mods.ready:
const auto* inventory = swml2::service<InventoryService>("my_mod.inventory", 1);
```

Version matching is exact. Append fields within a compatible service version and request the minimum size needed; change version for incompatible behavior/layout. Duplicate publications are rejected. Tables and anything they reference must remain valid for the process lifetime. A failed mod's services are withdrawn before `mods.ready`. Query after readiness and do not retain a service from a provider still initializing.

The registry is in-process interoperability, not a sandbox. A table lookup does not make its functions thread-safe, host-authoritative or transactional. Providers must state those contracts. No allocator-owned C++ objects should cross DLL boundaries.

## Settings and storage

```cpp
bool enabled = swml2::boolean(L"feature", L"enabled", true);
int64_t count = swml2::integer(L"feature", L"count", 8, 1, 64);
double gain = swml2::number(L"feature", L"gain", 1.0, 0.0, 4.0);
std::vector<uint8_t> bytes;
swml_result result = swml2::read(L"state.bin", bytes);
result = swml2::write(L"state.bin", data, size);
```

Typed reads reject malformed/out-of-range values and NaN/infinity; they return the supplied fallback. They do not change the file. Missing and intentionally empty string settings are distinct. String lengths include the terminator when reported by the C setting API.

Storage writes create a unique temporary file, write and flush it, then replace the named file with write-through publication. Reads support size probes. Files are limited to 16 MiB. Names are single filename components with letters/digits/underscore/hyphen/dot; separators, `..`, trailing dots and directory/reparse targets are rejected. This is a convenience boundary, not protection from a malicious native DLL.

Storage calls perform synchronous IO. Call them on a suitable worker with copied immutable data when they might stall, then schedule a game-lane completion. They are mod-local persistence; world IDs, checkpoints, schema migration and coordinated native saves remain the mod/provider's responsibility. Atomic replacement of one file does not make multiple files atomic.

## Multiplayer

The core owns the application queue. NetworkFixes remains the only receiver of Steam channel 11, extending its existing `SWRD` envelope with types 12 (SDK capability) and 13 (SDK payload). Other mods never poll that channel. Vanilla game packets and existing redundancy message types are unchanged.

```cpp
std::vector<swml_peer> peers;
if (swml2::peers(peers) == SWML_OK) {
    for (const auto& peer : peers) {
        swml_handle operation = 0;
        swml_result result = swml2::send(peer, "my_mod.command", payload, size, operation);
    }
}
```

Subscribe to `my_mod.command` to receive `swml_network_message`. Check `event->data_size` before reading its header, then verify `message.data_size` against the available bytes. The peer is supplied by the authenticated transport identity, not an application field supplied by the sender. Your payload is at `message.data`. Payload limit is 896 bytes; larger transfers need application chunking with explicit bounds and acknowledgement.

`network_send(OK)` means queued locally. `<id>.net.result` reports whether Steam accepted it or the endpoint/send failed. This is **not** confirmation that the remote application executed an operation. Steam reliable delivery does not replace operation IDs, world/rules handshakes, host validation, application acknowledgements, timeout/retry and idempotency. Queue saturation can reject receive delivery; the receiver records this in rejected statistics, and critical applications must retry unacknowledged operations. Core lifecycle and transport-result topics cannot be injected by remote packets.

Network IO is performed on the existing network thread; received callbacks run on the game lane. Queue/provider ownership prevents mods from stealing messages, but a native mod is still trusted code. The transport does not decide host/client roles or provide automatic host migration. Live two-machine behavior must be validated for each gameplay protocol.

## Native hooks and discovery

The original mapped/signature-checked `game_live`, `game_ptr`, `hook_game` and `game_live_native` helpers remain the preferred path for audited repository addresses. Follow the repository address rules; do not introduce raw executable-base offsets.

For reusable dynamic discovery, include `resolve.h`:

```cpp
uintptr_t match = native_find_pattern("48 8B ?? ?? ?? 89", ".text");
uintptr_t table = native_find_vtable("c_example_class");
uintptr_t begin = 0, end = 0;
bool bounded = native_function_bounds(match, &begin, &end);
```

The example pattern/class are illustrative, not verified game integration points. Pattern lookup accepts up to 512 bytes and returns zero for malformed, missing or ambiguous matches. Scanning is restricted to `.text`, `.rdata` or `.data` in the current executable. It scans the live image: earlier detours may hide a pattern. Do it at initialization/feature activation, never every tick.

## Limits and performance

Current limits are 2,048 registrations globally, 256 per mod, 1,024 queued custom events, 8 MiB copied task/event input, 256 pending outgoing network messages, 64 registered peers, 256 services, and 64 KiB local payloads. Network payloads have their smaller bound. Input validation and queue saturation return explicit result codes.

Each lane pump processes at most 64 queued operations and checks a 2 ms budget between operations. Broadcast fanout and individual callbacks cannot be preempted; this is not a hard frame-time guarantee. Built-in callbacks also run synchronously. Keep all callbacks short and profile real workloads. Cancelled/finished tasks and old callback snapshots are reclaimed instead of leaking on every registration change.

`stats()` reports current registrations, queue sizes/bytes and cumulative callbacks/rejections/failures. These are core-wide diagnostics, not per-feature CPU measurements.

## Build, package and validation

```powershell
cmake -S swml -B swml/build -DSWML_SDK_TESTS=ON
cmake --build swml/build --config Release -- /m:1 /nodeReuse:false
ctest --test-dir swml/build -C Release --output-on-failure
python swml/tools/package_sdk.py
```

The package contains SDK sources/headers, the generator, example, matching runtime DLLs and SHA-256 manifest. `--source-only` omits DLLs. Standalone generated builds are an additional smoke check.

## Building from this repository

`port_table.h` and `port_sigs.h` are generated per game build and aren't in this repository. Point the build at them:

```
cmake -B build -A x64 -DSWML_GAME_DATA_DIR=path/to/generated/headers
```
