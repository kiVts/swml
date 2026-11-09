#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swml_version {
struct Version { uint32_t major{}, minor{}, patch{}; std::vector<std::string> prerelease; };
inline bool identifier(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}
inline std::optional<Version> parse(std::string_view text) {
    if (text.empty() || text.size() > 128) return {};
    size_t plus = text.find('+');
    if (plus != text.npos) {
        auto build = text.substr(plus + 1); size_t at = 0;
        do { size_t end = build.find('.', at); if (!identifier(build.substr(at, end - at))) return {};
             if (end == build.npos) break; at = end + 1; } while (true);
        text = text.substr(0, plus);
    }
    size_t dash = text.find('-'); auto numbers = text.substr(0, dash);
    Version result; uint32_t* fields[]{&result.major, &result.minor, &result.patch}; size_t at = 0;
    for (unsigned i = 0; i < 3; ++i) {
        size_t end = numbers.find('.', at); auto part = numbers.substr(at, end - at);
        if (part.empty() || (part.size() > 1 && part[0] == '0')) return {};
        auto r = std::from_chars(part.data(), part.data() + part.size(), *fields[i]);
        if (r.ec != std::errc{} || r.ptr != part.data() + part.size()) return {};
        if ((i < 2 && end == numbers.npos) || (i == 2 && end != numbers.npos)) return {};
        at = end + 1;
    }
    if (dash != text.npos) {
        auto tail = text.substr(dash + 1); at = 0;
        do {
            size_t end = tail.find('.', at); auto part = tail.substr(at, end - at);
            if (!identifier(part)) return {};
            bool numeric = std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (numeric && part.size() > 1 && part[0] == '0') return {};
            result.prerelease.emplace_back(part);
            if (end == tail.npos) break; at = end + 1;
        } while (true);
    }
    return result;
}
inline int compare(const Version& a, const Version& b) {
    for (auto [x,y] : {std::pair{a.major,b.major}, std::pair{a.minor,b.minor}, std::pair{a.patch,b.patch}})
        if (x != y) return x < y ? -1 : 1;
    if (a.prerelease.empty() != b.prerelease.empty()) return a.prerelease.empty() ? 1 : -1;
    for (size_t i = 0; i < std::min(a.prerelease.size(), b.prerelease.size()); ++i) {
        const auto& x = a.prerelease[i]; const auto& y = b.prerelease[i]; if (x == y) continue;
        auto numeric = [](const auto& s) { return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
        bool nx = numeric(x), ny = numeric(y);
        if (nx != ny) return nx ? -1 : 1;
        if (nx && x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
        return x < y ? -1 : 1;
    }
    return a.prerelease.size() == b.prerelease.size() ? 0 : a.prerelease.size() < b.prerelease.size() ? -1 : 1;
}
// Comparators joined by '&' or spaces are ANDed. '^' and '~' follow semver
// upper bounds; prereleases match only ranges explicitly mentioning one.
inline bool satisfies(std::string_view version, std::string_view range) {
    if (range.empty() || range == "*") return true;
    auto v = parse(version); if (!v) return false;
    bool mentions_prerelease = false, any = false; size_t at = 0;
    while (at < range.size()) {
        while (at < range.size() && (range[at] == ' ' || range[at] == '&')) ++at;
        if (at == range.size()) break;
        size_t end = range.find_first_of(" &", at); if (end == range.npos) end = range.size();
        auto term = range.substr(at, end - at); at = end;
        size_t n = 0; while (n < term.size() && (term[n] == '<' || term[n] == '>' || term[n] == '=' || term[n] == '^' || term[n] == '~')) ++n;
        auto op = term.substr(0, n); auto target = parse(term.substr(n)); if (!target) return false;
        any = true;
        mentions_prerelease |= !target->prerelease.empty()&&v->major==target->major&&v->minor==target->minor&&v->patch==target->patch;
        int c = compare(*v, *target); bool match = false;
        if (op.empty() || op == "=") match = c == 0;
        else if (op == ">") match = c > 0; else if (op == ">=") match = c >= 0;
        else if (op == "<") match = c < 0; else if (op == "<=") match = c <= 0;
        else if (op == "^" || op == "~") {
            Version upper = *target; upper.prerelease.clear(); uint32_t* increment = nullptr;
            if (op == "~") { increment = &upper.minor; upper.patch = 0; }
            else if (upper.major) { increment = &upper.major; upper.minor = upper.patch = 0; }
            else if (upper.minor) { increment = &upper.minor; upper.patch = 0; }
            else increment = &upper.patch;
            if (*increment == std::numeric_limits<uint32_t>::max()) return false;
            ++*increment; match = c >= 0 && compare(*v, upper) < 0;
        }
        if (!match) return false;
    }
    return any && (v->prerelease.empty() || mentions_prerelease);
}
inline std::pair<std::string,std::string> requirement(const std::string& text) {
    auto at = text.find('@'); return {text.substr(0, at), at == text.npos ? "" : text.substr(at + 1)};
}
} // namespace swml_version
