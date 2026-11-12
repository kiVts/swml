#pragma once
// UTF-8 <-> UTF-32 conversion shared by the POSIX platform halves.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace swml_platform::detail {

inline void append_utf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

inline std::wstring decode_utf8(std::string_view text, bool strict)
{
    std::wstring out;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { if (strict) throw std::runtime_error("invalid UTF-8 text"); out += wchar_t(0xFFFD); i++; continue; }
        if (extra > 0 && i + size_t(extra) >= text.size()) {
            if (strict) throw std::runtime_error("invalid UTF-8 text");
            out += wchar_t(0xFFFD); break;
        }
        bool ok = true;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (i + size_t(k) < text.size()) ? static_cast<unsigned char>(text[i + size_t(k)]) : 0;
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
            (extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) {
            if (strict) throw std::runtime_error("invalid UTF-8 text");
            out += wchar_t(0xFFFD); i++; continue;
        }
        out += wchar_t(cp);
        i += size_t(extra) + 1;
    }
    return out;
}

inline std::string encode_utf8(const std::wstring& text)
{
    std::string out;
    for (wchar_t c : text) {
        uint32_t cp = uint32_t(c);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) throw std::runtime_error("invalid wide text");
        append_utf8(out, cp);
    }
    return out;
}

} // namespace swml_platform::detail
