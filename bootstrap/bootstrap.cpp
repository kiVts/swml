// Explicit local mod bootstrap. No remote process access, executable patching,
// security configuration changes, or automatic core loading on direct launches.
#include "contract.h"
#include "exports.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <iterator>

extern "C" void* swml_gl_slots[];
namespace {
HMODULE self_module=nullptr,system_gl=nullptr;
INIT_ONCE forwarding_once=INIT_ONCE_STATIC_INIT,start_once=INIT_ONCE_STATIC_INIT;
void log_line(const std::wstring& root,const wchar_t* text){
    OutputDebugStringW(text);if(root.empty())return;
    auto directory=root+L"\\logs";CreateDirectoryW(directory.c_str(),nullptr);FILE* file=nullptr;
    if(_wfopen_s(&file,(directory+L"\\bootstrap.log").c_str(),L"a, ccs=UTF-8")||!file)return;
    SYSTEMTIME t;GetLocalTime(&t);fwprintf(file,L"%02u:%02u:%02u [%lu] %ls\n",t.wHour,t.wMinute,t.wSecond,GetCurrentProcessId(),text);fclose(file);
}
BOOL CALLBACK forwarding(PINIT_ONCE,void*,void**){
    wchar_t path[32768]{};UINT n=GetSystemDirectoryW(path,32768);if(!n||n>=32740)return FALSE;
    wcscat_s(path,L"\\opengl32.dll");system_gl=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!system_gl||system_gl==self_module)return FALSE;
    for(size_t i=0;i<std::size(kGlExports);++i){auto proc=GetProcAddress(system_gl,kGlExports[i]);if(!proc)return FALSE;InterlockedExchangePointer(&swml_gl_slots[i],reinterpret_cast<void*>(proc));}
    return TRUE;
}
BOOL start_impl(){
    const auto root=swml_bootstrap::environment(L"SWML_ROOT");
    if(swml_bootstrap::environment(L"SWML_MODE")!=L"1"||swml_bootstrap::environment(L"SWML_OFF")==L"1") {log_line(root,L"mods disabled; forwarding OpenGL only");return TRUE;}
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);const wchar_t* name=wcsrchr(exe,L'\\');name=name?name+1:exe;
    if(_wcsicmp(name,L"stormworks64.exe")){log_line(root,L"unsupported executable; core not loaded");return TRUE;}
    if(root.empty()||!std::filesystem::path(root).is_absolute()){log_line(root,L"missing absolute SWML_ROOT; core not loaded");return TRUE;}
    const auto core_path=root+L"\\swml_core.dll";
    if(!swml_bootstrap::exported(core_path,"swml_bootstrap_start_v1")){log_line(root,L"incompatible core; explicit-start export missing; core not loaded");return TRUE;}
    HMODULE core=LoadLibraryExW(core_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!core){log_line(root,L"core load failed; see Windows error/debug output");return TRUE;}
    using Start=BOOL(WINAPI*)(uint32_t);auto begin=reinterpret_cast<Start>(GetProcAddress(core,"swml_bootstrap_start_v1"));
    if(!begin||!begin(1)){log_line(root,L"core initialization request failed");return TRUE;}
    // Core stays loaded for process lifetime: hooks/workers cannot be safely unloaded.
    log_line(root,L"core startup requested after successful wglMakeCurrent");return TRUE;
}
BOOL CALLBACK start(PINIT_ONCE,void*,void**){try{return start_impl();}catch(...){OutputDebugStringW(L"SWML bootstrap: startup failed; core left disabled");return TRUE;}}
}
extern "C" void* swml_resolve_gl(unsigned index){
    const DWORD error=GetLastError();
    if(index>=std::size(kGlExports)||!InitOnceExecuteOnce(&forwarding_once,forwarding,nullptr,nullptr)){
        OutputDebugStringW(L"SWML bootstrap: cannot forward the real system OpenGL library");
        RaiseFailFastException(nullptr,nullptr,0);return nullptr;
    }
    void* result=swml_gl_slots[index];SetLastError(error);return result;
}
extern "C" BOOL WINAPI swml_wglMakeCurrent(HDC dc,HGLRC context){
    using MakeCurrent=BOOL(WINAPI*)(HDC,HGLRC);auto original=reinterpret_cast<MakeCurrent>(swml_resolve_gl(kMakeCurrent));
    BOOL ok=original(dc,context);const DWORD error=GetLastError();
    if(ok&&context){try{InitOnceExecuteOnce(&start_once,start,nullptr,nullptr);}catch(...){OutputDebugStringW(L"SWML bootstrap initialization failed; game continues without mods");}}
    SetLastError(error);return ok;
}
extern "C" unsigned WINAPI swml_bootstrap_version(){return 1;}
BOOL WINAPI DllMain(HMODULE module,DWORD reason,void*){if(reason==DLL_PROCESS_ATTACH)self_module=module;return TRUE;}
