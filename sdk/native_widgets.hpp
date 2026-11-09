#pragma once
// Native widget composition on the retained C APIs. Call only inside the
// owning UI callback. Every addition lives in the game's normal UI hierarchy.
#include "presentation.h"
#include <cmath>
#include <cstring>
#include <set>

namespace swml4::ui {
struct Port {
    const swml_api_v3* base = nullptr;
    const swml_api_v4* extra = nullptr;
    swml_mod* owner = nullptr;
    Port(const swml_api* api, swml_mod* mod) : base(swml_get_v3(api)), extra(swml_get_v4(api)), owner(mod) {}
    bool available() const { return base && owner; }
    swml_result create(swml_ui_handle parent, const swml_ui_desc& desc, swml_ui_handle& out) const {
        out = 0;
        try { return available() ? base->ui_create(owner, parent, &desc, &out) : SWML_UNAVAILABLE; }
        catch (...) { out = 0; return SWML_UNAVAILABLE; }
    }
    swml_result control(swml_ui_handle parent, const swml_ui_control_desc& desc, swml_ui_handle& out) const {
        out = 0;
        try { return extra && owner ? extra->ui_control_create(owner, parent, &desc, &out) : SWML_UNAVAILABLE; }
        catch (...) { out = 0; return SWML_UNAVAILABLE; }
    }
    swml_result destroy(swml_ui_handle id) const { return available() ? base->ui_destroy(owner, id) : SWML_UNAVAILABLE; }
    swml_result text(swml_ui_handle id, const std::string& value) const { return available() ? base->ui_set_text(owner, id, value.c_str()) : SWML_UNAVAILABLE; }
    swml_result visible(swml_ui_handle id, bool value) const { return available() ? base->ui_set_visible(owner, id, value) : SWML_UNAVAILABLE; }
    swml_result enabled(swml_ui_handle id, bool value) const { return available() ? base->ui_set_enabled(owner, id, value) : SWML_UNAVAILABLE; }
    swml_result context(uint32_t fields, swml_world_context& out) const {
        out = {}; out.size = sizeof(out);
        return extra && owner ? extra->context_get(owner, fields, &out) : SWML_UNAVAILABLE;
    }
    swml_result value(swml_ui_handle id, std::string& json) const {
        if (!extra || !owner) return SWML_UNAVAILABLE;
        uint32_t needed = 0;
        auto result = extra->ui_value_get(owner, id, nullptr, 0, &needed);
        if (result != SWML_BUFFER_SMALL || !needed || needed > 16385) return result == SWML_OK ? SWML_INVALID : result;
        std::vector<char> bytes(needed);
        result = extra->ui_value_get(owner, id, bytes.data(), needed, &needed);
        if (result == SWML_OK) json = bytes.data();
        return result;
    }
};
struct Theme {
    uint32_t foreground = 0xffe6e6e6, background = 0xff27292a, hover = 0xffb49140, pressed = 0xff5678c0;
    float font_size = 0.8f, gap = 0.025f, line_height = 0.15f;
    bool valid() const {
        return std::isfinite(font_size) && font_size > 0 && font_size <= 16 &&
            std::isfinite(gap) && gap >= 0 && gap <= 1 &&
            std::isfinite(line_height) && line_height >= 0.05f && line_height <= 2;
    }
    swml_ui_desc description(uint32_t kind, swml_ui_rect rect, const char* text = "") const {
        return {sizeof(swml_ui_desc), kind, rect, text, foreground, background, hover, pressed,
                font_size, SWML_UI_MIDDLE_LEFT, 1, 1};
    }
};
inline bool rectangle(const swml_ui_rect& r) {
    return r.anchor <= 8 && std::isfinite(r.x) && std::isfinite(r.y) &&
        std::isfinite(r.width) && std::isfinite(r.height) && r.width > 0 && r.height > 0 &&
        std::fabs(r.x) <= 10000 && std::fabs(r.y) <= 10000 && r.width <= 10000 && r.height <= 10000;
}
// Stack-local construction transaction. A failed child rolls back the entire
// owned panel. finish() transfers its native lifetime to the caller/root.
class Builder {
    Port port_;
    Theme theme_;
    swml_ui_handle root_ = 0;
    swml_result result_ = SWML_OK;
    std::set<swml_ui_handle> owned_;
public:
    Builder(Port port, swml_ui_handle parent, swml_ui_rect rect, Theme theme = {}) : port_(port), theme_(theme) {
        if (!theme.valid() || !rectangle(rect)) result_ = SWML_INVALID;
        else { result_ = port_.create(parent, theme_.description(SWML_UI_CONTAINER, rect), root_); if (root_) owned_.insert(root_); }
    }
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;
    ~Builder() { if (root_) try { port_.destroy(root_); } catch (...) {} }
    swml_ui_handle root() const { return root_; }
    const Theme& theme() const { return theme_; }
    const Port& port() const { return port_; }
    swml_result result() const { return result_; }
    swml_result fail(swml_result error) { if (result_ == SWML_OK) result_ = error; return result_; }
    swml_ui_handle add(uint32_t kind, swml_ui_rect rect, const char* text = "", swml_ui_handle parent = 0) {
        if (result_ != SWML_OK) return 0;
        if (parent && !owned_.contains(parent)) { result_ = SWML_DENIED; return 0; }
        swml_ui_handle out = 0;
        result_ = port_.create(parent ? parent : root_, theme_.description(kind, rect, text), out);
        if (out) owned_.insert(out);
        return out;
    }
    swml_ui_handle edit(swml_ui_rect rect, const char* initial = "", uint32_t max_bytes = 128) {
        if (result_ != SWML_OK) return 0;
        auto json = swml_json::quote(initial);
        swml_ui_control_desc desc{}; desc.size = sizeof(desc);
        desc.widget = theme_.description(SWML_UI_EDITBOX, rect); desc.value_json = json.c_str(); desc.max_characters = max_bytes;
        swml_ui_handle out = 0; result_ = port_.control(root_, desc, out); if (out) owned_.insert(out); return out;
    }
    swml_result finish(swml_ui_handle& out) {
        out = 0;
        if (result_ != SWML_OK) return result_;
        out = root_; root_ = 0; return SWML_OK;
    }
};

class CachedWidget {
    swml_ui_handle id_ = 0;
    std::optional<std::string> text_;
    std::optional<bool> visible_, enabled_;
public:
    CachedWidget() = default;
    explicit CachedWidget(swml_ui_handle id) : id_(id) {}
    swml_ui_handle id() const { return id_; }
    void reset(swml_ui_handle id = 0) { id_ = id; text_.reset(); visible_.reset(); enabled_.reset(); }
    swml_result text(Port port, std::string_view value) {
        if (text_ && *text_ == value) return SWML_OK;
        if (value.size() > 16384 || value.find('\0') != value.npos || !swml_json::valid_utf8(value)) return SWML_INVALID;
        std::string copied(value);
        auto result = port.text(id_, copied); if (result == SWML_OK) text_ = std::move(copied); return result;
    }
    swml_result visible(Port port, bool value) {
        if (visible_ && *visible_ == value) return SWML_OK;
        auto result = port.visible(id_, value); if (result == SWML_OK) visible_ = value; return result;
    }
    swml_result enabled(Port port, bool value) {
        if (enabled_ && *enabled_ == value) return SWML_OK;
        auto result = port.enabled(id_, value); if (result == SWML_OK) enabled_ = value; return result;
    }
};
enum class ListAction { none, selected, page_changed, stale };
class List {
    swml_ui_handle root_ = 0;
    std::vector<CachedWidget> rows_;
    std::vector<std::string> bindings_;
    CachedWidget previous_, next_, caption_;
    uint64_t rendered_ = 0;
    uint64_t model_ = 0;
    std::string empty_ = "No matching supplies";
public:
    void forget() { *this = List{}; }
    swml_ui_handle root() const { return root_; }
    // page_size controls native object count, never catalogue length.
    swml_result build(Builder& builder, swml_ui_rect rect, size_t page_size, const char* empty_text = "No matching supplies") {
        if (root_ || !rectangle(rect) || !page_size || page_size > 16 || !empty_text || !presentation::text_valid(empty_text, 256)) return builder.fail(SWML_INVALID);
        const auto& theme = builder.theme();
        float row_height = (rect.height - theme.line_height - theme.gap * float(page_size + 1)) / float(page_size);
        if (row_height < 0.05f || rect.width < 0.6f) return builder.fail(SWML_INVALID);
        root_ = builder.add(SWML_UI_CONTAINER, rect);
        if (!root_) return builder.result();
        empty_ = empty_text;
        for (size_t i = 0; i < page_size; ++i) {
            float y = rect.height - (row_height + theme.gap) * float(i + 1);
            rows_.emplace_back(builder.add(SWML_UI_BUTTON, {0, y, rect.width, row_height, SWML_UI_BOTTOM_LEFT}, "", root_));
        }
        float arrow = std::min(0.4f, rect.width / 4);
        previous_.reset(builder.add(SWML_UI_BUTTON, {0, 0, arrow, theme.line_height, SWML_UI_BOTTOM_LEFT}, "<", root_));
        next_.reset(builder.add(SWML_UI_BUTTON, {rect.width - arrow, 0, arrow, theme.line_height, SWML_UI_BOTTOM_LEFT}, ">", root_));
        caption_.reset(builder.add(SWML_UI_LABEL, {arrow + theme.gap, 0, rect.width - 2 * arrow - 2 * theme.gap, theme.line_height, SWML_UI_BOTTOM_LEFT}, "", root_));
        bindings_.resize(page_size);
        auto result = builder.result(); if (result != SWML_OK) forget(); return result;
    }
    swml_result render(Port port, const presentation::Catalogue& model, const swml_ui_event& event) {
        // Core drains the complete click batch before UPDATE. Rebinding a row
        // halfway through that batch could turn an old click into a new item.
        if (event.kind != SWML_UI_UPDATE && event.kind != SWML_UI_MOUNT) return SWML_WRONG_THREAD;
        if (!root_ || rows_.size() != model.page_size()) return SWML_INVALID;
        if (rendered_ == model.view() && model_ == model.identity()) return SWML_OK;
        // Commit binding revision only after every native write succeeds.
        rendered_ = 0;
        for (size_t i = 0; i < rows_.size(); ++i) {
            auto* row = model.visible(i);
            bool empty = !row && !model.count() && i == 0;
            std::string text = row ? (model.selected() && model.selected()->id == row->id ? "> " : "") + row->display() : empty ? empty_ : "";
            auto result = rows_[i].text(port, text); if (result != SWML_OK) return result;
            result = rows_[i].visible(port, row || empty); if (result != SWML_OK) return result;
            result = rows_[i].enabled(port, row != nullptr); if (result != SWML_OK) return result;
            bindings_[i] = row ? row->id : "";
        }
        auto result = previous_.enabled(port, model.page() != 0); if (result != SWML_OK) return result;
        result = next_.enabled(port, model.page() + 1 < model.pages()); if (result != SWML_OK) return result;
        result = caption_.text(port, std::to_string(model.page() + 1) + " / " + std::to_string(model.pages()));
        if (result == SWML_OK) { rendered_ = model.view(); model_ = model.identity(); }
        return result;
    }
    ListAction click(swml_ui_handle element, presentation::Catalogue& model) {
        bool ours = element == previous_.id() || element == next_.id();
        for (const auto& row : rows_) ours |= element == row.id();
        if (!ours) return ListAction::none;
        if (!rendered_ || rendered_ != model.view() || model_ != model.identity()) return ListAction::stale;
        if (element == previous_.id()) return model.page() && model.page(model.page() - 1) ? ListAction::page_changed : ListAction::none;
        if (element == next_.id()) return model.page(model.page() + 1) ? ListAction::page_changed : ListAction::none;
        for (size_t i = 0; i < rows_.size(); ++i) if (element == rows_[i].id() && !bindings_[i].empty())
            return model.select(bindings_[i]) ? ListAction::selected : ListAction::stale;
        return ListAction::none;
    }
};

class NoticeArea {
    std::array<CachedWidget, 3> lines_;
    std::vector<presentation::Notice> snapshot_;
    presentation::WorldKey world_;
    uint64_t revision_ = 0;
    bool dirty_ = true;
public:
    void forget() { *this = NoticeArea{}; }
    swml_result build(Builder& builder, swml_ui_rect rect) {
        if (lines_[0].id() || !rectangle(rect)) return builder.fail(SWML_INVALID);
        float height = (rect.height - 2 * builder.theme().gap) / 3;
        if (height < 0.05f) return builder.fail(SWML_INVALID);
        for (size_t i = 0; i < lines_.size(); ++i)
            lines_[i].reset(builder.add(SWML_UI_LABEL, {rect.x, rect.y + (height + builder.theme().gap) * float(2 - i), rect.width, height, rect.anchor}));
        return builder.result();
    }
    swml_result render(Port port, presentation::Notices& notices, presentation::WorldKey world, uint64_t now) {
        if (world != world_) { world_ = world; revision_ = 0; snapshot_.clear(); dirty_ = true; }
        auto result = notices.snapshot(world, now, revision_, snapshot_);
        if (result != SWML_OK && result != SWML_PENDING) { snapshot_.clear(); dirty_ = true; }
        if (result == SWML_OK) dirty_ = true;
        if (!dirty_) return SWML_OK;
        for (size_t i = 0; i < lines_.size(); ++i) {
            std::string text;
            if (i < snapshot_.size()) {
                text = snapshot_[i].text;
                if (snapshot_[i].repeats > 1) text += " (x" + std::to_string(snapshot_[i].repeats) + ")";
            }
            auto write = lines_[i].text(port, text); if (write != SWML_OK) return write;
            write = lines_[i].visible(port, !text.empty()); if (write != SWML_OK) return write;
        }
        dirty_ = false; return result == SWML_PENDING ? SWML_OK : result;
    }
};

class Confirmation {
    CachedWidget title_, reason_, confirm_, cancel_;
public:
    void forget() { *this = Confirmation{}; }
    swml_result build(Builder& builder, swml_ui_rect rect, const char* confirm = "Confirm", const char* cancel = "Close") {
        if (title_.id() || !rectangle(rect) || rect.width < 0.8f || rect.height < 0.5f) return builder.fail(SWML_INVALID);
        float line = builder.theme().line_height, gap = builder.theme().gap;
        title_.reset(builder.add(SWML_UI_LABEL, {rect.x, rect.y + rect.height - line, rect.width, line, rect.anchor}));
        reason_.reset(builder.add(SWML_UI_LABEL, {rect.x, rect.y + rect.height - 2 * line - gap, rect.width, line, rect.anchor}));
        confirm_.reset(builder.add(SWML_UI_BUTTON, {rect.x, rect.y, (rect.width - gap) / 2, line, rect.anchor}, confirm));
        cancel_.reset(builder.add(SWML_UI_BUTTON, {rect.x + (rect.width + gap) / 2, rect.y, (rect.width - gap) / 2, line, rect.anchor}, cancel));
        return builder.result();
    }
    swml_result render(Port port, const presentation::ActionGate& gate, std::string_view title, std::string_view localized_reason = "") {
        auto result = title_.text(port, title); if (result != SWML_OK) return result;
        result = reason_.text(port, localized_reason.empty() ? std::string_view(gate.reason()) : localized_reason); if (result != SWML_OK) return result;
        result = confirm_.enabled(port, gate.can_submit()); if (result != SWML_OK) return result;
        return cancel_.enabled(port, !gate.pending());
    }
    bool confirm(swml_ui_handle element) const { return element && element == confirm_.id(); }
    bool cancel(swml_ui_handle element) const { return element && element == cancel_.id(); }
};

// One instance per native root. It only deletes its own adopted panel; original
// game widgets are never modified. Consumers clear their widget caches on changed.
class ModePanel {
    swml_ui_handle panel_ = 0;
    std::string mode_;
    presentation::WorldKey world_;
    uint32_t preview_ = 0, role_ = 0, fields_ = 0;
public:
    struct State { bool active = false, changed = false; swml_world_context context{}; };
    swml_ui_handle panel() const { return panel_; }
    void adopt(swml_ui_handle panel) { panel_ = panel; }
    swml_result sync(Port port, const swml_ui_event& event, const char* wanted_mode, bool allow_preview, State& out) {
        out = {};
        if (!wanted_mode || !presentation::identifier(wanted_mode)) return SWML_INVALID;
        if (event.kind == SWML_UI_UNMOUNT) {
            out.changed = panel_ != 0; panel_ = 0; mode_.clear(); world_ = {}; fields_ = preview_ = role_ = 0; return SWML_OK;
        }
        auto result = port.context(SWML_CONTEXT_MODE, out.context);
        bool active = result == SWML_OK && !strcmp(out.context.mode, wanted_mode) && (allow_preview || !out.context.mode_preview);
        auto world = presentation::WorldKey::from(out.context);
        bool changed = mode_ != out.context.mode || world_ != world || preview_ != out.context.mode_preview ||
            role_ != out.context.role || fields_ != out.context.fields;
        if (panel_ && (!active || changed)) {
            auto removed = port.destroy(panel_);
            if (removed != SWML_OK && removed != SWML_NOT_FOUND) return removed;
            panel_ = 0;
            changed = true;
        }
        mode_ = out.context.mode; world_ = world; preview_ = out.context.mode_preview;
        role_ = out.context.role; fields_ = out.context.fields;
        out.active = active; out.changed = changed;
        return result == SWML_UNAVAILABLE ? SWML_OK : result;
    }
};
} // namespace swml4::ui
