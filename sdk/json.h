#pragma once
// Bounded JSON values for metadata/content tooling. No platform calls or DLL ABI.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swml_json {
struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data = nullptr;
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : data(v) {}
    Value(double v) : data(v) {}
    Value(int v) : data(double(v)) {}
    Value(const char* v) : data(std::string(v)) {}
    Value(std::string v) : data(std::move(v)) {}
    Value(Array v) : data(std::move(v)) {}
    Value(Object v) : data(std::move(v)) {}
    bool is_object() const { return std::holds_alternative<Object>(data); }
    bool is_array() const { return std::holds_alternative<Array>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_number() const { return std::holds_alternative<double>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    const Object& object() const { return std::get<Object>(data); }
    const Array& array() const { return std::get<Array>(data); }
    const std::string& string() const { return std::get<std::string>(data); }
    double number() const { return std::get<double>(data); }
    bool boolean() const { return std::get<bool>(data); }
    const Value* find(std::string_view key) const {
        if (!is_object()) return nullptr;
        auto it = object().find(key); return it == object().end() ? nullptr : &it->second;
    }
    std::string text(std::string_view key, std::string fallback = {}) const {
        auto* v = find(key); return v && v->is_string() ? v->string() : fallback;
    }
};
inline bool valid_utf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        uint32_t c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) continue;
        unsigned n; uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; c &= 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; c &= 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; c &= 7; minimum = 0x10000; }
        else return false;
        while (n--) {
            if (i == text.size()) return false;
            auto b = static_cast<unsigned char>(text[i++]);
            if ((b & 0xc0) != 0x80) return false;
            c = (c << 6) | (b & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
class Parser {
    std::string_view text;
    size_t at = 0, nodes = 0;
    [[noreturn]] void fail() const { throw std::runtime_error("invalid JSON at byte " + std::to_string(at)); }
    void space() { while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at; }
    bool take(char c) { space(); if (at < text.size() && text[at] == c) { ++at; return true; } return false; }
    uint32_t hex() {
        uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            if (at == text.size()) fail();
            char c = text[at++]; unsigned digit;
            if (c >= '0' && c <= '9') digit = unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') digit = unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = unsigned(c - 'A' + 10);
            else { fail(); }
            code = (code << 4) | digit;
        }
        return code;
    }
    static void append_utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    std::string string() {
        if (!take('"')) fail();
        std::string out;
        while (at < text.size()) {
            auto c = static_cast<unsigned char>(text[at++]);
            if (c == '"') return out;
            if (c < 32) fail();
            if (c != '\\') { out += char(c); continue; }
            if (at == text.size()) fail();
            switch (text[at++]) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                uint32_t code = hex();
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (at + 2 > text.size() || text.substr(at, 2) != "\\u") fail();
                    at += 2; uint32_t low = hex();
                    if (low < 0xdc00 || low > 0xdfff) fail();
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) fail();
                append_utf8(out, code); break;
            }
            default: fail();
            }
        }
        fail();
    }
    Value value(unsigned depth) {
        space();
        if (depth > 32 || ++nodes > 16384 || at == text.size()) fail();
        if (text[at] == '"') return string();
        if (take('{')) {
            Value::Object object;
            if (take('}')) return object;
            do {
                auto key = string(); if (!take(':')) fail();
                if (!object.emplace(std::move(key), value(depth + 1)).second) fail();
                if (take('}')) return object;
            } while (take(','));
            fail();
        }
        if (take('[')) {
            Value::Array array;
            if (take(']')) return array;
            do { array.push_back(value(depth + 1)); if (take(']')) return array; } while (take(','));
            fail();
        }
        for (auto literal : {std::string_view("true"), std::string_view("false"), std::string_view("null")})
            if (text.substr(at, literal.size()) == literal) {
                at += literal.size(); return literal == "null" ? Value{} : Value(literal == "true");
            }
        const size_t begin = at;
        if (text[at] == '-') ++at;
        if (at == text.size()) fail();
        if (text[at] == '0') ++at;
        else { if (text[at] < '1' || text[at] > '9') fail(); while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at; }
        if (at < text.size() && text[at] == '.') {
            ++at; size_t digits = at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
            if (digits == at) fail();
        }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            ++at; if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
            size_t digits = at; while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
            if (digits == at) fail();
        }
        double number = 0;
        auto converted = std::from_chars(text.data() + begin, text.data() + at, number);
        if (converted.ec != std::errc{} || converted.ptr != text.data() + at || !std::isfinite(number)) fail();
        return number;
    }
public:
    explicit Parser(std::string_view input) : text(input) {}
    Value parse() {
        if (text.size() > 1024 * 1024 || !valid_utf8(text)) fail();
        auto result = value(0); space(); if (at != text.size()) fail(); return result;
    }
};
inline Value parse(std::string_view text) { return Parser(text).parse(); }
inline std::string quote(std::string_view text) {
    if (!valid_utf8(text)) throw std::runtime_error("invalid UTF-8");
    std::string out = "\""; constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}
inline std::string dump(const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value.data)) return "null";
    if (value.is_bool()) return value.boolean() ? "true" : "false";
    if (value.is_number()) {
        if (!std::isfinite(value.number())) throw std::runtime_error("non-finite JSON number");
        char buffer[64]; auto result = std::to_chars(buffer, buffer + sizeof(buffer), value.number());
        if (result.ec != std::errc{}) throw std::runtime_error("JSON number conversion failed");
        return {buffer, result.ptr};
    }
    if (value.is_string()) return quote(value.string());
    std::string out = value.is_array() ? "[" : "{"; bool first = true;
    if (value.is_array()) for (const auto& item : value.array()) {
        if (!first) out += ','; first = false; out += dump(item);
    } else for (const auto& [key, item] : value.object()) {
        if (!first) out += ','; first = false; out += quote(key) + ':' + dump(item);
    }
    return out + (value.is_array() ? ']' : '}');
}
} // namespace swml_json
