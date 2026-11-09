#pragma once
#include "swml_v2.h"
const swml_api_v2* core_v2_api();
void core_v2_attach(swml_mod*);
void core_v2_drop(swml_mod*);
void core_v2_available(bool game, bool render);
void core_v2_ready();
void core_v2_dispatch(uint32_t lane, const char* topic, const void* data = nullptr, uint32_t size = 0);
void core_v2_pump(uint32_t lane);
swml_result core_v2_forward(swml_mod* sender, swml_mod* recipient, const char* topic, const void*, uint32_t);
swml_result core_v2_queue_builtin(const char* topic,const void*,uint32_t);
