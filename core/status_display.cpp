#include "status_display.h"
#include "core.h"
#include "health.h"
#include "native_ui.h"
#include "swml_v2.h"
#include <map>
#include <string>
#include <string_view>

namespace {
struct Label { std::string base, last; };
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
    const auto wanted = label.base + " | SWML V" SWML_SDK_VERSION " " +
        std::to_string(status.loaded_mods) + " mods loaded" + (status.fault_found ? ", fault found" : "");
    if (wanted == text) return;
    if (sdk_ui::set_text(&owner, event->root, wanted.c_str()) == SWML_OK) label.last = wanted;
    else swml_health::note_fault();
}
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
