#pragma once
#include "swml_v2.h"
#include <cstring>

// Payload inside NetworkFixes' SWRD envelope. No native game packet is changed.
namespace swml_wire {
inline constexpr uint32_t version = 1;
inline bool valid_topic(const char* topic, size_t size) {
    if (!size || size >= SWML_NAME_CAPACITY) return false;
    for (size_t i = 0; i < size; ++i) {
        char c = topic[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}
inline uint32_t encode(const swml_transport_packet& packet, uint8_t* out, uint32_t capacity) {
    const size_t name = strnlen_s(packet.topic, SWML_NAME_CAPACITY);
    if (!out || !valid_topic(packet.topic, name) || packet.data_size > SWML_NET_MAX_PAYLOAD ||
        capacity < 4 + name + packet.data_size) return 0;
    // Explicit little endian, independent of struct packing/alignment.
    out[0] = static_cast<uint8_t>(name); out[1] = 0;
    out[2] = static_cast<uint8_t>(packet.data_size); out[3] = static_cast<uint8_t>(packet.data_size >> 8);
    memcpy(out + 4, packet.topic, name);
    memcpy(out + 4 + name, packet.data, packet.data_size);
    return static_cast<uint32_t>(4 + name + packet.data_size);
}
inline bool decode(const uint8_t* data, uint32_t size, swml_transport_packet& out) {
    if (!data || size < 4) return false;
    uint32_t name = uint32_t(data[0]) | uint32_t(data[1]) << 8;
    uint32_t payload = uint32_t(data[2]) | uint32_t(data[3]) << 8;
    if (payload > SWML_NET_MAX_PAYLOAD || size != 4 + name + payload ||
        !valid_topic(reinterpret_cast<const char*>(data + 4), name)) return false;
    out = {};
    memcpy(out.topic, data + 4, name);
    out.data_size = payload;
    memcpy(out.data, data + 4 + name, payload);
    return true;
}
}
