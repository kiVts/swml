#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <map>

namespace swml_gate {
using Nonce = std::array<uint8_t, 16>;
constexpr uint16_t protocol = 1;
constexpr int channel = 0x53574d; // Core-owned; distinct from game 0..7 and Network Fixes 11.
constexpr uint64_t timeout_ms = 10000, retry_ms = 500, cooldown_ms = 30000, idle_ms = 60000;
constexpr size_t max_peers = 64, wire_size = 48;
enum class Kind : uint16_t { hello = 1, answer = 2 };
enum class State { pending, allowed, rejected };
struct Packet { uint16_t version = protocol; Kind kind = Kind::hello; Nonce nonce{}, echo{}; };
inline bool nonzero(const Nonce& nonce) { for (auto b : nonce) if (b) return true; return false; }
inline std::array<uint8_t, wire_size> encode(const Packet& p) {
    std::array<uint8_t, wire_size> out{};
    constexpr char magic[] = "SWMLJOIN";
    std::memcpy(out.data(), magic, 8);
    out[8] = uint8_t(p.version); out[9] = uint8_t(p.version >> 8);
    const auto kind = uint16_t(p.kind);
    out[10] = uint8_t(kind); out[11] = uint8_t(kind >> 8);
    std::memcpy(out.data() + 16, p.nonce.data(), 16);
    std::memcpy(out.data() + 32, p.echo.data(), 16);
    return out;
}
inline bool decode(const void* data, size_t size, Packet& p) {
    if (!data || size != wire_size) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (std::memcmp(bytes, "SWMLJOIN", 8) || bytes[12] || bytes[13] || bytes[14] || bytes[15]) return false;
    p.version = uint16_t(bytes[8] | (uint16_t(bytes[9]) << 8));
    p.kind = Kind(bytes[10] | (uint16_t(bytes[11]) << 8));
    if (p.kind != Kind::hello && p.kind != Kind::answer) return false;
    std::memcpy(p.nonce.data(), bytes + 16, 16); std::memcpy(p.echo.data(), bytes + 32, 16);
    return nonzero(p.nonce) && (p.kind != Kind::hello || !nonzero(p.echo));
}
struct Peer {
    Nonce local{}, remote{};
    State state = State::pending;
    uint64_t started = 0, activity = 0, last_hello = 0, last_answer = 0;
    uint64_t last_session_check = 0;
    bool sent_hello = false, sent_answer = false, answer_needed = false, rejection_reported = false;
};
class Gate {
public:
    std::map<uint64_t, Peer> peers;
    Peer* begin(uint64_t id, uint64_t now, const Nonce& fresh) {
        if (!id || !nonzero(fresh)) return nullptr;
        auto it = peers.find(id);
        if (it != peers.end()) return &it->second;
        if (peers.size() >= max_peers) return nullptr;
        Peer p; p.local = fresh; p.started = p.activity = now;
        return &peers.emplace(id, p).first->second;
    }
    void expire(uint64_t now) {
        for (auto& [id, p] : peers)
            if (p.state == State::pending && now - p.started >= timeout_ms) p.state = State::rejected;
    }
    bool receive(uint64_t id, const Packet& packet, uint64_t now, const Nonce& fresh) {
        if (!nonzero(packet.nonce) || (packet.kind != Kind::hello && packet.kind != Kind::answer) ||
            (packet.kind == Kind::hello && nonzero(packet.echo))) return false;
        auto* p = begin(id, now, fresh);
        if (!p || p->state == State::rejected) return false;
        // Expiry is checked before accepting a late response; traffic never extends the deadline.
        if (p->state == State::pending && now - p->started >= timeout_ms) { p->state = State::rejected; return false; }
        if (packet.version != protocol) { p->state = State::rejected; return false; }
        if (packet.nonce == p->local) return false; // A reflected local packet is not a peer.
        if (nonzero(p->remote) && p->remote != packet.nonce) {
            // A restarted peer must answer a new challenge; old answers cannot unlock it.
            if (p->state != State::allowed || packet.kind != Kind::hello || !nonzero(fresh) || fresh == p->local) return false;
            Peer next; next.local = fresh; next.started = next.activity = now; *p = next;
        }
        const bool first = !nonzero(p->remote);
        // An answer must echo OUR current challenge, not just contain a recognizable header.
        if (packet.kind == Kind::answer && packet.echo != p->local) return false;
        p->remote = packet.nonce;
        p->activity = now;
        if (packet.kind == Kind::hello || first) p->answer_needed = true;
        if (packet.kind == Kind::answer) p->state = State::allowed;
        return true;
    }
    bool allowed(uint64_t id) const {
        auto it = peers.find(id); return it != peers.end() && it->second.state == State::allowed;
    }
};
}
