#include "core.h"
#include "core_v2.h"
#include "core_v4.h"
#include "mod_manifest.h"
#include "settings_schema.h"
#include "content_schema.h"
#include "native_ui.h"
#include "world_context.h"
#include "input_runtime.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace {
using swml_json::Value;
struct Owner {
    std::string schema;
    std::vector<swml_settings::Field> fields;
    std::map<std::string,Value> settings;
    std::map<std::string,Value> languages;
};
struct Definition { swml_mod* owner; uint32_t version; Value schema; };
struct Content { swml_mod* owner; uint64_t revision; std::string json; };
std::mutex mutex;
std::map<swml_mod*,Owner> owners;
std::map<std::string,Definition> definitions;
using ContentKey=std::tuple<std::string,std::string,std::string>; // registry, scope, id
std::map<ContentKey,Content> content;
uint64_t revision=0;
constexpr size_t owner_budget=4*1024*1024,total_budget=32*1024*1024;
uint64_t capabilities() { return SWML_V4_CAP_SETTINGS|SWML_V4_CAP_TRANSLATIONS|SWML_V4_CAP_REGISTRIES|SWML_V4_CAP_ASSETS|SWML_V4_CAP_DIAGNOSTICS|
    (sdk_ui::controls_enabled()?uint64_t(SWML_V4_CAP_NATIVE_CONTROLS):0)|sdk_world::capabilities()|
    (sdk_input::available()?uint64_t(SWML_V4_CAP_INPUT):0); }
bool text(const char* value,size_t limit=65535) { return value && strnlen_s(value,limit+1)<=limit && swml_json::valid_utf8(value); }
bool named(const char* value) { return text(value,95)&&swml_settings::name(value); }
bool scope_name(const char* value) { return value&&(!*value||named(value)); }
bool owned(swml_mod* mod,const char* value) { return named(value)&&std::string_view(value).starts_with(mod->id+"."); }
bool host_settings_allowed(swml_mod* mod) {
    swml_world_context context{};context.size=sizeof(context);
    return sdk_world::get(mod,SWML_CONTEXT_WORLD_ID|SWML_CONTEXT_ROLE,&context)==SWML_OK&&
        (context.role==SWML_ROLE_HOST||context.role==SWML_ROLE_LOCAL);
}
swml_result copy(std::string_view text,char* out,uint32_t capacity,uint32_t* needed) {
    if (needed) *needed=uint32_t(text.size()+1);
    if (!needed||(!out&&capacity)) return SWML_INVALID;
    if (capacity<text.size()+1) return SWML_BUFFER_SMALL;
    memcpy(out,text.data(),text.size()); out[text.size()]='\0'; return SWML_OK;
}
size_t bytes(swml_mod* mod) {
    size_t count=0;
    for (const auto& [owner,state] : owners) if (!mod||owner==mod) {
        count+=state.schema.size(); for (const auto& [locale,table] : state.languages) count+=locale.size()+swml_json::dump(table).size();
    }
    for (const auto& [name,definition] : definitions) if (!mod||definition.owner==mod) count+=name.size()+swml_json::dump(definition.schema).size();
    for (const auto& [key,item] : content) if (!mod||item.owner==mod) count+=item.json.size()+std::get<0>(key).size()+std::get<1>(key).size()+std::get<2>(key).size();
    return count;
}
bool room(swml_mod* mod,size_t extra,size_t removed=0) {
    return extra<=owner_budget && bytes(mod)+extra<=owner_budget+removed && bytes(nullptr)+extra<=total_budget+removed;
}
Value read_setting(swml_mod* mod,const swml_settings::Field& field) {
    const std::wstring section=swml_manifest::wide(field.section), key=swml_manifest::wide(field.key);
    const wchar_t sentinel[]=L"\x1fSWML_MISSING\x1f"; wchar_t buffer[8192]{};
    DWORD size=GetPrivateProfileStringW(section.c_str(),key.c_str(),sentinel,buffer,DWORD(std::size(buffer)),mod->settings.c_str());
    if (size>=std::size(buffer)-1) throw std::runtime_error("setting value too long");
    return std::wstring_view(buffer)==sentinel?field.initial:swml_settings::decode(field,swml_manifest::utf8(buffer));
}
auto field(Owner& owner,const char* id) {
    return std::find_if(owner.fields.begin(),owner.fields.end(),[&](const auto& item){return item.id==id;});
}
swml_result settings_register(swml_mod* mod,const char* json) {
    if (!text(json)) return SWML_INVALID;
    try {
        auto schema=swml_json::parse(json); auto fields=swml_settings::parse(schema); auto canonical=swml_json::dump(schema);
        std::lock_guard lock(mutex); auto owner=owners.find(mod); if (owner==owners.end()) return SWML_DENIED;
        if (!owner->second.schema.empty()) return owner->second.schema==canonical?SWML_OK:SWML_CONFLICT;
        if (!room(mod,canonical.size())) return SWML_LIMIT;
        std::map<std::string,Value> values;
        for (const auto& item : fields) values.emplace(item.id,read_setting(mod,item));
        owner->second.schema=std::move(canonical); owner->second.fields=std::move(fields);owner->second.settings=std::move(values);return SWML_OK;
    } catch (...) { return SWML_INVALID; }
}
swml_result settings_schema(swml_mod* mod,char* out,uint32_t capacity,uint32_t* needed) {
    if (needed) *needed=0;
    std::lock_guard lock(mutex);auto owner=owners.find(mod);if(owner==owners.end())return SWML_DENIED;
    return owner->second.schema.empty()?SWML_NOT_FOUND:copy(owner->second.schema,out,capacity,needed);
}
swml_result setting_get(swml_mod* mod,const char* id,char* out,uint32_t capacity,uint32_t* needed) {
    if (needed) *needed=0; if (!named(id))return SWML_INVALID;
    std::lock_guard lock(mutex);auto owner=owners.find(mod);if(owner==owners.end())return SWML_DENIED;
    auto item=owner->second.settings.find(id);return item==owner->second.settings.end()?SWML_NOT_FOUND:copy(swml_json::dump(item->second),out,capacity,needed);
}
swml_result setting_set(swml_mod* mod,const char* id,const char* json) {
    if(!named(id)||!text(json,8191))return SWML_INVALID;
    try {
        auto value=swml_json::parse(json);std::string event;
        {
            std::lock_guard lock(mutex);auto owner=owners.find(mod);if(owner==owners.end())return SWML_DENIED;
            auto found=field(owner->second,id);if(found==owner->second.fields.end())return SWML_NOT_FOUND;
            swml_settings::validate(*found,value);
            // A host setting cannot be changed live without a verified native authority provider.
            if(found->authority=="host"&&!host_settings_allowed(mod))return SWML_DENIED;
            if(swml_json::dump(owner->second.settings.at(id))==swml_json::dump(value))return SWML_OK;
            auto section=swml_manifest::wide(found->section), key=swml_manifest::wide(found->key),encoded=swml_manifest::wide(swml_settings::encode(*found,value));
            if(!WritePrivateProfileStringW(section.c_str(),key.c_str(),encoded.c_str(),mod->settings.c_str()))return SWML_IO_ERROR;
            event=swml_json::dump(Value::Object{{"id",id},{"previous",owner->second.settings.at(id)},{"value",value},{"restart",found->restart}});
            owner->second.settings[id]=std::move(value);
            if(found->type=="keybind"&&found->section=="input")sdk_input::setting_changed(mod,found->key.c_str(),owner->second.settings[id].string().c_str());
        }
        auto result=core_v2_api()->publish(mod,(mod->id+".settings.changed").c_str(),event.c_str(),uint32_t(event.size()+1));
        return result==SWML_OK?SWML_OK:SWML_APPLIED_WITHOUT_EVENT;
    }catch(...){return SWML_INVALID;}
}
swml_result settings_reload(swml_mod* mod) {
    try {
        std::vector<std::string> changes;
        {
            std::lock_guard lock(mutex);auto owner=owners.find(mod);if(owner==owners.end())return SWML_DENIED;
            if(owner->second.schema.empty())return SWML_NOT_FOUND;
            std::map<std::string,Value> next;
            WritePrivateProfileStringW(nullptr,nullptr,nullptr,mod->settings.c_str());
            for(const auto& item:owner->second.fields)next.emplace(item.id,read_setting(mod,item));
            for(const auto& item:owner->second.fields)if(swml_json::dump(next.at(item.id))!=swml_json::dump(owner->second.settings.at(item.id))) {
                if(item.authority=="host"&&!host_settings_allowed(mod))return SWML_DENIED;
                changes.push_back(swml_json::dump(Value::Object{{"id",item.id},{"previous",owner->second.settings.at(item.id)},{"value",next.at(item.id)},{"restart",item.restart}}));
            }
            owner->second.settings=std::move(next);
            for(const auto& item:owner->second.fields)if(item.type=="keybind"&&item.section=="input")sdk_input::setting_changed(mod,item.key.c_str(),owner->second.settings.at(item.id).string().c_str());
        }
        swml_result result=SWML_OK;
        for(const auto& event:changes) { auto r=core_v2_api()->publish(mod,(mod->id+".settings.changed").c_str(),event.c_str(),uint32_t(event.size()+1));if(r!=SWML_OK)result=r; }
        return result==SWML_OK?SWML_OK:SWML_APPLIED_WITHOUT_EVENT;
    }catch(...){return SWML_INVALID;}
}
swml_result translations_register(swml_mod* mod,const char* locale,const char* json) {
    if(!named(locale)||strlen(locale)>31||!text(json))return SWML_INVALID;
    try {
        auto table=swml_json::parse(json);if(!table.is_object()||table.object().size()>512)return SWML_INVALID;
        for(const auto& [key,value]:table.object())if(!swml_settings::name(key)||!value.is_string()||value.string().size()>4096||value.string().find('\0')!=std::string::npos)return SWML_INVALID;
        auto size=swml_json::dump(table).size();std::lock_guard lock(mutex);auto owner=owners.find(mod);if(owner==owners.end())return SWML_DENIED;
        auto previous=owner->second.languages.find(locale);size_t old=previous==owner->second.languages.end()?0:swml_json::dump(previous->second).size()+strlen(locale);
        if(previous==owner->second.languages.end()&&owner->second.languages.size()>=16)return SWML_LIMIT;
        if(!room(mod,size+strlen(locale),old))return SWML_LIMIT;
        owner->second.languages[locale]=std::move(table);return SWML_OK;
    }catch(...){return SWML_INVALID;}
}
swml_result translate(swml_mod* mod,const char* locale,const char* key,const char* json,char* out,uint32_t capacity,uint32_t* needed) {
    if(needed)*needed=0;if(!named(locale)||!named(key)||!text(json,8191))return SWML_INVALID;
    try {
        auto arguments=swml_json::parse(json);if(!arguments.is_object()||arguments.object().size()>32)return SWML_INVALID;
        std::string value;
        {
            std::lock_guard lock(mutex);auto owner=owners.find(mod);if(owner==owners.end())return SWML_DENIED;
            std::vector<std::string> fallbacks{locale};auto dash=std::string(locale).find_first_of("-_");if(dash!=std::string::npos)fallbacks.push_back(std::string(locale).substr(0,dash));fallbacks.push_back("en");
            bool found=false;
            for(const auto& name:fallbacks) { auto language=owner->second.languages.find(name);if(language==owner->second.languages.end())continue;auto message=language->second.find(key);if(message){value=message->string();found=true;break;} }
            if(!found)return SWML_NOT_FOUND;
        }
        std::string formatted;
        for(size_t i=0;i<value.size();) {
            if(i+1<value.size()&&value[i]=='{'&&value[i+1]=='{'){formatted+='{';i+=2;}
            else if(i+1<value.size()&&value[i]=='}'&&value[i+1]=='}'){formatted+='}';i+=2;}
            else if(value[i]=='{') { auto end=value.find('}',i+1);if(end==value.npos)return SWML_INVALID;auto argument=arguments.find(value.substr(i+1,end-i-1));if(!argument)return SWML_NOT_FOUND;
                if(!argument->is_string()&&!argument->is_number()&&!argument->is_bool())return SWML_INVALID;
                formatted+=argument->is_string()?argument->string():swml_json::dump(*argument);i=end+1;
            }else formatted+=value[i++];
            if(formatted.size()>16384)return SWML_LIMIT;
        }
        if(formatted.find('\0')!=formatted.npos)return SWML_INVALID;
        return copy(formatted,out,capacity,needed);
    }catch(...){return SWML_INVALID;}
}
swml_result registry_define(swml_mod* mod,const char* name,uint32_t version,const char* json) {
    if(!named(name)||!version||!text(json))return SWML_INVALID;
    try {
        auto schema=swml_json::parse(json);swml_content::check_schema(schema);std::lock_guard lock(mutex);
        if(!owners.contains(mod)||!owned(mod,name))return SWML_DENIED;
        if(definitions.contains(name))return definitions.at(name).owner==mod&&definitions.at(name).version==version&&swml_json::dump(definitions.at(name).schema)==swml_json::dump(schema)?SWML_OK:SWML_CONFLICT;
        if(definitions.size()>=256||!room(mod,strlen(name)+strlen(json)))return SWML_LIMIT;
        definitions.emplace(name,Definition{mod,version,std::move(schema)});return SWML_OK;
    }catch(...){return SWML_INVALID;}
}
swml_result registry_put(swml_mod* mod,const char* registry,const char* id,const char* scope,const char* json) {
    if(!named(registry)||!named(id)||!scope_name(scope)||!text(json))return SWML_INVALID;
    try {
        auto value=swml_json::parse(json);auto encoded=swml_json::dump(value);std::lock_guard lock(mutex);
        if(!owners.contains(mod)||!owned(mod,id))return SWML_DENIED;
        auto definition=definitions.find(registry);if(definition==definitions.end())return SWML_NOT_FOUND;
        swml_content::validate(value,definition->second.schema);
        ContentKey key{registry,scope,id};auto previous=content.find(key);
        if(previous!=content.end()&&previous->second.owner!=mod)return SWML_DENIED;
        size_t removed=previous==content.end()?0:previous->second.json.size()+strlen(registry)+strlen(scope)+strlen(id);
        size_t owned_entries=0;for(const auto& [k,item]:content){(void)k;if(item.owner==mod)++owned_entries;}
        if((previous==content.end()&&(content.size()>=4096||owned_entries>=512))||!room(mod,encoded.size()+strlen(registry)+strlen(scope)+strlen(id),removed))return SWML_LIMIT;
        content.insert_or_assign(key,Content{mod,++revision,std::move(encoded)});return SWML_OK;
    }catch(...){return SWML_INVALID;}
}
swml_result registry_get(swml_mod* mod,const char* registry,const char* id,const char* scope,char* out,uint32_t capacity,uint32_t* needed) {
    if(needed)*needed=0;if(!named(registry)||!named(id)||!scope_name(scope))return SWML_INVALID;
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;
    auto item=content.find({registry,scope,id});return item==content.end()?SWML_NOT_FOUND:copy(item->second.json,out,capacity,needed);
}
swml_result registry_list(swml_mod* mod,const char* registry,const char* scope,char* out,uint32_t capacity,uint32_t* needed) {
    if(needed)*needed=0;if(!named(registry)||!scope_name(scope))return SWML_INVALID;
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;
    auto definition=definitions.find(registry);if(definition==definitions.end())return SWML_NOT_FOUND;
    Value::Array list;
    for(const auto& [key,item]:content)if(std::get<0>(key)==registry&&std::get<1>(key)==scope)
        list.emplace_back(Value::Object{{"id",std::get<2>(key)},{"owner",item.owner->id},{"revision",std::to_string(item.revision)},{"version",double(definition->second.version)}});
    return copy(swml_json::dump(list),out,capacity,needed);
}
swml_result asset_read(swml_mod* mod,const char* relative,void* out,uint32_t capacity,uint32_t* needed) {
    if(needed)*needed=0;if(!text(relative,199)||!needed||(!out&&capacity))return SWML_INVALID;
    std::wstring path;
    try {
        auto name=swml_manifest::wide(relative);if(!swml_manifest::relative_path(name))return SWML_INVALID;
        std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;path=mod->dir+L"\\assets";
        // Reject links in every path component, including the asset root.
        auto verify=[](const std::wstring& p){auto a=GetFileAttributesW(p.c_str());return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);};
        if(!verify(path))return SWML_NOT_FOUND;
        size_t at=0;do{auto end=name.find_first_of(L"/\\",at);path+=L"\\"+name.substr(at,end-at);if(!verify(path))return SWML_DENIED;if(end==name.npos)break;at=end+1;}while(true);
    }catch(...){return SWML_INVALID;}
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return SWML_NOT_FOUND;
    LARGE_INTEGER length{};swml_result result=SWML_IO_ERROR;
    if(GetFileSizeEx(file,&length)&&length.QuadPart>=0&&length.QuadPart<=1024*1024){*needed=uint32_t(length.QuadPart);if(capacity<*needed)result=SWML_BUFFER_SMALL;else{DWORD got=0;result=!*needed||(ReadFile(file,out,*needed,&got,nullptr)&&got==*needed)?SWML_OK:SWML_IO_ERROR;}}
    else result=SWML_LIMIT;
    CloseHandle(file);return result;
}
swml_result log_threshold(swml_mod* mod,uint32_t level) {
    if(level>SWML_LOG_ERROR)return SWML_INVALID;std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;
    std::atomic_ref(mod->log_threshold).store(level);return SWML_OK;
}
int log_enabled(swml_mod* mod,uint32_t level) {
    std::lock_guard lock(mutex);return owners.contains(mod)&&level<=SWML_LOG_ERROR&&level>=std::atomic_ref(mod->log_threshold).load();
}
swml_result profile_callbacks(swml_mod* mod,int enabled) {
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;std::atomic_ref(mod->profiling).store(enabled?1u:0u);return SWML_OK;
}
swml_result stats(swml_mod* mod,swml_v4_stats* out) {
    if(!out||out->size<sizeof(*out))return SWML_INVALID;std::lock_guard lock(mutex);auto owner=owners.find(mod);if(owner==owners.end())return SWML_DENIED;
    swml_v4_stats value{};value.size=sizeof(value);value.settings=uint32_t(owner->second.fields.size());value.languages=uint32_t(owner->second.languages.size());value.owned_bytes=bytes(mod);
    for(const auto& [name,d]:definitions){(void)name;if(d.owner==mod)++value.registries;}
    for(const auto& [key,item]:content){(void)key;if(item.owner==mod)++value.entries;}
    value.profiled_callbacks=std::atomic_ref(mod->profiled_callbacks).load();value.callback_nanoseconds=std::atomic_ref(mod->callback_ns).load();value.longest_callback_nanoseconds=std::atomic_ref(mod->longest_callback_ns).load();*out=value;return SWML_OK;
}
const swml_api_v4 api{SWML_V4_VERSION,sizeof(swml_api_v4),capabilities,settings_register,settings_schema,setting_get,setting_set,settings_reload,
    translations_register,translate,registry_define,registry_put,registry_get,registry_list,asset_read,log_threshold,log_enabled,profile_callbacks,stats,
    sdk_ui::create_control,sdk_ui::value_get,sdk_ui::value_set,
    sdk_world::bind,sdk_world::update,sdk_world::get,sdk_world::read,sdk_world::write,
    sdk_input::action_register,sdk_input::rebind,sdk_input::cancel,sdk_input::list};
}
const swml_api_v4* core_v4_api(){return &api;}
bool core_v4_attach(swml_mod* mod){
    sdk_input::attach(mod);
    sdk_world::attach(mod);
    {std::lock_guard lock(mutex);owners.try_emplace(mod);}
    auto path=mod->dir+L"\\settings.schema.json";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){
        auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return true;
        mod->status="failed: cannot read settings.schema.json";return false;
    }
    LARGE_INTEGER size{};std::string json;bool okay=GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<=65535;
    if(okay){json.resize(size_t(size.QuadPart));DWORD got=0;okay=ReadFile(file,json.data(),DWORD(json.size()),&got,nullptr)&&got==json.size();}
    CloseHandle(file);
    if(!okay||settings_register(mod,json.c_str())!=SWML_OK){mod->status="failed: invalid settings schema or persisted setting";return false;}
    return true;
}
void core_v4_drop(swml_mod* mod){
    sdk_input::drop(mod);
    sdk_world::drop(mod);
    std::lock_guard lock(mutex);owners.erase(mod);std::atomic_ref(mod->profiling).store(0);
    for(auto it=definitions.begin();it!=definitions.end();)if(it->second.owner==mod){
        for(auto item=content.begin();item!=content.end();)if(std::get<0>(item->first)==it->first)item=content.erase(item);else++item;
        it=definitions.erase(it);
    }else++it;
    for(auto it=content.begin();it!=content.end();)if(it->second.owner==mod)it=content.erase(it);else++it;
}
