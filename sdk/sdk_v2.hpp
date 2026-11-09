#pragma once
#include "sdk.h"
#include "swml_v2.h"
#include "log.h"
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace swml2 {
inline const swml_api_v2* api() { return swml_get_v2(swml()); }
inline bool require(uint64_t capabilities, const char* feature) {
    const auto* v2 = api();
    if (v2 && (v2->capabilities() & capabilities) == capabilities) return true;
    mod_status("%s: required SDK capability unavailable", feature);
    return false;
}
inline swml_result on(const char* topic, swml_callback callback, void* context, swml_handle& handle) {
    handle = 0;
    return api() ? api()->subscribe(swml_self(), topic, callback, context, &handle) : SWML_UNAVAILABLE;
}
inline swml_result emit(const char* topic, const void* data = nullptr, uint32_t size = 0) {
    return api() ? api()->publish(swml_self(), topic, data, size) : SWML_UNAVAILABLE;
}
inline swml_result task(uint32_t lane, swml_callback callback, void* context, swml_handle& handle,
                        uint64_t delay_ms = 0, uint64_t period_ms = 0, const void* data = nullptr, uint32_t size = 0) {
    handle = 0;
    return api() ? api()->schedule(swml_self(), lane, delay_ms, period_ms, callback, context, data, size, &handle)
                 : SWML_UNAVAILABLE;
}
inline swml_result cancel(swml_handle& handle) {
    if (!handle) return SWML_OK;
    auto result = api() ? api()->cancel(swml_self(), handle) : SWML_UNAVAILABLE;
    if (result == SWML_OK || result == SWML_NOT_FOUND) handle = 0;
    return result;
}
template<class T> const T* service(const char* name, uint32_t version = 1) {
    static_assert(sizeof(T) >= sizeof(swml_service_header));
    const swml_service_header* result = nullptr;
    if (!api() || api()->service_query(swml_self(), name, version, sizeof(T), &result) != SWML_OK) return nullptr;
    return reinterpret_cast<const T*>(result);
}
inline swml_result setting(const wchar_t* section, const wchar_t* key, std::wstring& value) {
    value.clear();
    if (!api()) return SWML_UNAVAILABLE;
    uint32_t needed = 0;
    auto result = api()->setting_read(swml_self(), section, key, nullptr, 0, &needed);
    if (result != SWML_BUFFER_SMALL) return result;
    std::vector<wchar_t> buffer(needed);
    result = api()->setting_read(swml_self(), section, key, buffer.data(), needed, &needed);
    if (result == SWML_OK) value.assign(buffer.data());
    return result;
}
inline bool boolean(const wchar_t* section, const wchar_t* key, bool fallback) {
    std::wstring value;
    if (setting(section, key, value) != SWML_OK) return fallback;
    if (value == L"1" || _wcsicmp(value.c_str(), L"true") == 0 || _wcsicmp(value.c_str(), L"on") == 0) return true;
    if (value == L"0" || _wcsicmp(value.c_str(), L"false") == 0 || _wcsicmp(value.c_str(), L"off") == 0) return false;
    swlog("invalid boolean setting [%ls] %ls; using default", section, key); return fallback;
}
inline int64_t integer(const wchar_t* section, const wchar_t* key, int64_t fallback, int64_t low, int64_t high) {
    std::wstring value;
    if (low > high || fallback < low || fallback > high) return fallback;
    if (setting(section, key, value) != SWML_OK) return fallback;
    wchar_t* end = nullptr; errno = 0;
    auto n = wcstoll(value.c_str(), &end, 10);
    if (!errno && end != value.c_str() && !*end && n >= low && n <= high) return n;
    swlog("invalid integer setting [%ls] %ls; using default", section, key); return fallback;
}
inline double number(const wchar_t* section, const wchar_t* key, double fallback, double low, double high) {
    std::wstring value;
    if (low > high || !std::isfinite(fallback) || fallback < low || fallback > high) return fallback;
    if (setting(section, key, value) != SWML_OK) return fallback;
    wchar_t* end = nullptr; errno = 0;
    double n = wcstod(value.c_str(), &end);
    if (!errno && end != value.c_str() && !*end && std::isfinite(n) && n >= low && n <= high) return n;
    swlog("invalid numeric setting [%ls] %ls; using default", section, key); return fallback;
}
inline swml_result read(const wchar_t* name, std::vector<uint8_t>& bytes) {
    bytes.clear();
    if (!api()) return SWML_UNAVAILABLE;
    uint32_t needed = 0;
    auto result = api()->storage_read(swml_self(), name, nullptr, 0, &needed);
    if (result != SWML_BUFFER_SMALL) return result;
    bytes.resize(needed);
    result = api()->storage_read(swml_self(), name, bytes.data(), needed, &needed);
    if (result == SWML_OK) bytes.resize(needed); else bytes.clear();
    return result;
}
inline swml_result write(const wchar_t* name, const void* data, uint32_t size) {
    return api() ? api()->storage_write(swml_self(), name, data, size) : SWML_UNAVAILABLE;
}
inline swml_result peers(std::vector<swml_peer>& result) {
    result.clear(); if (!api()) return SWML_UNAVAILABLE;
    uint32_t count = 0;
    auto r = api()->network_peers(swml_self(), nullptr, 0, &count);
    if (r != SWML_BUFFER_SMALL && r != SWML_OK) return r;
    result.resize(count);
    r = api()->network_peers(swml_self(), result.data(), count, &count);
    if (r == SWML_OK) result.resize(count); else result.clear();
    return r;
}
inline swml_result send(const swml_peer& peer, const char* topic, const void* data,
                        uint32_t size, swml_handle& operation) {
    operation = 0;
    return api() ? api()->network_send(swml_self(), &peer, topic, data, size, &operation) : SWML_UNAVAILABLE;
}
}
