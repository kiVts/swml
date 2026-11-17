#include "status_display.h"
#include "core.h"
#include "health.h"
#include "native_ui.h"
#include "swml_v2.h"
#include "../launcher/loader_version.h"
#include "multiplayer_gate.h"
#include "status_line.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
// Mods' segments (status_line.h), in the order they were first set.
struct Segment { const swml_mod* mod; std::string text; uint32_t color; };
std::mutex segments_mutex;
std::vector<Segment> segments;

// The mods' part of the line, plain and with colour markup: "<n>" switches the label to markup style n, whose colour
// is styles[n - 1] (up to three distinct colours; a fourth keeps the label's own). "<" in text is written "<<".
struct Segments { std::string plain, styled; std::vector<uint32_t> styles; };
Segments segments_text() {
    std::lock_guard lock(segments_mutex);
    Segments out;
    for (const auto& s : segments) {
        std::string escaped;
        for (char c : s.text) { if (c == '<') escaped += '<'; escaped += c; }
        out.plain += " | " + escaped;
        auto style = std::find(out.styles.begin(), out.styles.end(), s.color);
        if (s.color && style == out.styles.end() && out.styles.size() < 3) style = out.styles.insert(out.styles.end(), s.color);
        if (!s.color || style == out.styles.end()) { out.styled += " | " + escaped; continue; }
        out.styled += " | <" + std::to_string(style - out.styles.begin() + 1) + ">" + escaped + "<0>";
    }
    return out;
}

struct Label { std::string base, last; std::vector<uint32_t> styles; bool styled = true; };
std::map<swml_ui_handle, Label> labels;
swml_mod owner = [] { swml_mod value; value.id = "swml.status"; return value; }();

bool native_version(std::string_view text) {
    if (!text.starts_with('v') || !text.ends_with(" 64Bit")) return false;
    text.remove_prefix(1); text.remove_suffix(6);
    int dots = 0;
    bool digit = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && digit) { ++dots; digit = false; }
        else return false;
    }
    return digit && dots == 2;
}

void screen(const swml_ui_event* event, void*) {
    if (event->kind == SWML_UI_UNMOUNT) { labels.erase(event->root); return; }
    if (event->kind != SWML_UI_MOUNT && event->kind != SWML_UI_UPDATE) return;
    swml_runtime_status status{sizeof(status)};
    if (!swml_health::snapshot(&status) || !status.ready) return;

    char text[384]{};
    uint32_t needed = 0;
    if (sdk_ui::get_text(&owner, event->root, text, sizeof(text), &needed) != SWML_OK) {
        labels.erase(event->root);
        return;
    }
    auto found = labels.find(event->root);
    if (found == labels.end()) {
        swml_ui_info info{sizeof(info)};
        if (sdk_ui::inspect(&owner, event->root, &info) != SWML_OK || info.kind != SWML_UI_LABEL ||
            info.rect.anchor != SWML_UI_BOTTOM_LEFT || !native_version(text)) return;
        found = labels.emplace(event->root, Label{text, {}}).first;
    }
    auto& label = found->second;
    if (label.base != text && label.last != text) {
        if (!native_version(text)) return; // Preserve unrelated text/another owner's edit.
        label.base = text;
    }
    auto wanted = label.base + " | SWML V" SWML_LOADER_VERSION " " +
        std::to_string(status.loaded_mods) + " mods loaded" + (status.fault_found ? ", fault found" : "");
    if (const auto* message = core_multiplayer_notice(); *message) wanted += std::string(" | ") + message;
    const auto mods = segments_text();
    if (label.styled && !mods.styles.empty()) {
        const auto styled = wanted + mods.styled;
        if (styled == text && label.styles == mods.styles) return;
        const auto result = sdk_ui::set_text_styled(&owner, event->root, styled.c_str(), mods.styles.data(), int(mods.styles.size()));
        if (result == SWML_OK) { label.last = styled; label.styles = mods.styles; return; }
        if (result != SWML_UNAVAILABLE) { swml_health::note_fault(); return; }
        label.styled = false; // this label's styles aren't the layout we know: no colours, plain text from now on
        corelog("status line: label colours unavailable; mods' text is shown in the label's colour");
    }
    wanted += mods.plain;
    if (wanted == text) return;
    if (sdk_ui::set_text(&owner, event->root, wanted.c_str()) == SWML_OK) label.last = wanted;
    else swml_health::note_fault();
}
}

void core_status_line_set(const swml_mod* mod, const char* text, uint32_t color) {
    std::string value = text ? text : "";
    if (value.size() > SWML_STATUS_LINE_MAX) {
        size_t cut = SWML_STATUS_LINE_MAX;
        while (cut && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) --cut; // whole UTF-8 characters
        value.resize(cut);
    }
    for (char& c : value) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    std::lock_guard lock(segments_mutex);
    auto found = std::find_if(segments.begin(), segments.end(), [&](const auto& s) { return s.mod == mod; });
    if (value.empty()) { if (found != segments.end()) segments.erase(found); }
    else if (found != segments.end()) { found->text = std::move(value); found->color = color; }
    else segments.push_back({mod, std::move(value), color});
}

bool core_status_install() {
    sdk_ui::attach(&owner);
    const swml_ui_rect filter{.03f, .01f, 2, -1, SWML_UI_BOTTOM_LEFT};
    swml_handle watch = 0;
    const auto result = sdk_ui::watch_rect(&owner, "mm_ui_element_text", &filter, screen, nullptr, &watch);
    if (result != SWML_OK) {
        sdk_ui::drop(&owner);
        corelog("built-in status display unavailable (%d); native version left unchanged", result);
        return false;
    }
    corelog("built-in SWML version and mod status display enabled");
    return true;
}

