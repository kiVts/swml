#pragma once
// How a mod's settings.default.ini is laid out in the settings panel:
// - the comment lines above a key are its description (the tooltip);
// - a ";@ Label" line above a key or a [section] gives it a display name (the key / "[section]" otherwise);
// - settings are listed in the file's own order.
// Older launchers read ";@ Label" lines as the start of the description, so a labelled file still works there.
#include <cwctype>
#include <map>
#include <string>
#include <utility>

namespace swml_settings_layout {
using Key = std::pair<std::wstring, std::wstring>; // lower-case section, lower-case key

struct Layout {
    std::map<Key, std::wstring> descriptions, labels;
    std::map<std::wstring, std::wstring> section_labels;
    std::map<Key, size_t> order;                // position of the key in the file
    std::map<std::wstring, size_t> section_order; // position of the section's first appearance
};

inline std::wstring lower(std::wstring s)
{
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

inline std::wstring trim(const std::wstring& s)
{
    const auto b = s.find_first_not_of(L" \t\r\n");
    if (b == s.npos) return {};
    const auto e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline Layout parse(const std::wstring& source)
{
    Layout out;
    std::wstring section, description, label;
    size_t start = 0, position = 0;
    while (start <= source.size()) {
        auto end = source.find(L'\n', start);
        if (end == source.npos) end = source.size();
        const auto line = trim(source.substr(start, end - start));
        start = end + 1;
        if (line.starts_with(L";@") || line.starts_with(L"#@")) {
            label = trim(line.substr(2));
        } else if (line.starts_with(L";") || line.starts_with(L"#")) {
            if (!description.empty()) description += L" ";
            description += trim(line.substr(1));
        } else if (line.starts_with(L"[")) {
            const auto close = line.find(L']');
            section = lower(trim(line.substr(1, close == line.npos ? line.npos : close - 1)));
            if (!out.section_order.contains(section)) out.section_order[section] = position++;
            if (!label.empty()) out.section_labels[section] = label;
            description.clear();
            label.clear();
        } else if (const auto equal = line.find(L'='); equal != line.npos) {
            const Key id{section, lower(trim(line.substr(0, equal)))};
            out.descriptions[id] = description;
            if (!label.empty()) out.labels[id] = label;
            if (!out.order.contains(id)) out.order[id] = position++;
            description.clear();
            label.clear();
        }
        if (end == source.size()) break;
    }
    return out;
}

// Sort key for a setting: settings named in the file first, in file order (by section, then key); others after
inline std::pair<size_t, size_t> rank(const Layout& layout, const Key& id)
{
    constexpr size_t kLast = size_t(-1);
    const auto s = layout.section_order.find(id.first);
    const auto k = layout.order.find(id);
    return {s == layout.section_order.end() ? kLast : s->second, k == layout.order.end() ? kLast : k->second};
}
} // namespace swml_settings_layout
