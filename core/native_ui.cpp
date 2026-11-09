#include "core.h"
#include "core_v3.h"
#include "native_ui.h"
#include "native_ui_signatures.h"
#include "native_ui_controls.h"
#include "json.h"
#include "input_runtime.h"
#include "native_ui_update.h"
#include "native_text.h"
#include "native_button.h"
#include "resolve.h"
#include <array>
#include <atomic>
#include <cstring>
#include <charconv>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>

namespace {
template<class T> T& field(void* p, size_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + offset); }
struct String { char* data; uint32_t length, capacity; };
struct Ring { void** data; uint32_t capacity, head, count; };
using Alloc = void*(*)(size_t);
using Free = void(*)(void*, size_t);
using StringFree = void(*)(void*);
using Reserve = void(*)(String*, uint32_t);
using Container = void*(*)(void*, float, float, float, float, uint32_t, uint32_t);
using Text = void*(*)(void*, float, float, float, float, uint32_t, String*, uint32_t, uint32_t, float, float, uint8_t);
using Button = void*(*)(void*, float, float, float, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t);
using Attach = void*(*)(void*, uintptr_t, void*);
using SetText = void(*)(void*, uintptr_t, const String*);
using LayoutText = void(*)(void*, uintptr_t);
using ResizeButton = void(*)(void*, uintptr_t, float, float);
using Update = void(*)(void*, uintptr_t, uintptr_t);
using Destroy = void(*)(void*);
using Press = void(*)(void*, uintptr_t, uintptr_t, uintptr_t);
Alloc allocate; Free release; Reserve reserve_string;
StringFree release_string;
struct HeapBlock {
    void* pointer; size_t size;
    explicit HeapBlock(size_t bytes) : pointer(allocate(bytes)), size(bytes) {
        if (!pointer) throw std::bad_alloc();
        // Native initialization may construct child objects after the constructor.
        // Zero unused pointers so interrupted initialization remains destructible.
        memset(pointer,0,size);
    }
    ~HeapBlock() { if (pointer) release(pointer, size); }
    void* take() { void* p = pointer; pointer = nullptr; return p; }
};
struct TemporaryString {
    String value{};
    ~TemporaryString() { if (value.data && value.capacity) release_string(value.data); }
};
Container container_ctor; Text text_ctor; Button button_ctor; Attach attach_child;
SetText set_text_native; LayoutText layout_text;
ResizeButton resize_button;
Update original_update; Destroy original_destroy; Press original_press;
uintptr_t container_table = 0;
std::mutex init_mutex;
std::atomic<bool> ready{false};
bool attempted = false;
using Checkbox = void*(*)(void*,float,float,float,uint32_t);
using Slider = void*(*)(void*,float,float,float,float,uint32_t,float,float,float,String*);
using Edit = void*(*)(void*,float,float,float,float,uint32_t,String*,uint32_t);
using SliderGet = float(*)(void*);
using SliderSet = void(*)(void*,uintptr_t,float);
using EditSet = void(*)(void*,uintptr_t,String*);
using EditFilter = void(*)(void*,String*);
Checkbox checkbox_ctor; Slider slider_ctor; Edit edit_ctor; SliderGet slider_get; SliderSet slider_set;
EditSet edit_set; EditFilter edit_filter;
std::map<uint32_t,uintptr_t> control_tables;
std::set<uint32_t> attempted_controls;

bool matches(uintptr_t address, const char* pattern) {
    const auto* data = reinterpret_cast<const uint8_t*>(address);
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { p += 2; ++data; continue; }
        auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
        if (*data++ != ((hex(p[0]) << 4) | hex(p[1]))) return false;
        p += 2;
    }
    return true;
}
uintptr_t relative_call(uintptr_t at) {
    if (*reinterpret_cast<const uint8_t*>(at) != 0xe8) return 0;
    int32_t delta = 0; memcpy(&delta, reinterpret_cast<const void*>(at + 1), 4);
    return at + 5 + delta;
}
void update_hook(void* object, uintptr_t traversal, uintptr_t application) {
    native_ui_update(original_update, object, traversal, application, sdk_input::native_observe, sdk_ui::update);
}
void destroy_hook(void* object) {
    sdk_input::native_forget(object);
    sdk_ui::deleting(object);
    original_destroy(object);
}
void press_hook(void* object, uintptr_t a, uintptr_t b, uintptr_t c) {
    // Native method 18 activates only enabled, hovered buttons. Defer application actions
    // until the next UI update so a callback cannot delete this button during its input method.
    bool clicked = field<uint8_t>(object, 0x90) != 0 && field<uint8_t>(object, 0x1d) != 0;
    original_press(object, a, b, c);
    if (clicked) sdk_ui::pressed(object);
}
bool enable() {
    std::lock_guard lock(init_mutex);
    if (ready.load()) return true;
    if (attempted) return false; // never chain a partially installed detour to itself on retry
    attempted = true;
    std::map<std::string, uintptr_t> functions;
    uintptr_t text_table = native_find_vtable("c_ui_element_text");
    container_table = native_find_vtable("c_ui_element_composite_ext");
    if (!text_table || !container_table) return false;
    for (const auto& sig : kNativeUiSignatures) {
        uintptr_t address = !strcmp(sig.name, "text_delete") ? *reinterpret_cast<const uintptr_t*>(text_table)
                                                           : native_find_pattern(sig.pattern);
        if (!address || !matches(address, sig.pattern)) {
            corelog("native UI unavailable: binding %s changed or is ambiguous", sig.name); return false;
        }
        functions[sig.name] = address;
    }
    // Method 35 must still resize the exact background classes constructed by
    // the native button initializer, not merely exist somewhere in the image.
    for (const auto& entry : {std::pair{"c_ui_element_rectangle_button", "button_resize"},
                             std::pair{"c_ui_element_rounded_rectangle", "rounded_resize"},
                             std::pair{"c_ui_element_rectangle", "rectangle_resize"}}) {
        const auto vtable = native_find_vtable(entry.first);
        if (!vtable || reinterpret_cast<const uintptr_t*>(vtable)[34] != functions[entry.second]) {
            corelog("native UI unavailable: %s resize slot changed", entry.first); return false;
        }
    }
    allocate = reinterpret_cast<Alloc>(relative_call(functions["button_init"] + kUiAllocateCallOffset));
    release = reinterpret_cast<Free>(relative_call(functions["text_delete"] + kUiDeleteCallOffset));
    release_string = reinterpret_cast<StringFree>(relative_call(functions["text_ctor"] + kUiStringReleaseCallOffset));
    if (!allocate || !release || !release_string) return false;
    reserve_string = reinterpret_cast<Reserve>(functions["reserve_string"]);
    container_ctor = reinterpret_cast<Container>(functions["container_ctor"]);
    text_ctor = reinterpret_cast<Text>(functions["text_ctor"]);
    button_ctor = reinterpret_cast<Button>(functions["button_ctor"]);
    attach_child = reinterpret_cast<Attach>(functions["attach"]);
    set_text_native = reinterpret_cast<SetText>(functions["set_text"]);
    layout_text = reinterpret_cast<LayoutText>(functions["layout_text"]);
    resize_button = reinterpret_cast<ResizeButton>(functions["button_resize"]);
    // Install only after every binding/layout contract passed. Shared hook chains preserve mods.
    bool okay = core_internal_hook(reinterpret_cast<void*>(functions["update"]), reinterpret_cast<void*>(update_hook),
                                    reinterpret_cast<void**>(&original_update), "SDK native UI update") &&
                core_internal_hook(reinterpret_cast<void*>(functions["destroy"]), reinterpret_cast<void*>(destroy_hook),
                                    reinterpret_cast<void**>(&original_destroy), "SDK native UI destruction") &&
                core_internal_hook(reinterpret_cast<void*>(functions["button_press"]), reinterpret_cast<void*>(press_hook),
                                    reinterpret_cast<void**>(&original_press), "SDK native button activation");
    ready.store(okay);
    return okay;
}
bool enable_control(uint32_t kind) {
    if(!ready.load()&&!enable())return false;
    std::lock_guard lock(init_mutex);
    if(control_tables.contains(kind))return true;
    if(!attempted_controls.insert(kind).second)return false;
    const char* prefix=kind==SWML_UI_CHECKBOX?"checkbox_":kind==SWML_UI_SLIDER?"slider_":kind==SWML_UI_EDITBOX?"edit_":nullptr;
    const char* type=kind==SWML_UI_CHECKBOX?"c_ui_element_checkbox":kind==SWML_UI_SLIDER?"c_ui_element_slider":kind==SWML_UI_EDITBOX?"c_ui_element_editbox":nullptr;
    if(!prefix)return false;
    uintptr_t vtable=native_find_vtable(type);if(!vtable)return false;
    std::map<std::string,uintptr_t> functions;
    for(const auto& signature:kNativeControlSignatures)if(std::string_view(signature.name).starts_with(prefix)) {
        auto address=native_find_pattern(signature.pattern);
        if(!address||!matches(address,signature.pattern)){corelog("native control unavailable: %s",signature.name);return false;}
        functions[signature.name]=address;
    }
    if(kind==SWML_UI_CHECKBOX)checkbox_ctor=reinterpret_cast<Checkbox>(functions["checkbox_ctor"]);
    if(kind==SWML_UI_SLIDER){slider_ctor=reinterpret_cast<Slider>(functions["slider_ctor"]);slider_get=reinterpret_cast<SliderGet>(functions["slider_get"]);slider_set=reinterpret_cast<SliderSet>(functions["slider_set"]);}
    if(kind==SWML_UI_EDITBOX){edit_ctor=reinterpret_cast<Edit>(functions["edit_ctor"]);edit_set=reinterpret_cast<EditSet>(functions["edit_set"]);edit_filter=reinterpret_cast<EditFilter>(functions["edit_filter"]);}
    control_tables[kind]=vtable;return true;
}
void temporary_text(TemporaryString& text,const std::string& value) {
    text.value.length=uint32_t(value.size());reserve_string(&text.value,text.value.length+1);
    if(!text.value.data)throw std::bad_alloc();memcpy(text.value.data,value.c_str(),value.size()+1);
}
void* create_control(const swml_ui_control_desc& desc) {
    const auto& r=desc.widget.rect;auto kind=desc.widget.kind;
    if(kind==SWML_UI_CHECKBOX){HeapBlock block(0x78);checkbox_ctor(block.pointer,r.x,r.y,r.width,r.anchor);return block.take();}
    if(kind==SWML_UI_SLIDER){
        TemporaryString format;temporary_text(format,"%.2f"); // no caller-controlled native format strings
        HeapBlock block(0xa0);slider_ctor(block.pointer,r.x,r.y,r.width,r.height,r.anchor,float(desc.minimum),float(desc.maximum),float(desc.step),&format.value);return block.take();
    }
    if(kind==SWML_UI_EDITBOX){
        auto value=swml_json::parse(desc.value_json);TemporaryString initial;temporary_text(initial,value.string());
        HeapBlock block(0x80);edit_ctor(block.pointer,r.x,r.y,r.width,r.height,r.anchor,&initial.value,0);return block.take();
    }
    return nullptr;
}
uint32_t control_kind(void* p) {
    auto vtable=*static_cast<uintptr_t*>(p);
    for(const auto& [kind,table]:control_tables)if(vtable==table)return kind;
    return 0;
}
int control_value(void* p,std::string& previous) {
    std::string value;auto kind=control_kind(p);
    if(kind==SWML_UI_CHECKBOX)value=field<uint8_t>(p,0x70)?"true":"false";
    else if(kind==SWML_UI_SLIDER){
        char buffer[64];auto converted=std::to_chars(buffer,buffer+sizeof(buffer),slider_get(p));if(converted.ec!=std::errc{})return -1;value.assign(buffer,converted.ptr);
    }else if(kind==SWML_UI_EDITBOX){
        void* child=field<void*>(p,0x78);if(!child)return -1;
        const auto& text=field<String>(child,0xb8);
        if(text.length>4096||!text.data||text.length>=text.capacity)return -1;
        if(previous.size()==text.length&&memcmp(previous.data(),text.data,text.length)==0)return 0;
        if(!swml_json::valid_utf8(std::string_view(text.data,text.length)))return -1;
        previous.assign(text.data,text.length);return 1;
    }else return -1;
    if(previous==value)return 0;previous=std::move(value);return 1;
}
void control_set(void* p,const std::string& value,uintptr_t game) {
    auto kind=control_kind(p);
    if(kind==SWML_UI_CHECKBOX)field<uint8_t>(p,0x70)=value=="true"?1:0;
    else if(kind==SWML_UI_SLIDER){float number=0;auto converted=std::from_chars(value.data(),value.data()+value.size(),number);if(converted.ec!=std::errc{})throw std::runtime_error("invalid slider value");slider_set(p,game,number);}
    else if(kind==SWML_UI_EDITBOX){TemporaryString text;temporary_text(text,value);edit_set(p,game,&text.value);}
}
void control_configure(void* p,const swml_ui_control_desc& desc,uintptr_t) {
    if(desc.widget.kind!=SWML_UI_EDITBOX)return;
    std::string allowed;
    if(desc.allowed_characters)allowed=desc.allowed_characters;
    else for(char c=32;c<127;++c)allowed+=c;
    TemporaryString text;temporary_text(text,allowed);edit_filter(p,&text.value);
    // The ctor's final parameter is an action code (+68 -> child +5c).
    // Native insertion checks the independent byte limit at child +cc.
    field<uint32_t>(field<void*>(p,0x78),0xcc)=desc.max_characters?desc.max_characters:255;
}
uintptr_t table(void* p) { return field<uintptr_t>(p, 0); }
bool is_a(void* p, const char* name) { return native_object_is_a(p, name); }
std::vector<void*> children(void* p) {
    std::vector<void*> result;
    if (!is_a(p, "mm_ui_element_composite")) return result;
    const auto& r = field<Ring>(p, 0x30);
    if (!r.count) return result;
    if (!r.data || !r.capacity || r.count > r.capacity || r.count > 4096) return result;
    result.reserve(r.count);
    for (uint32_t i = 0; i < r.count; ++i) result.push_back(r.data[(uint64_t(r.head) + i) % r.capacity]);
    return result;
}
void* create(const swml_ui_desc& d) {
    const auto& r = d.rect;
    if (d.kind == SWML_UI_CONTAINER) {
        HeapBlock block(0x58); void* p = block.pointer; container_ctor(p, r.x, r.y, r.width, r.height, r.anchor, 0);
        field<uintptr_t>(p, 0) = container_table; field<uintptr_t>(p, 0x50) = 0;
        return block.take();
    }
    if (d.kind == SWML_UI_LABEL) {
        TemporaryString temporary; auto& text = temporary.value; const char* value = d.text ? d.text : "";
        text.length = static_cast<uint32_t>(strlen(value));
        reserve_string(&text, text.length + 1); if (!text.data) return nullptr;
        memcpy(text.data, value, size_t(text.length) + 1);
        // The constructor consumes/frees the temporary GAME-heap string. Never pass a literal here.
        HeapBlock block(0xc8);
        construct_native_text(text_ctor, block.pointer, d, &text);
        return block.take();
    }
    if (d.kind == SWML_UI_BUTTON) {
        HeapBlock block(0xc8);
        button_ctor(block.pointer, r.x, r.y, r.width, r.height, r.anchor, 0, d.background, d.hover, d.pressed, 1);
        return block.take();
    }
    return nullptr;
}
void attach(void* parent, uintptr_t game, void* child, const swml_ui_desc& d) {
    attach_child(parent, game, child);
    if (d.kind == SWML_UI_BUTTON) {
        auto label = d; label.kind = SWML_UI_LABEL;
        label.rect = {0, 0, d.rect.width, d.rect.height, SWML_UI_CENTER};
        void* caption = create(label); if (!caption) throw std::bad_alloc();
        attach_child(child, game, caption);
    }
}
void erase(void* object, uintptr_t game, void* known_parent) {
    void* parent = field<void*>(object, 0x20);
    // Native attachment inserts into the ring before initialization sets the parent field.
    if (!parent) parent = known_parent;
    if (parent) {
        auto& r = field<Ring>(parent, 0x30);
        if (!r.capacity || r.count > r.capacity || r.count > 4096) return;
        for (uint32_t i = 0; i < r.count; ++i) if (r.data[(uint64_t(r.head) + i) % r.capacity] == object) {
            for (uint32_t j = i; j + 1 < r.count; ++j)
                r.data[(uint64_t(r.head) + j) % r.capacity] = r.data[(uint64_t(r.head) + j + 1) % r.capacity];
            r.data[(uint64_t(r.head) + r.count - 1) % r.capacity] = nullptr; --r.count; break;
        }
    }
    // Native teardown clears descendant focus/hover and releases native render geometry.
    if (field<void*>(reinterpret_cast<void*>(game), 0x188) == object) field<void*>(reinterpret_cast<void*>(game), 0x188) = nullptr;
    if (field<void*>(reinterpret_cast<void*>(game), 0x190) == object) field<void*>(reinterpret_cast<void*>(game), 0x190) = nullptr;
    auto* v = reinterpret_cast<uintptr_t*>(table(object));
    reinterpret_cast<void(*)(void*, uintptr_t)>(v[2])(object, game);
    reinterpret_cast<void(*)(void*, uint32_t)>(v[0])(object, 1);
}
swml_ui_info inspect(void* p) {
    swml_ui_info info{}; info.size = sizeof(info);
    info.kind = is_a(p, "c_ui_element_text") ? SWML_UI_LABEL :
                is_a(p, "c_ui_element_rectangle_button") ? SWML_UI_BUTTON : SWML_UI_CONTAINER;
    if(auto kind=control_kind(p))info.kind=kind;
    info.rect = {field<float>(p, 8), field<float>(p, 12), field<float>(p, 16), field<float>(p, 20), field<uint32_t>(p, 24)};
    info.visible = field<uint8_t>(p, 0x1c) != 0;
    info.enabled = info.kind == SWML_UI_BUTTON ? field<uint8_t>(p, 0x90) != 0 : 1;
    return info;
}
void rect(void* p, const swml_ui_rect& r, uintptr_t game) {
    const bool button = is_a(p, "c_ui_element_rectangle_button");
    if (button) resize_native_button(resize_button, p, game, field<float>(p, 16), field<float>(p, 20), r.width, r.height);
    field<float>(p, 8) = r.x; field<float>(p, 12) = r.y;
    field<float>(p, 16) = r.width; field<float>(p, 20) = r.height; field<uint32_t>(p, 24) = r.anchor;
    if (is_a(p, "c_ui_element_text")) layout_text(p, game);
    // The button's caption is an ordinary native child and follows its parent dimensions.
    else if (button)
        for (auto* child : children(p)) if (is_a(child, "c_ui_element_text")) {
            field<float>(child, 16) = r.width; field<float>(child, 20) = r.height; layout_text(child, game);
        }
}
void visible(void* p, bool value) { field<uint8_t>(p, 0x1c) = value; }
void enabled(void* p, bool value, uintptr_t game) {
    if (is_a(p, "c_ui_element_rectangle_button")) {
        // The native setter reapplies the button's alpha through method 30, and that writes the
        // visibility flag: disabling a hidden button would show it. Enabling must not change it.
        const auto shown = field<uint8_t>(p, 0x1c);
        auto* v = reinterpret_cast<uintptr_t*>(table(p));
        reinterpret_cast<void(*)(void*, uintptr_t, uint8_t)>(v[37])(p, game, value);
        field<uint8_t>(p, 0x1c) = shown;
    }
}
void* text_target(void* p) {
    if (is_a(p, "c_ui_element_text")) return p;
    if (is_a(p, "c_ui_element_rectangle_button"))
        for (auto* child : children(p)) if (is_a(child, "c_ui_element_text")) return child;
    return nullptr;
}
bool get_text(void* p, std::string& out) {
    p = text_target(p); if (!p) return false;
    auto& s = field<String>(p, 0x58);
    if (s.length > 16384 || (s.length && !s.data)) return false;
    out.assign(s.data ? s.data : "", s.length); return true;
}
void set_text(void* p, const std::string& value, uintptr_t game) {
    p = text_target(p); if (!p) return;
    String text{const_cast<char*>(value.c_str()), static_cast<uint32_t>(value.size()), static_cast<uint32_t>(value.size() + 1)};
    set_text_native(p, game, &text); // setter copies; unlike the constructor it does not consume input
}
const NativeUiBackend backend{enable, native_find_vtable, table, is_a, children, create, attach, erase,
                               inspect, rect, visible, enabled, get_text, set_text,
                               enable_control,create_control,control_value,control_set,control_configure};
}
const NativeUiBackend& native_ui_backend() { return backend; }
