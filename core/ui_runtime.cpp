#include "core.h"
#include "health.h"
#include "native_ui.h"
#include "json.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace sdk_ui {
namespace {
struct Watch { swml_handle id; swml_mod* owner; uintptr_t table; swml_ui_callback callback; void* context; bool active = true; uint32_t running = 0; bool filtered = false; swml_ui_rect filter{}; };
struct Root { uint64_t id, public_handle = 0; void* native; uintptr_t game; std::shared_ptr<Watch> watch; bool live = true; std::set<uint64_t> bindings, controls; };
struct Node {
    uint64_t id; void* native; std::shared_ptr<Root> root; bool owned;
    swml_ui_info original{}, last{};
    bool rect_changed = false, visible_changed = false, enabled_changed = false, text_changed = false;
    std::string original_text, last_text;
    uint32_t control_kind = 0, max_characters = 0;
    double minimum = 0, maximum = 0;
    std::string control_value;
};
struct Created { uint64_t root; void* parent; };
std::recursive_mutex mutex;
std::set<swml_mod*> owners;
std::map<uint64_t, std::shared_ptr<Watch>> watches;
std::map<uint64_t, std::shared_ptr<Root>> roots;
std::map<uint64_t, std::shared_ptr<Node>> nodes;
std::map<std::pair<uint64_t, uintptr_t>, uint64_t> bound;
std::map<void*, Created> created;
std::map<void*, uint64_t> claims;
std::map<void*, std::set<uint64_t>> native_handles;
std::set<uintptr_t> interested_tables;
std::deque<uint64_t> clicks;
uint64_t next_id = 1;
std::atomic<bool> backend_ready{false};
std::atomic<bool> controls_ready{false};
std::atomic<bool> traversal_needed{false};
const NativeUiBackend* test_backend = nullptr;
thread_local std::shared_ptr<Root> current;
thread_local bool tearing_down = false;
const NativeUiBackend& backend() { return test_backend ? *test_backend : native_ui_backend(); }
bool utf8(const char* text) {
    if (!text || strnlen_s(text, 16385) > 16384) return false;
    return !*text || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0) > 0;
}
bool valid_rect(const swml_ui_rect& r) {
    return r.anchor <= 8 && std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
        std::isfinite(r.height) && fabsf(r.x) <= 10000 && fabsf(r.y) <= 10000 &&
        r.width >= 0 && r.width <= 10000 && r.height >= 0 && r.height <= 10000;
}
bool same_rect(const swml_ui_rect& a, const swml_ui_rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height && a.anchor == b.anchor;
}
swml_result access(swml_mod* m) {
    if (!current || current->watch->owner != m || tearing_down) return SWML_WRONG_THREAD;
    if (!owners.contains(m) || !current->watch->active) return SWML_DENIED;
    return SWML_OK;
}
std::shared_ptr<Node> node(uint64_t id) {
    auto it = nodes.find(id);
    return it != nodes.end() && current && it->second->root == current && current->live ? it->second : nullptr;
}
uint64_t bind(const std::shared_ptr<Root>& root, void* native) {
    auto key = std::make_pair(root->id, reinterpret_cast<uintptr_t>(native));
    auto existing = bound.find(key); if (existing != bound.end()) return existing->second;
    if (!native || nodes.size() >= 8192) return 0;
    auto creator = created.find(native);
    if (creator != created.end() && creator->second.root != root->id) return 0;
    auto n = std::make_shared<Node>(); n->id = next_id++; n->native = native; n->root = root;
    n->owned = creator != created.end(); nodes[n->id] = n; bound[key] = n->id;
    native_handles[native].insert(n->id); root->bindings.insert(n->id);
    if (native == root->native) root->public_handle = n->id;
    return n->id;
}
swml_result claim(const std::shared_ptr<Node>& n) {
    if (n->owned) return SWML_OK;
    auto it = claims.find(n->native);
    if (it != claims.end() && it->second != n->root->id) return SWML_CONFLICT;
    if (it == claims.end()) { n->original = n->last = backend().inspect(n->native); claims[n->native] = n->root->id; }
    return SWML_OK;
}
void forget(uint64_t id) {
    auto it = nodes.find(id); if (it == nodes.end()) return;
    auto claim = claims.find(it->second->native);
    if (claim != claims.end() && claim->second == it->second->root->id) claims.erase(claim);
    auto aliases = native_handles.find(it->second->native);
    if (aliases != native_handles.end()) { aliases->second.erase(id); if (aliases->second.empty()) native_handles.erase(aliases); }
    it->second->root->bindings.erase(id);
    it->second->root->controls.erase(id);
    bound.erase({it->second->root->id, reinterpret_cast<uintptr_t>(it->second->native)}); nodes.erase(it);
}
void record_created(void* p, void* parent, uint64_t root, unsigned depth = 0) {
    if (depth > 32 || created.size() >= 4096) return;
    created[p] = {root, parent};
    for (void* child : backend().children(p)) record_created(child, p, root, depth + 1);
}
void restore(const std::shared_ptr<Node>& n) {
    if (n->owned || !claims.contains(n->native) || claims[n->native] != n->root->id) return;
    auto info = backend().inspect(n->native);
    if (n->rect_changed && same_rect(info.rect, n->last.rect)) backend().rect(n->native, n->original.rect, n->root->game);
    if (n->visible_changed && info.visible == n->last.visible) backend().visible(n->native, n->original.visible != 0);
    if (n->enabled_changed && info.enabled == n->last.enabled) backend().enabled(n->native, n->original.enabled != 0, n->root->game);
    std::string text;
    if (n->text_changed && backend().get_text(n->native, text) && text == n->last_text)
        backend().set_text(n->native, n->original_text, n->root->game);
    claims.erase(n->native);
}
void cleanup(const std::shared_ptr<Root>& root) {
    // Only delete top-level SDK objects. Native parent destruction recursively owns the rest.
    std::vector<void*> top;
    for (const auto& [p, c] : created)
        if (c.root == root->id && (!created.contains(c.parent) || created[c.parent].root != root->id)) top.push_back(p);
    for (void* p : top) if (created.contains(p)) backend().erase(p, root->game, created[p].parent);
    std::vector<uint64_t> erase;
    for (const auto& [id, n] : nodes) if (n->root == root) { restore(n); erase.push_back(id); }
    for (uint64_t id : erase) forget(id);
    root->live = false; roots.erase(root->id);
    if(roots.empty()&&watches.empty())traversal_needed.store(false);
}
int invoke_raw(swml_ui_callback callback, const swml_ui_event* event, void* context) {
    __try { callback(event, context); return 1; }
    __except (GetExceptionCode() == 0xe06d7363 ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void call(const std::shared_ptr<Root>& root, uint32_t kind, uint64_t element = 0) {
    uint64_t root_handle = 0;
    {
        std::lock_guard lock(mutex);
        if (!root->watch->active || !owners.contains(root->watch->owner)) return;
        if ((kind == SWML_UI_CLICK || kind == SWML_UI_CHANGE) && (!nodes.contains(element) || nodes[element]->root != root)) return;
        root_handle = kind == SWML_UI_UNMOUNT ? root->public_handle : bind(root, root->native);
        ++root->watch->running;
    }
    auto previous = current; bool previous_teardown = tearing_down;
    current = root; tearing_down = kind == SWML_UI_UNMOUNT;
    swml_ui_event event{sizeof(event), kind, root->watch->id, root_handle, element};
    bool okay = false;
    try { okay = invoke_raw(root->watch->callback, &event, root->watch->context) != 0; } catch (...) {}
    current = previous; tearing_down = previous_teardown;
    {
        std::lock_guard lock(mutex); --root->watch->running;
        if (!okay) root->watch->active = false;
        if (!root->watch->active && !root->watch->running) watches.erase(root->watch->id);
    }
    if (!okay) {
        swml_health::note_fault();
        core_log(root->watch->owner->id.c_str(), "native UI callback failed; watch disabled");
    }
}
}
void set_backend_for_tests(const NativeUiBackend* value) { test_backend = value; backend_ready.store(false); controls_ready.store(false); }
void attach(swml_mod* m) { std::lock_guard lock(mutex); owners.insert(m); }
void drop(swml_mod* m) {
    std::lock_guard lock(mutex); owners.erase(m);
    for (auto it = watches.begin(); it != watches.end();) {
        if (it->second->owner != m) { ++it; continue; }
        it->second->active = false;
        if (!it->second->running) it = watches.erase(it); else ++it;
    }
}
bool enabled() { return backend_ready.load(); }
bool controls_enabled() { return controls_ready.load(); }
swml_result enable(swml_mod* m) {
    { std::lock_guard lock(mutex); if (!owners.contains(m)) return SWML_DENIED; }
    if (!backend_ready.load() && !backend().enable()) return SWML_UNAVAILABLE;
    backend_ready.store(true); return SWML_OK;
}
swml_result watch_rect(swml_mod* m, const char* name, const swml_ui_rect* filter, swml_ui_callback callback, void* context, swml_handle* out) {
    if (out) *out = 0;
    auto checked=filter?*filter:swml_ui_rect{};if(checked.height==-1)checked.height=0;
    if (!out || !name || !*name || strnlen_s(name, 256) >= 256 || !callback || (filter && !valid_rect(checked))) return SWML_INVALID;
    auto result = enable(m); if (result != SWML_OK) return result;
    uintptr_t table = backend().class_table(name); if (!table) return SWML_UNAVAILABLE;
    std::lock_guard lock(mutex);
    if (!owners.contains(m)) return SWML_DENIED;
    if (watches.size() >= 64) return SWML_LIMIT;
    auto w = std::make_shared<Watch>(); w->id = next_id++; w->owner = m; w->table = table; w->callback = callback; w->context = context;
    if (filter) { w->filtered = true; w->filter = *filter; }
    watches[w->id] = w; interested_tables.insert(table); traversal_needed.store(true); *out = w->id; return SWML_OK;
}
swml_result watch(swml_mod* m, const char* name, swml_ui_callback callback, void* context, swml_handle* out) {
    return watch_rect(m, name, nullptr, callback, context, out);
}
swml_result unwatch(swml_mod* m, swml_handle id) {
    std::lock_guard lock(mutex);
    if (!owners.contains(m)) return SWML_DENIED;
    auto it = watches.find(id); if (it == watches.end()) return SWML_NOT_FOUND;
    auto w = it->second; if (w->owner != m) return SWML_DENIED;
    w->active = false;
    if (w->running) return SWML_PENDING;
    watches.erase(it); return SWML_OK;
}
swml_result create(swml_mod* m, swml_ui_handle parent, const swml_ui_desc* d, swml_ui_handle* out) {
    if (out) *out = 0;
    if (!out || !d || d->size < sizeof(*d) || d->kind < 1 || d->kind > 3 || !valid_rect(d->rect) ||
        !std::isfinite(d->font_size) || d->font_size <= 0 || d->font_size > 16 || d->text_align > 8 || (d->text && !utf8(d->text))) return SWML_INVALID;
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto p = node(parent); if (!p) return SWML_NOT_FOUND;
    if (!backend().is_a(p->native, "mm_ui_element_composite") || backend().inspect(p->native).kind == SWML_UI_LABEL) return SWML_INVALID;
    size_t count = 0; for (const auto& [id, n] : nodes) if (n->root->watch->owner == m && n->owned) ++count;
    if (count >= 1024 || nodes.size() >= 8190 || created.size() >= 4090) return SWML_LIMIT;
    void* native = backend().create(*d); if (!native) return SWML_UNAVAILABLE;
    // Track before native initialization: an exception during attachment must still leave
    // enough ownership information for safe removal from the parent's native ring.
    created[native] = {current->id, p->native};
    try { backend().attach(p->native, current->game, native, *d); }
    catch (...) { backend().erase(native,current->game,p->native);throw; }
    backend().visible(native, d->visible != 0); backend().enabled(native, d->enabled != 0, current->game);
    record_created(native, p->native, current->id);
    *out = bind(current, native); return *out ? SWML_OK : SWML_LIMIT;
}
swml_result create_control(swml_mod* m,swml_ui_handle parent,const swml_ui_control_desc* d,swml_ui_handle* out) {
    if(out)*out=0;
    if(!out||!d||d->size<sizeof(*d)||d->widget.size<sizeof(swml_ui_desc)||d->widget.kind<SWML_UI_CHECKBOX||d->widget.kind>SWML_UI_EDITBOX||
        !valid_rect(d->widget.rect)||d->widget.rect.width<=0||d->widget.rect.height<=0||!d->widget.enabled||!utf8(d->value_json)||
        d->max_characters>4096||(d->allowed_characters&&!utf8(d->allowed_characters)))return SWML_INVALID;
    try {
        auto value=swml_json::parse(d->value_json);auto kind=d->widget.kind;const auto& r=d->widget.rect;
        if(kind==SWML_UI_CHECKBOX&&(!value.is_bool()||r.width!=r.height))return SWML_INVALID;
        if(kind==SWML_UI_SLIDER&&(!value.is_number()||!std::isfinite(d->minimum)||!std::isfinite(d->maximum)||!std::isfinite(d->step)||
            d->step<0||fabs(d->minimum)>1000000||fabs(d->maximum)>1000000||float(d->minimum)>=float(d->maximum)||d->step>d->maximum-d->minimum||
            value.number()<d->minimum||value.number()>d->maximum||r.width<=r.height))return SWML_INVALID;
        uint32_t max_chars=d->max_characters?d->max_characters:255;
        if(kind==SWML_UI_EDITBOX&&(!value.is_string()||value.string().size()>max_chars||value.string().find_first_of("\r\n\0",0,3)!=std::string::npos))return SWML_INVALID;
        std::lock_guard lock(mutex);auto result=access(m);if(result!=SWML_OK)return result;
        auto p=node(parent);if(!p)return SWML_NOT_FOUND;
        if(!backend().is_a(p->native,"mm_ui_element_composite")||backend().inspect(p->native).kind!=SWML_UI_CONTAINER)return SWML_INVALID;
        if(!backend().enable_control||!backend().create_control||!backend().control_value||!backend().control_set||!backend().enable_control(kind))return SWML_UNAVAILABLE;
        if(nodes.size()>=8190||created.size()>=4080||current->controls.size()>=256)return SWML_LIMIT;
        auto* native=backend().create_control(*d);if(!native)return SWML_UNAVAILABLE;
        created[native]={current->id,p->native};
        try {
            backend().attach(p->native,current->game,native,d->widget);
            if(backend().control_configure)backend().control_configure(native,*d,current->game);
            auto raw=value.is_string()?value.string():swml_json::dump(value);
            backend().control_set(native,raw,current->game);backend().visible(native,d->widget.visible!=0);
        }catch(...){backend().erase(native,current->game,p->native);throw;}
        record_created(native,p->native,current->id);
        auto handle=bind(current,native);
        if(!handle){backend().erase(native,current->game,p->native);return SWML_LIMIT;}
        *out=handle;
        auto n=node(*out);n->control_kind=kind;n->minimum=d->minimum;n->maximum=d->maximum;n->max_characters=max_chars;
        if(backend().control_value(native,n->control_value)<0){backend().erase(native,current->game,p->native);forget(handle);*out=0;return SWML_UNAVAILABLE;}
        current->controls.insert(*out);controls_ready.store(true);return SWML_OK;
    }catch(const std::exception&){return SWML_INVALID;}catch(...){return SWML_UNAVAILABLE;}
}
swml_result value_get(swml_mod* m,swml_ui_handle id,char* out,uint32_t capacity,uint32_t* needed) {
    if(needed)*needed=0;if(!needed||(!out&&capacity))return SWML_INVALID;
    std::lock_guard lock(mutex);auto result=access(m);if(result!=SWML_OK)return result;
    auto n=node(id);if(!n)return SWML_NOT_FOUND;if(!n->control_kind||!backend().control_value)return SWML_INVALID;
    std::string raw;if(backend().control_value(n->native,raw)<0)return SWML_UNAVAILABLE;
    auto json=n->control_kind==SWML_UI_EDITBOX?swml_json::quote(raw):raw;*needed=uint32_t(json.size()+1);
    if(capacity<*needed)return SWML_BUFFER_SMALL;memcpy(out,json.c_str(),*needed);return SWML_OK;
}
swml_result value_set(swml_mod* m,swml_ui_handle id,const char* json) {
    if(!utf8(json))return SWML_INVALID;
    try {
        auto value=swml_json::parse(json);std::lock_guard lock(mutex);auto result=access(m);if(result!=SWML_OK)return result;
        auto n=node(id);if(!n)return SWML_NOT_FOUND;if(!n->owned||!n->control_kind)return SWML_DENIED;
        if(n->control_kind==SWML_UI_CHECKBOX&&!value.is_bool())return SWML_INVALID;
        if(n->control_kind==SWML_UI_SLIDER&&(!value.is_number()||value.number()<n->minimum||value.number()>n->maximum))return SWML_INVALID;
        if(n->control_kind==SWML_UI_EDITBOX&&(!value.is_string()||value.string().size()>n->max_characters||value.string().find_first_of("\r\n\0",0,3)!=std::string::npos))return SWML_INVALID;
        backend().control_set(n->native,value.is_string()?value.string():swml_json::dump(value),current->game);
        backend().control_value(n->native,n->control_value);return SWML_OK;
    }catch(const std::exception&){return SWML_INVALID;}
}
swml_result destroy(swml_mod* m, swml_ui_handle id) {
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    if (!n->owned) return SWML_DENIED;
    backend().erase(n->native, current->game, created[n->native].parent); return SWML_OK;
}
swml_result children(swml_mod* m, swml_ui_handle id, swml_ui_handle* out, uint32_t capacity, uint32_t* needed) {
    if (needed) *needed = 0;
    if (!needed || (!out && capacity)) return SWML_INVALID;
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    auto list = backend().children(n->native); *needed = static_cast<uint32_t>(list.size());
    if (capacity < *needed) return SWML_BUFFER_SMALL;
    for (uint32_t i = 0; i < *needed; ++i) { out[i] = bind(current, list[i]); if (!out[i]) return SWML_LIMIT; }
    return SWML_OK;
}
swml_result find_class(swml_mod* m, swml_ui_handle id, const char* name, uint32_t occurrence, swml_ui_handle* out) {
    if (out) *out = 0;
    if (!out || !name || !*name || strnlen_s(name, 256) >= 256) return SWML_INVALID;
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    std::deque<void*> queue{n->native}; unsigned visited = 0;
    while (!queue.empty() && ++visited <= 4096) {
        void* p = queue.front(); queue.pop_front();
        if (backend().is_a(p, name) && occurrence-- == 0) {
            *out = bind(current, p); return *out ? SWML_OK : SWML_LIMIT;
        }
        for (void* child : backend().children(p)) if (queue.size() < 4096) queue.push_back(child);
    }
    return SWML_NOT_FOUND;
}
swml_result inspect(swml_mod* m, swml_ui_handle id, swml_ui_info* out) {
    if (!out || out->size < sizeof(*out)) return SWML_INVALID;
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    *out = backend().inspect(n->native); out->owned = n->owned; return SWML_OK;
}
swml_result set_rect(swml_mod* m, swml_ui_handle id, const swml_ui_rect* rect) {
    if (!rect || !valid_rect(*rect)) return SWML_INVALID;
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    if (n->control_kind) { auto existing=backend().inspect(n->native).rect;if(existing.width!=rect->width||existing.height!=rect->height)return SWML_UNAVAILABLE; }
    result = claim(n); if (result != SWML_OK) return result;
    n->rect_changed = true; backend().rect(n->native, *rect, current->game);
    n->last.rect = backend().inspect(n->native).rect; return SWML_OK;
}
swml_result set_visible(swml_mod* m, swml_ui_handle id, int value) {
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    result = claim(n); if (result != SWML_OK) return result;
    n->last.visible = value != 0; n->visible_changed = true; backend().visible(n->native, value != 0); return SWML_OK;
}
swml_result set_enabled(swml_mod* m, swml_ui_handle id, int value) {
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    if (backend().inspect(n->native).kind != SWML_UI_BUTTON) return SWML_INVALID;
    result = claim(n); if (result != SWML_OK) return result;
    n->last.enabled = value != 0; n->enabled_changed = true; backend().enabled(n->native, value != 0, current->game); return SWML_OK;
}
swml_result set_text(swml_mod* m, swml_ui_handle id, const char* value) {
    if (!utf8(value)) return SWML_INVALID;
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    std::string previous; if (!backend().get_text(n->native, previous)) return SWML_INVALID;
    result = claim(n); if (result != SWML_OK) return result;
    if (!n->text_changed) n->original_text = previous;
    n->text_changed = true; n->last_text = value; backend().set_text(n->native, value, current->game); return SWML_OK;
}
swml_result get_text(swml_mod* m, swml_ui_handle id, char* out, uint32_t capacity, uint32_t* needed) {
    if (needed) *needed = 0;
    if (!needed || (!out && capacity)) return SWML_INVALID;
    std::lock_guard lock(mutex); auto result = access(m); if (result != SWML_OK) return result;
    auto n = node(id); if (!n) return SWML_NOT_FOUND;
    std::string value; if (!backend().get_text(n->native, value)) return SWML_INVALID;
    *needed = static_cast<uint32_t>(value.size() + 1);
    if (capacity < *needed) return SWML_BUFFER_SMALL;
    memcpy(out, value.c_str(), *needed); return SWML_OK;
}
void update(void* native, uintptr_t game) {
    if (!backend_ready.load() || !traversal_needed.load()) return;
    std::vector<std::pair<std::shared_ptr<Root>, bool>> callbacks;
    std::vector<std::shared_ptr<Root>> clean;
    {
        std::lock_guard lock(mutex);
        if (!interested_tables.contains(backend().table(native))) return;
        for (const auto& [id, root] : roots) if (root->native == native) {
            root->game = game;
            if (!root->watch->active || !owners.contains(root->watch->owner)) clean.push_back(root);
            else callbacks.push_back({root, false});
        }
        for (const auto& root : clean) cleanup(root);
        if (!created.contains(native)) for (const auto& [id, w] : watches) if (w->active && w->table == backend().table(native)) {
            bool found = false; for (const auto& pair : callbacks) if (pair.first->watch == w) found = true;
            if (!found) {
                if (w->filtered) {auto rect=backend().inspect(native).rect;auto filter=w->filter;if(filter.height==-1)filter.height=rect.height;if(!same_rect(rect,filter))continue;}
                if (roots.size() >= 256) break;
                auto root = std::make_shared<Root>(); root->id = next_id++; root->native = native; root->game = game; root->watch = w;
                if (!bind(root, native)) break;
                roots[root->id] = root; callbacks.push_back({root, true});
            }
        }
    }
    for (const auto& [root, mount] : callbacks) {
        std::vector<uint64_t> actions;
        std::vector<uint64_t> changes;
        { std::lock_guard lock(mutex);
          for(auto id:root->controls){auto n=nodes.find(id);if(n!=nodes.end()&&backend().control_value&&backend().control_value(n->second->native,n->second->control_value)>0)changes.push_back(id);}
          for (auto it = clicks.begin(); it != clicks.end();) {
              auto n = nodes.find(*it);
              if (n == nodes.end()) it = clicks.erase(it);
              else if (n->second->root == root) { actions.push_back(*it); it = clicks.erase(it); } else ++it;
          }
        }
        for (auto id : actions) call(root, SWML_UI_CLICK, id);
        for (auto id : changes) call(root, SWML_UI_CHANGE, id);
        call(root, mount ? SWML_UI_MOUNT : SWML_UI_UPDATE);
    }
}
void pressed(void* native) {
    std::lock_guard lock(mutex);
    if (!created.contains(native) || clicks.size() >= 256) return;
    for (const auto& [id, n] : nodes) if (n->native == native && n->owned) { clicks.push_back(id); break; }
}
void deleting(void* native) {
    std::vector<std::shared_ptr<Root>> notify;
    {
        std::lock_guard lock(mutex);
        auto aliases = native_handles.find(native);
        if (aliases == native_handles.end()) { created.erase(native); claims.erase(native); return; }
        for (const auto& [id, root] : roots) if (root->native == native) { root->live = false; notify.push_back(root); }
        std::set<uint64_t> erase = aliases->second;
        for (const auto& root : notify) erase.insert(root->bindings.begin(), root->bindings.end());
        for (auto id : erase) forget(id);
        for (const auto& root : notify) roots.erase(root->id);
        for (auto it = created.begin(); it != created.end();) {
            bool dead = false; for (const auto& root : notify) if (it->second.root == root->id) dead = true;
            if (dead) it = created.erase(it); else ++it;
        }
        created.erase(native); claims.erase(native);
    }
    for (const auto& root : notify) call(root, SWML_UI_UNMOUNT);
}
}
