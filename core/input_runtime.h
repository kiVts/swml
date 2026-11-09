#pragma once
#include "swml_v4.h"
#include <cstdint>
struct NativeInputBackend { bool (*enable)(swml_mod*); uintptr_t (*class_table)(const char*); };
const NativeInputBackend& native_input_backend();
namespace sdk_input {
void attach(swml_mod*);
void drop(swml_mod*);
bool requested();
bool available();
bool may_observe(uintptr_t table);
void set_backend_for_tests(const NativeInputBackend*);
swml_result action_register(swml_mod*,const swml_action_desc*,swml_callback,void*,swml_handle*);
swml_result rebind(swml_mod*,swml_handle,const char*);
swml_result cancel(swml_mod*,swml_handle);
swml_result list(swml_mod*,char*,uint32_t,uint32_t*);
void setting_changed(swml_mod*,const char* key,const char* chord);
// Native OS observation never consumes a message. Delivery occurs through 
// only after a complete observed native UI context has updated.
void window_ready(bool);
void focus(bool);
void key(uint32_t,bool);
void observe(uintptr_t object,uintptr_t table,bool text_entry);
void forget(uintptr_t object);
void finish_frame();
// Implemented by the native provider. Called from existing native UI/present hooks.
void native_present(void* dc);
void native_observe(void* object,uintptr_t game);
void native_forget(void* object);
}
