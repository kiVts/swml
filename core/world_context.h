#pragma once
#include "swml_v4.h"
namespace sdk_world {
void attach(swml_mod*);
void drop(swml_mod*);
uint64_t capabilities();
swml_result bind(swml_mod*,uint32_t,swml_handle*);
swml_result update(swml_mod*,swml_handle,const swml_world_context*);
swml_result get(swml_mod*,uint32_t,swml_world_context*);
swml_result read(swml_mod*,uint64_t,const char*,uint32_t,uint32_t*,void*,uint32_t,uint32_t*);
swml_result write(swml_mod*,uint64_t,const char*,uint32_t,const void*,uint32_t);
}
