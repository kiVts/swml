#include "world_context.h"
#include "core.h"
#include "core_v2.h"
#include "mod_manifest.h"
#include <libdeflate.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace sdk_world {
namespace {
constexpr uint32_t all_fields=31, dependent_fields=SWML_CONTEXT_ROLE|SWML_CONTEXT_TIME|SWML_CONTEXT_SAVE_EPOCH;
struct Provider {swml_mod* owner;uint32_t fields;};
std::mutex mutex;std::set<swml_mod*> owners;std::map<swml_handle,Provider> providers;
swml_handle next=1;
swml_world_context context{sizeof(swml_world_context)};
uint32_t claims=0;
bool nonzero(const uint8_t* id){return std::any_of(id,id+16,[](uint8_t byte){return byte!=0;});}
swml_result notify(){
    swml_world_context snapshot;
    {std::lock_guard lock(mutex);snapshot=context;}
    return core_v2_queue_builtin("swml.world.context",&snapshot,sizeof(snapshot));
}
bool file_name(const char* name){
    return name&&*name&&strnlen_s(name,41)<=40&&std::all_of(name,name+strlen(name),[](char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';});
}
std::wstring storage_name(const char* name){
    constexpr char hex[]="0123456789abcdef";std::string result="world_";
    for(auto byte:context.world_id){result+=hex[byte>>4];result+=hex[byte&15];}
    result+="_";result+=name;return swml_manifest::wide(result);
}
uint32_t crc(const uint8_t* header,const uint8_t* bytes,size_t length){
    return libdeflate_crc32(libdeflate_crc32(0,header,32),bytes,length);
}
uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void put32(std::vector<uint8_t>& out,uint32_t value){for(unsigned shift=0;shift<32;shift+=8)out.push_back(uint8_t(value>>shift));}
bool valid_blob(const std::vector<uint8_t>& bytes){
    return bytes.size()>=36&&memcmp(bytes.data(),"SW4W\x01\0\0\0",8)==0&&memcmp(bytes.data()+8,context.world_id,16)==0&&u32(bytes.data()+24)!=0&&
        u32(bytes.data()+28)==bytes.size()-36&&crc(bytes.data(),bytes.data()+36,bytes.size()-36)==u32(bytes.data()+32);
}
swml_result load(swml_mod* mod,const std::wstring& name,std::vector<uint8_t>& bytes){
    uint32_t needed=0;auto result=core_v2_api()->storage_read(mod,name.c_str(),nullptr,0,&needed);
    if(result!=SWML_BUFFER_SMALL&&result!=SWML_OK)return result;
    if(needed>1024*1024+36)return SWML_LIMIT;
    bytes.resize(needed);result=core_v2_api()->storage_read(mod,name.c_str(),bytes.data(),needed,&needed);
    return result==SWML_OK&&valid_blob(bytes)?SWML_OK:result==SWML_OK?SWML_CORRUPT:result;
}
swml_result authorized(swml_mod* mod,uint64_t generation){
    if(!owners.contains(mod))return SWML_DENIED;
    if((context.fields&(SWML_CONTEXT_WORLD_ID|SWML_CONTEXT_ROLE))!=(SWML_CONTEXT_WORLD_ID|SWML_CONTEXT_ROLE))return SWML_UNAVAILABLE;
    if(generation!=context.generation)return SWML_CONFLICT;
    return context.role==SWML_ROLE_HOST||context.role==SWML_ROLE_LOCAL?SWML_OK:SWML_DENIED;
}
}
void attach(swml_mod* mod){std::lock_guard lock(mutex);owners.insert(mod);}
void drop(swml_mod* mod){
    bool changed=false;
    {std::lock_guard lock(mutex);owners.erase(mod);
     for(auto it=providers.begin();it!=providers.end();)if(it->second.owner==mod){
         uint32_t mask=it->second.fields;claims&=~mask;context.fields&=~mask;
         if(mask&SWML_CONTEXT_WORLD_ID){++context.generation;memset(context.world_id,0,16);context.fields&=~dependent_fields;}
         it=providers.erase(it);changed=true;
     }else++it;
     if(changed)++context.revision;
    }
    if(changed)notify();
}
uint64_t capabilities(){
    std::lock_guard lock(mutex);uint64_t out=context.fields?SWML_V4_CAP_CONTEXT:0;
    if((context.fields&(SWML_CONTEXT_WORLD_ID|SWML_CONTEXT_ROLE))==(SWML_CONTEXT_WORLD_ID|SWML_CONTEXT_ROLE)&&(context.role==SWML_ROLE_HOST||context.role==SWML_ROLE_LOCAL))out|=SWML_V4_CAP_WORLD_STORAGE;
    return out;
}
swml_result bind(swml_mod* mod,uint32_t fields,swml_handle* out){
    if(out)*out=0;if(!out||!fields||(fields&~all_fields))return SWML_INVALID;
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;
    if(fields&claims)return SWML_CONFLICT;if(!next)return SWML_LIMIT;
    auto token=next++;providers.emplace(token,Provider{mod,fields});claims|=fields;*out=token;return SWML_OK;
}
swml_result update(swml_mod* mod,swml_handle token,const swml_world_context* value){
    if(!value||value->size<sizeof(*value)||(value->fields&~all_fields))return SWML_INVALID;
    {
        std::lock_guard lock(mutex);auto provider=providers.find(token);
        if(!owners.contains(mod)||provider==providers.end()||provider->second.owner!=mod)return SWML_DENIED;
        auto mask=provider->second.fields;if(value->fields&~mask)return SWML_DENIED;
        if(value->fields&SWML_CONTEXT_MODE){if(!memchr(value->mode,0,sizeof(value->mode))||!swml_manifest::valid_id(value->mode)||value->mode_preview>1)return SWML_INVALID;}
        if((value->fields&SWML_CONTEXT_WORLD_ID)&&!nonzero(value->world_id))return SWML_INVALID;
        if(value->fields&dependent_fields){
            bool new_id=(mask&SWML_CONTEXT_WORLD_ID)&&(value->fields&SWML_CONTEXT_WORLD_ID);
            if(!new_id&&!(context.fields&SWML_CONTEXT_WORLD_ID))return SWML_UNAVAILABLE;
            if(value->generation!=context.generation)return SWML_CONFLICT;
        }
        if((value->fields&SWML_CONTEXT_ROLE)&&(value->role<SWML_ROLE_HOST||value->role>SWML_ROLE_LOCAL))return SWML_INVALID;
        auto next_context=context;
        if(mask&SWML_CONTEXT_WORLD_ID){
            bool previous=(context.fields&SWML_CONTEXT_WORLD_ID)!=0, incoming=(value->fields&SWML_CONTEXT_WORLD_ID)!=0;
            if(previous!=incoming||(incoming&&memcmp(context.world_id,value->world_id,16)!=0)){
                ++next_context.generation;next_context.fields&=~dependent_fields;next_context.role=0;next_context.save_epoch=next_context.simulation_ms=0;
            }
            memcpy(next_context.world_id,value->world_id,16);
        }
        if(value->fields&SWML_CONTEXT_SAVE_EPOCH){if((next_context.fields&SWML_CONTEXT_SAVE_EPOCH)&&value->save_epoch<next_context.save_epoch)return SWML_CONFLICT;next_context.save_epoch=value->save_epoch;}
        if(value->fields&SWML_CONTEXT_TIME){if((next_context.fields&SWML_CONTEXT_TIME)&&value->simulation_ms<next_context.simulation_ms)return SWML_CONFLICT;next_context.simulation_ms=value->simulation_ms;}
        if(value->fields&SWML_CONTEXT_MODE){memcpy(next_context.mode,value->mode,sizeof(value->mode));next_context.mode_preview=value->mode_preview;}
        if(value->fields&SWML_CONTEXT_ROLE)next_context.role=value->role;
        next_context.fields=(next_context.fields&~mask)|value->fields;
        if(memcmp(&next_context,&context,sizeof(context))==0)return SWML_OK;
        next_context.revision=context.revision+1;context=next_context;
    }
    return notify()==SWML_OK?SWML_OK:SWML_APPLIED_WITHOUT_EVENT;
}
swml_result get(swml_mod* mod,uint32_t required,swml_world_context* out){
    if(!out||out->size<sizeof(*out)||(required&~all_fields))return SWML_INVALID;
    std::lock_guard lock(mutex);if(!owners.contains(mod))return SWML_DENIED;
    *out=context;return(context.fields&required)==required?SWML_OK:SWML_UNAVAILABLE;
}
swml_result read(swml_mod* mod,uint64_t generation,const char* name,uint32_t expected_schema,uint32_t* stored,void* out,uint32_t capacity,uint32_t* needed){
    if(needed)*needed=0;if(stored)*stored=0;
    if(!needed||!stored||(!out&&capacity)||!file_name(name))return SWML_INVALID;
    std::lock_guard lock(mutex);auto permission=authorized(mod,generation);if(permission!=SWML_OK)return permission;
    auto path=storage_name(name);std::vector<uint8_t> bytes;auto result=load(mod,path,bytes);bool backup=false;
    if(result!=SWML_OK){auto recovered=load(mod,path+L".bak",bytes);if(recovered!=SWML_OK)return result;backup=true;}
    *stored=u32(bytes.data()+24);*needed=u32(bytes.data()+28);
    if(expected_schema&&expected_schema!=*stored)return SWML_CONFLICT;
    if(capacity<*needed)return SWML_BUFFER_SMALL;
    if(*needed)memcpy(out,bytes.data()+36,*needed);return backup?SWML_RECOVERED_BACKUP:SWML_OK;
}
swml_result write(swml_mod* mod,uint64_t generation,const char* name,uint32_t schema,const void* data,uint32_t size){
    if(!file_name(name)||!schema||size>1024*1024||(size&&!data))return SWML_INVALID;
    std::lock_guard lock(mutex);auto permission=authorized(mod,generation);if(permission!=SWML_OK)return permission;
    auto path=storage_name(name);std::vector<uint8_t> previous;
    auto loaded=load(mod,path,previous);
    if(loaded==SWML_OK){auto result=core_v2_api()->storage_write(mod,(path+L".bak").c_str(),previous.data(),uint32_t(previous.size()));if(result!=SWML_OK)return result;}
    else if(loaded!=SWML_NOT_FOUND&&loaded!=SWML_CORRUPT)return loaded;
    std::vector<uint8_t> bytes{'S','W','4','W',1,0,0,0};bytes.insert(bytes.end(),context.world_id,context.world_id+16);
    put32(bytes,schema);put32(bytes,size);auto checksum=crc(bytes.data(),static_cast<const uint8_t*>(data),size);put32(bytes,checksum);
    if(size)bytes.insert(bytes.end(),static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+size);
    return core_v2_api()->storage_write(mod,path.c_str(),bytes.data(),uint32_t(bytes.size()));
}
}
