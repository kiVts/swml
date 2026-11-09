#include "input_runtime.h"
#include "native_ui.h"
#include "resolve.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <iterator>
#include <string_view>
namespace {
std::mutex install_mutex;
std::atomic<HWND> window{nullptr};
std::atomic<HHOOK> observer{nullptr};
std::atomic<HHOOK> queued_observer{nullptr};
std::atomic<bool> hook_attempted{false};
void observe(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        if(hwnd==window.load()){
            switch(message){
            case WM_SETFOCUS:sdk_input::focus(true);break;
            case WM_KILLFOCUS:sdk_input::focus(false);break;
            case WM_ACTIVATEAPP:sdk_input::focus(wp!=0);break;
            case WM_KEYDOWN:case WM_SYSKEYDOWN:case WM_KEYUP:case WM_SYSKEYUP:{
                uint32_t key=uint32_t(wp);bool extended=(lp&(1ll<<24))!=0;
                if(key==VK_CONTROL)key=extended?VK_RCONTROL:VK_LCONTROL;
                else if(key==VK_MENU)key=extended?VK_RMENU:VK_LMENU;
                else if(key==VK_SHIFT)key=MapVirtualKeyW(UINT((lp>>16)&255),MAPVK_VSC_TO_VK_EX);
                sdk_input::key(key,message==WM_KEYDOWN||message==WM_SYSKEYDOWN);break;
            }
            case WM_LBUTTONDOWN:case WM_LBUTTONUP:sdk_input::key(VK_LBUTTON,message==WM_LBUTTONDOWN);break;
            case WM_RBUTTONDOWN:case WM_RBUTTONUP:sdk_input::key(VK_RBUTTON,message==WM_RBUTTONDOWN);break;
            case WM_MBUTTONDOWN:case WM_MBUTTONUP:sdk_input::key(VK_MBUTTON,message==WM_MBUTTONDOWN);break;
            case WM_XBUTTONDOWN:case WM_XBUTTONUP:sdk_input::key(HIWORD(wp)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2,message==WM_XBUTTONDOWN);break;
            case WM_NCDESTROY:window.store(nullptr);hook_attempted.store(false);sdk_input::window_ready(false);break;
            }
        }
}
LRESULT CALLBACK observe_message(int code,WPARAM wp,LPARAM lp){
    if(code>=0){auto* message=reinterpret_cast<CWPSTRUCT*>(lp);observe(message->hwnd,message->message,message->wParam,message->lParam);}
    return CallNextHookEx(nullptr,code,wp,lp); // observation never consumes native input
}
LRESULT CALLBACK observe_queued(int code,WPARAM wp,LPARAM lp){
    // Keyboard/mouse messages are posted to the queue. CALLWNDPROC alone covers
    // sent messages, not this path. Ignore PM_NOREMOVE to avoid duplicate edges.
    if(code>=0&&wp==PM_REMOVE){auto* message=reinterpret_cast<MSG*>(lp);observe(message->hwnd,message->message,message->wParam,message->lParam);}
    return CallNextHookEx(nullptr,code,wp,lp);
}
bool enable(swml_mod* mod){return sdk_ui::enable(mod)==SWML_OK;}
const NativeInputBackend backend{enable,native_find_vtable};
}
const NativeInputBackend& native_input_backend(){return backend;}
namespace sdk_input {
void native_present(void* raw_dc){
    if(!requested()){
        if(observer.load()||queued_observer.load()){
            std::lock_guard lock(install_mutex);auto previous=observer.exchange(nullptr);if(previous)UnhookWindowsHookEx(previous);
            auto queued=queued_observer.exchange(nullptr);if(queued)UnhookWindowsHookEx(queued);
            window.store(nullptr);hook_attempted.store(false);window_ready(false);
        }
        return;
    }
    if(!window.load()){
        std::lock_guard lock(install_mutex);auto candidate=WindowFromDC(static_cast<HDC>(raw_dc));
        DWORD process=0;DWORD thread=candidate?GetWindowThreadProcessId(candidate,&process):0;
        wchar_t type[128]{};if(candidate)GetClassNameW(candidate,type,int(std::size(type)));
        if(!hook_attempted.load()&&candidate&&process==GetCurrentProcessId()&&std::wstring_view(type).starts_with(L"GLFW")){
            hook_attempted.store(true);HMODULE module=nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&observe_message),&module);
            auto previous=observer.exchange(nullptr);if(previous)UnhookWindowsHookEx(previous);
            auto queued=queued_observer.exchange(nullptr);if(queued)UnhookWindowsHookEx(queued);
            window.store(candidate);observer.store(SetWindowsHookExW(WH_CALLWNDPROC,observe_message,module,thread));
            queued_observer.store(SetWindowsHookExW(WH_GETMESSAGE,observe_queued,module,thread));
            bool installed=observer.load()&&queued_observer.load();
            if(!installed){
                auto sent=observer.exchange(nullptr);if(sent)UnhookWindowsHookEx(sent);
                auto posted=queued_observer.exchange(nullptr);if(posted)UnhookWindowsHookEx(posted);
                window.store(nullptr);
            }
            window_ready(installed);focus(installed&&GetForegroundWindow()==candidate);
        }
    }
    if(auto current=window.load())focus(GetForegroundWindow()==current);
    finish_frame();
}
void native_observe(void* object,uintptr_t game){
    if(!requested())return;
    auto table=*static_cast<uintptr_t*>(object);if(!may_observe(table))return;
    if(!*reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(object)+0x1c))return;
    auto* focused_element=*reinterpret_cast<void**>(game+0x188);
    bool text_entry=focused_element&&(native_object_is_a(focused_element,"mm_ui_element_editbox")||native_object_is_a(focused_element,"c_ui_element_editbox"));
    observe(reinterpret_cast<uintptr_t>(object),table,text_entry);
}
void native_forget(void* object){if(requested())forget(reinterpret_cast<uintptr_t>(object));}
}
