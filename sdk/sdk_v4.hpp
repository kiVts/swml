#pragma once
#include "sdk_v3.hpp"
#include "swml_v4.h"
#include "json.h"
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
namespace swml4 {
inline const swml_api_v4* api() { return swml_get_v4(swml()); }
template<class Operation> inline swml_result read_text(Operation operation,std::string& output) {
    uint32_t needed=0;auto result=operation(nullptr,0,&needed);
    if(result!=SWML_BUFFER_SMALL&&result!=SWML_OK)return result;
    if(needed>1024*1024)return SWML_LIMIT;
    std::vector<char> bytes(needed?needed:1);result=operation(bytes.data(),uint32_t(bytes.size()),&needed);
    if(result==SWML_OK)output=bytes.data();return result;
}
inline swml_result settings(const char* schema) { return api()?api()->settings_register(swml_self(),schema):SWML_UNAVAILABLE; }
inline swml_result get(const char* id,std::string& json) {
    return api()?read_text([&](char* out,uint32_t n,uint32_t* needed){return api()->setting_get(swml_self(),id,out,n,needed);},json):SWML_UNAVAILABLE;
}
inline swml_result set(const char* id,const swml_json::Value& value) { return api()?api()->setting_set(swml_self(),id,swml_json::dump(value).c_str()):SWML_UNAVAILABLE; }
inline bool boolean(const char* id,bool fallback=false) {
    std::string json;if(get(id,json)!=SWML_OK)return fallback;
    try{auto value=swml_json::parse(json);return value.is_bool()?value.boolean():fallback;}catch(...){return fallback;}
}
inline double number(const char* id,double fallback=0) {
    std::string json;if(get(id,json)!=SWML_OK)return fallback;
    try{auto value=swml_json::parse(json);return value.is_number()?value.number():fallback;}catch(...){return fallback;}
}
inline swml_result translations(const char* locale,const char* json) { return api()?api()->translations_register(swml_self(),locale,json):SWML_UNAVAILABLE; }
inline swml_result translate(const char* locale,const char* key,std::string& output,const swml_json::Value& arguments=swml_json::Value::Object{}) {
    auto json=swml_json::dump(arguments);
    return api()?read_text([&](char* out,uint32_t n,uint32_t* needed){return api()->translate(swml_self(),locale,key,json.c_str(),out,n,needed);},output):SWML_UNAVAILABLE;
}
inline swml_result registry(const char* name,uint32_t version,const char* schema) { return api()?api()->registry_define(swml_self(),name,version,schema):SWML_UNAVAILABLE; }
inline swml_result put(const char* registry,const char* id,const swml_json::Value& value,const char* scope="") {
    return api()?api()->registry_put(swml_self(),registry,id,scope,swml_json::dump(value).c_str()):SWML_UNAVAILABLE;
}
inline swml_result content(const char* registry,const char* id,std::string& json,const char* scope="") {
    return api()?read_text([&](char* out,uint32_t n,uint32_t* needed){return api()->registry_get(swml_self(),registry,id,scope,out,n,needed);},json):SWML_UNAVAILABLE;
}
inline swml_result logf(uint32_t level,const char* format,...) {
    if(!api())return SWML_UNAVAILABLE;
    if(!api()->log_enabled(swml_self(),level))return level>SWML_LOG_ERROR?SWML_INVALID:SWML_OK;
    char message[16385];va_list arguments;va_start(arguments,format);int length=vsnprintf(message,sizeof(message),format,arguments);va_end(arguments);
    if(length<0||size_t(length)>=sizeof(message))return SWML_LIMIT;
    return swml3::log(level,message);
}
inline swml_result profiling(bool enabled) { return api()?api()->profile_callbacks(swml_self(),enabled?1:0):SWML_UNAVAILABLE; }
inline swml_result stats(swml_v4_stats& out) { out={};out.size=sizeof(out);return api()?api()->stats(swml_self(),&out):SWML_UNAVAILABLE; }
inline swml_result context(uint32_t fields,swml_world_context& out) { out={};out.size=sizeof(out);return api()?api()->context_get(swml_self(),fields,&out):SWML_UNAVAILABLE; }
inline bool same_world(uint64_t generation) { swml_world_context current{};return context(SWML_CONTEXT_WORLD_ID,current)==SWML_OK&&current.generation==generation; }
namespace ui {
inline swml_ui_control_desc description(uint32_t kind,swml_ui_rect rect,const char* value_json) {
    swml_ui_control_desc out{};out.size=sizeof(out);out.widget=swml3::ui::description(kind,rect);out.minimum=0;out.maximum=1;out.value_json=value_json;out.max_characters=255;return out;
}
inline swml_result create(swml_ui_handle parent,const swml_ui_control_desc& desc,swml_ui_handle& out) {
    out=0;return api()?api()->ui_control_create(swml_self(),parent,&desc,&out):SWML_UNAVAILABLE;
}
inline swml_result value(swml_ui_handle element,std::string& json) {
    return api()?read_text([&](char* out,uint32_t n,uint32_t* needed){return api()->ui_value_get(swml_self(),element,out,n,needed);},json):SWML_UNAVAILABLE;
}
inline swml_result value(swml_ui_handle element,const swml_json::Value& json) {
    return api()?api()->ui_value_set(swml_self(),element,swml_json::dump(json).c_str()):SWML_UNAVAILABLE;
}
}
} // namespace swml4
