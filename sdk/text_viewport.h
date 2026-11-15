#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace swml4::text_fields {
// Byte offsets remain in the native saved string; the displayed slice never
// becomes the value. Boundaries prevent scrolling into the middle of UTF-8.
struct TextViewport {
    struct Glyph { uint32_t byte, code; float advance, kerning; };
    std::vector<Glyph> glyphs;
    std::vector<float> prefix{0};
    uint32_t bytes = 0;
    size_t first = 0, end = 0;
    template<class Measure> void assign(std::string_view text, Measure measure) {
        glyphs.clear(); prefix.assign(1, 0); bytes = static_cast<uint32_t>(text.size());
        uint32_t previous = 0;
        for (uint32_t at = 0; at < bytes;) {
            const auto start = at;
            const auto lead = static_cast<uint8_t>(text[at++]);
            uint32_t code = lead, count = 0;
            if (lead >= 0xc2 && lead <= 0xdf) { code &= 0x1f; count = 1; }
            else if (lead >= 0xe0 && lead <= 0xef) { code &= 0xf; count = 2; }
            else if (lead >= 0xf0 && lead <= 0xf4) { code &= 7; count = 3; }
            else if (lead >= 0x80) code = 0xfffd;
            bool valid = at + count <= bytes;
            for (uint32_t i = 0; valid && i < count; ++i) {
                const auto c = static_cast<uint8_t>(text[at + i]);
                valid = (c & 0xc0) == 0x80;
                code = (code << 6) | (c & 0x3f);
            }
            if (!valid || (count == 1 && code < 0x80) || (count == 2 && code < 0x800) ||
                (count == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
                code = 0xfffd; count = 0;
            }
            at += valid ? count : 0;
            if (code < 0x20 || code == 0x7f) code = ' '; // single-line presentation only
            auto [advance, kern] = measure(previous, code);
            if (!std::isfinite(advance) || advance < 0) advance = 0;
            if (!std::isfinite(kern)) kern = 0;
            kern = std::max(-advance, kern);
            glyphs.push_back({start, code, advance, kern});
            prefix.push_back(prefix.back() + advance + kern);
            previous = code;
        }
        first = std::min(first, glyphs.size());
    }
    size_t index(uint32_t byte) const {
        auto it = std::upper_bound(glyphs.begin(), glyphs.end(), byte,
            [](uint32_t value, const Glyph& g) { return value < g.byte; });
        return byte >= bytes ? glyphs.size() : (it == glyphs.begin() ? 0 : size_t(it - glyphs.begin() - 1));
    }
    uint32_t byte(size_t index) const { return index < glyphs.size() ? glyphs[index].byte : bytes; }
    float position(size_t index) const {
        if (index <= first) return 0;
        return std::max(0.f, prefix[index] - prefix[first] - glyphs[first].kerning);
    }
    void follow(uint32_t caret, float budget) {
        const size_t active = index(caret);
        if (active < first) first = active;
        while (first < active && position(active) > budget) ++first;
        if (prefix.back() <= budget) first = 0;
        end = first;
        while (end < glyphs.size() && position(end + 1) <= budget) ++end;
    }
    uint32_t hit(float x) const {
        size_t best = first;
        float distance = std::abs(x);
        for (size_t i = first + 1; i <= end; ++i) {
            const float d = std::abs(position(i) - x);
            if (d < distance) { distance = d; best = i; }
        }
        return byte(best);
    }
    std::string visible() const {
        std::string out;
        for (size_t i = first; i < end; ++i) {
            const auto c = glyphs[i].code;
            if (c == '<') out += '<'; // native markup escape
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
            else if (c < 0x10000) { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
            else { out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        }
        return out;
    }
};
struct TextVertex { float x, y, z; uint32_t color; float u, v; };
static_assert(sizeof(TextVertex) == 24);
inline void clip_text_quad(TextVertex* q, float left, float right, float bottom, float top) {
    const float x0 = q[0].x, x1 = q[2].x, y0 = q[0].y, y1 = q[1].y;
    const float u0 = q[0].u, u1 = q[2].u, v0 = q[0].v, v1 = q[1].v;
    for (unsigned i = 0; i < 4; ++i) {
        auto& vertex = q[i];
        vertex.x = std::clamp(vertex.x, left, right);
        vertex.y = std::clamp(vertex.y, bottom, top);
        if (x1 != x0) vertex.u = u0 + (vertex.x - x0) / (x1 - x0) * (u1 - u0);
        if (y1 != y0) vertex.v = v0 + (vertex.y - y0) / (y1 - y0) * (v1 - v0);
    }
}
} // namespace swml4::text_fields
