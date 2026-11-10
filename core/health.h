#pragma once
#include "swml_api.h"
#include <atomic>
#include <string_view>

namespace swml_health {
inline std::atomic<uint32_t> loaded{0};
inline std::atomic<bool> fault{false}, ready{false};
inline void note_fault() { fault.store(true, std::memory_order_relaxed); }
inline bool bad_status(std::string_view text) {
    return text.starts_with("failed") || text.starts_with("FAILED") ||
        text.starts_with("partly running") || text.starts_with("nothing could be installed");
}
inline int snapshot(swml_runtime_status* out) {
    if (!out || out->size < sizeof(*out)) return 0;
    const bool complete = ready.load(std::memory_order_acquire);
    *out = {sizeof(*out), loaded.load(std::memory_order_relaxed),
            uint32_t(fault.load(std::memory_order_relaxed)), uint32_t(complete)};
    return 1;
}
}
