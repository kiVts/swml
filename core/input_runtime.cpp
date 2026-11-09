#include "input_runtime.h"
#include "core.h"
#include "core_v2.h"
#include "mod_manifest.h"
#include "input_bindings.h"
#include "json.h"
#include "settings_schema.h"
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
namespace sdk_input {
namespace {
struct Action {swml_mod* owner;swml_handle handle;std::string name,topic,root;uintptr_t table;swml_input::Binding binding;uint32_t repeat;uint64_t last=0;bool pressed=false;};
struct Key {uint32_t code,modifiers;bool down;};
struct Context {uintptr_t table;bool text_entry;};
std::mutex mutex;std::set<swml_mod*> owners;std::map<swml_handle,Action> actions;
std::map<uintptr_t,Context> contexts;std::array<bool,256> keys{};std::deque<Key> events;
std::set<uintptr_t> last_roots;std::atomic<bool> needed{false},ready{false};bool focused=false;uint64_t generation=1;
std::array<std::atomic<uint64_t>,16> table_filter{};
const NativeInputBackend* test_backend=nullptr;
const NativeInputBackend& backend(){return test_backend?*test_backend:native_input_backend();}
uint32_t modifiers(){return(keys[17]||keys[162]||keys[163]?1u:0u)|(keys[16]||keys[160]||keys[161]?2u:0u)|(keys[18]||keys[164]||keys[165]?4u:0u)|(keys[91]||keys[92]?8u:0u);}
void emit(Action& action,uint32_t phase){
    swml_action_event event{sizeof(event),phase,action.binding.key,action.binding.modifiers,action.handle,generation};
    core_v2_api()->publish(action.owner,action.topic.c_str(),&event,sizeof(event));
}
void release(Action& action){if(action.pressed){action.pressed=false;emit(action,SWML_INPUT_RELEASE);}}
swml_result apply(Action& action,const char* chord,bool persist){
    if(!chord||strnlen_s(chord,96)>=96)return SWML_INVALID;auto binding=swml_input::parse(chord);if(!binding)return SWML_INVALID;
    if(persist&&!WritePrivateProfileStringW(L"input",swml_manifest::wide(action.name).c_str(),swml_manifest::wide(swml_input::format(*binding)).c_str(),action.owner->settings.c_str()))return SWML_IO_ERROR;
    if(action.binding.key==binding->key&&action.binding.modifiers==binding->modifiers)return SWML_OK;
    release(action);action.binding=*binding;return SWML_OK;
}
}
void set_backend_for_tests(const NativeInputBackend* value){test_backend=value;}
void attach(swml_mod* mod){std::lock_guard lock(mutex);owners.insert(mod);}
void drop(swml_mod* mod){
    std::lock_guard lock(mutex);owners.erase(mod);
    for(auto it=actions.begin();it!=actions.end();)if(it->second.owner==mod){core_v2_api()->cancel(mod,it->first);it=actions.erase(it);}else++it;
    needed.store(!actions.empty());if(actions.empty()){keys.fill(false);events.clear();contexts.clear();last_roots.clear();++generation;}
}
bool requested(){return needed.load(std::memory_order_relaxed);}
bool available(){return ready.load();}
bool may_observe(uintptr_t table){size_t bit=(table>>3)&1023;return(table_filter[bit/64].load(std::memory_order_relaxed)&(1ull<<(bit%64)))!=0;}
swml_result action_register(swml_mod* mod,const swml_action_desc* d,swml_callback callback,void* user,swml_handle* out){
    if(out)*out=0;
    if(!out||!d||d->size<sizeof(*d)||!callback||!d->name||!d->native_root_class||strnlen_s(d->name,80)>=80||
        strnlen_s(d->native_root_class,256)>=256||!*d->native_root_class||!swml_json::valid_utf8(d->native_root_class)||
        !swml_settings::name(d->name)||d->modifiers>15||(d->repeat_ms&&d->repeat_ms<50)||d->repeat_ms>60000)return SWML_INVALID;
    {
        std::lock_guard lock(mutex);if(!owners.contains(mod)||!std::string_view(d->name).starts_with(mod->id+"."))return SWML_DENIED;
    }
    auto formatted=swml_input::format({d->key,d->modifiers});if(formatted.empty())return SWML_INVALID;
    auto table=backend().class_table(d->native_root_class);if(!table)return SWML_UNAVAILABLE;
    if(!(core_v2_api()->capabilities()&SWML_CAP_GAME_LOOP)||!backend().enable||!backend().enable(mod))return SWML_UNAVAILABLE;
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;
    if(actions.size()>=128)return SWML_LIMIT;
    for(const auto& [id,action]:actions){(void)id;if(action.name==d->name)return SWML_CONFLICT;}
    std::wstring override=swml_manifest::read(mod->settings,L"input",swml_manifest::wide(d->name).c_str(),swml_manifest::wide(formatted).c_str());
    auto binding=swml_input::parse(swml_manifest::utf8(override));if(!binding)return SWML_INVALID;
    auto topic=mod->id+".input."+std::string(d->name).substr(mod->id.size()+1);if(topic.size()>=SWML_NAME_CAPACITY)return SWML_LIMIT;
    swml_handle handle=0;auto result=core_v2_api()->subscribe(mod,topic.c_str(),callback,user,&handle);if(result!=SWML_OK)return result;
    actions.emplace(handle,Action{mod,handle,d->name,topic,d->native_root_class,table,*binding,d->repeat_ms});
    size_t bit=(table>>3)&1023;table_filter[bit/64].fetch_or(1ull<<(bit%64),std::memory_order_relaxed);needed.store(true);*out=handle;return SWML_OK;
}
swml_result rebind(swml_mod* mod,swml_handle handle,const char* chord){
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;auto action=actions.find(handle);if(action==actions.end())return SWML_NOT_FOUND;
    if(action->second.owner!=mod)return SWML_DENIED;return apply(action->second,chord,true);
}
swml_result cancel(swml_mod* mod,swml_handle handle){
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;auto action=actions.find(handle);if(action==actions.end())return SWML_NOT_FOUND;
    if(action->second.owner!=mod)return SWML_DENIED;auto result=core_v2_api()->cancel(mod,handle);actions.erase(action);needed.store(!actions.empty());
    if(actions.empty()){keys.fill(false);events.clear();contexts.clear();last_roots.clear();++generation;}
    return result;
}
void setting_changed(swml_mod* mod,const char* key,const char* chord){
    std::lock_guard lock(mutex);for(auto& [id,action]:actions){(void)id;if(action.owner==mod&&action.name==key)apply(action,chord,false);}
}
swml_result list(swml_mod* mod,char* out,uint32_t capacity,uint32_t* needed_out){
    if(needed_out)*needed_out=0;if(!needed_out||(!out&&capacity))return SWML_INVALID;
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;swml_json::Value::Array list;
    for(const auto& [id,action]:actions)list.emplace_back(swml_json::Value::Object{{"name",action.name},{"owner",action.owner->id},{"root",action.root},{"binding",swml_input::format(action.binding)},{"handle",std::to_string(id)}});
    auto json=swml_json::dump(list);*needed_out=uint32_t(json.size()+1);if(capacity<*needed_out)return SWML_BUFFER_SMALL;memcpy(out,json.c_str(),*needed_out);return SWML_OK;
}
void window_ready(bool value){ready.store(value);if(!value)focus(false);}
void focus(bool value){std::lock_guard lock(mutex);if(focused!=value){++generation;focused=value;keys.fill(false);events.clear();if(!value)for(auto& [id,action]:actions){(void)id;release(action);}}}
void key(uint32_t code,bool down){
    if(code>=keys.size())return;std::lock_guard lock(mutex);if(!focused||keys[code]==down)return;keys[code]=down;
    if(events.size()>=256){keys.fill(false);events.clear();++generation;for(auto& [id,action]:actions){(void)id;release(action);}return;}
    events.push_back({code,modifiers(),down});
}
void observe(uintptr_t object,uintptr_t table,bool text_entry){
    if(!requested()||!may_observe(table))return;std::lock_guard lock(mutex);
    bool interested=false;for(const auto& [id,action]:actions){(void)id;if(action.table==table)interested=true;}
    if(interested&&contexts.size()<256)contexts[object]={table,text_entry};
}
void forget(uintptr_t object){std::lock_guard lock(mutex);contexts.erase(object);}
void finish_frame(){
    if(!requested())return;std::lock_guard lock(mutex);std::set<uintptr_t> roots;
    for(const auto& [object,state]:contexts)if(!state.text_entry)roots.insert(object);
    if(roots!=last_roots){last_roots=std::move(roots);++generation;}
    auto now=core_v2_api()->now_ms();
    for(auto& [id,action]:actions){(void)id;bool allowed=focused&&ready.load();bool observed=false;
        for(const auto& [object,state]:contexts){(void)object;if(state.table==action.table&&!state.text_entry)observed=true;}
        allowed&=observed;if(!allowed){release(action);continue;}
        for(const auto& event:events)if(event.code==action.binding.key){
            if(!event.down)release(action);
            else if(event.modifiers==action.binding.modifiers&&!action.pressed){action.pressed=true;action.last=now;emit(action,SWML_INPUT_PRESS);}
        }
        if(action.pressed&&(modifiers()!=action.binding.modifiers||!keys[action.binding.key]))release(action);
        if(action.pressed&&action.repeat&&now-action.last>=action.repeat){action.last=now;emit(action,SWML_INPUT_HELD);}
    }
    events.clear();contexts.clear();
}
}
