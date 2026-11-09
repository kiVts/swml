#include "settings_editor.h"
#include "settings_schema.h"
#include "mod_manifest.h"
#include <fstream>
#include <iterator>

namespace {
struct State {
    std::vector<swml_settings::Field> fields;
    std::wstring settings;
    HWND list{},value{},choices{},description{};
    bool done=false;
    int selected=-1;
};
void choose(State& state) {
    int index=int(SendMessageW(state.list,LB_GETCURSEL,0,0));state.selected=index;
    if(index<0||size_t(index)>=state.fields.size())return;
    const auto& field=state.fields[size_t(index)];
    wchar_t buffer[8192]{};auto initial=swml_manifest::wide(field.initial.is_string()?field.initial.string():swml_json::dump(field.initial));
    GetPrivateProfileStringW(swml_manifest::wide(field.section).c_str(),swml_manifest::wide(field.key).c_str(),initial.c_str(),buffer,DWORD(std::size(buffer)),state.settings.c_str());
    bool dropdown=field.type=="bool"||field.type=="enum";
    ShowWindow(state.value,dropdown?SW_HIDE:SW_SHOW);ShowWindow(state.choices,dropdown?SW_SHOW:SW_HIDE);
    if(dropdown) {
        SendMessageW(state.choices,CB_RESETCONTENT,0,0);
        std::vector<std::string> values=field.type=="bool"?std::vector<std::string>{"false","true"}:field.choices;
        std::string current=swml_manifest::utf8(buffer);
        if(field.type=="bool")try{current=swml_json::dump(swml_settings::decode(field,current));}catch(...){}
        for(const auto& item:values){auto text=swml_manifest::wide(item);auto n=SendMessageW(state.choices,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));if(item==current)SendMessageW(state.choices,CB_SETCURSEL,n,0);}
    }else SetWindowTextW(state.value,buffer);
    auto detail=field.description+"\r\n\r\nType: "+field.type+(field.restart?"; restart required":"")+(field.authority=="host"?"; host controlled":"");
    if(field.type=="int"||field.type=="number")detail+="\r\nRange: "+swml_json::dump(field.minimum)+" to "+swml_json::dump(field.maximum);
    SetWindowTextW(state.description,swml_manifest::wide(detail).c_str());
}
void apply(HWND window,State& state,bool defaults) {
    if(state.selected<0||size_t(state.selected)>=state.fields.size())return;
    const auto& field=state.fields[size_t(state.selected)];
    try {
        swml_json::Value value=field.initial;
        if(!defaults){
            auto control=field.type=="bool"||field.type=="enum"?state.choices:state.value;
            wchar_t text[8192]{};GetWindowTextW(control,text,int(std::size(text)));value=swml_settings::decode(field,swml_manifest::utf8(text));
        }
        auto encoded=swml_manifest::wide(swml_settings::encode(field,value));
        if(!WritePrivateProfileStringW(swml_manifest::wide(field.section).c_str(),swml_manifest::wide(field.key).c_str(),encoded.c_str(),state.settings.c_str()))throw std::runtime_error("could not save setting");
        choose(state);
    }catch(const std::exception& e){MessageBoxW(window,swml_manifest::wide(e.what()).c_str(),L"Invalid setting",MB_ICONERROR);}
}
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){state=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}
    if(state)switch(message){
    case WM_CREATE:{
        auto control=[&](const wchar_t* type,const wchar_t* text,DWORD style,int x,int y,int width,int height,int id){
            auto child=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS|style,x,y,width,height,window,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
            if(swml_manifest::wine()){
                using Theme=HRESULT(WINAPI*)(HWND,LPCWSTR,LPCWSTR);static auto module=LoadLibraryW(L"uxtheme.dll");static auto theme=module?reinterpret_cast<Theme>(GetProcAddress(module,"SetWindowTheme")):nullptr;if(theme)theme(child,L"",L"");
            }
            return child;
        };
        state->list=control(L"LISTBOX",L"",LBS_NOTIFY|WS_BORDER|WS_VSCROLL,12,12,210,320,100);
        state->value=control(L"EDIT",L"",ES_AUTOHSCROLL|WS_BORDER,238,14,380,28,101);SendMessageW(state->value,EM_SETLIMITTEXT,4096,0);
        state->choices=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,238,14,380,240,102);
        state->description=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,238,58,380,220,103);
        control(L"BUTTON",L"Apply value",BS_PUSHBUTTON,238,295,110,30,IDOK);
        control(L"BUTTON",L"Restore default",BS_PUSHBUTTON,358,295,125,30,104);
        control(L"BUTTON",L"Close",BS_PUSHBUTTON,493,295,125,30,IDCANCEL);
        for(const auto& field:state->fields){auto label=swml_manifest::wide(field.category+" / "+field.id);SendMessageW(state->list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
        SendMessageW(state->list,LB_SETCURSEL,0,0);choose(*state);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==100&&HIWORD(wp)==LBN_SELCHANGE)choose(*state);
        else if(LOWORD(wp)==IDOK)apply(window,*state,false);
        else if(LOWORD(wp)==104)apply(window,*state,true);
        else if(LOWORD(wp)==IDCANCEL)DestroyWindow(window);
        return 0;
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:state->done=true;return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
bool edit_schema_settings(HWND owner,const std::wstring& schema_file,const std::wstring& settings_file){
    if(GetFileAttributesW(schema_file.c_str())==INVALID_FILE_ATTRIBUTES)return false;
    try {
        std::ifstream file(schema_file,std::ios::binary);std::string json;char buffer[4096];
        while(file){file.read(buffer,sizeof(buffer));json.append(buffer,size_t(file.gcount()));if(json.size()>65535)throw std::runtime_error("settings schema exceeds size limit");}
        State state;state.fields=swml_settings::parse(swml_json::parse(json));state.settings=settings_file;
        auto instance=GetModuleHandleW(nullptr);WNDCLASSW type{};type.lpfnWndProc=procedure;type.hInstance=instance;type.lpszClassName=L"swml_schema_settings";type.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);type.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&type);
        RECT parent{};GetWindowRect(owner,&parent);auto window=CreateWindowExW(WS_EX_DLGMODALFRAME,type.lpszClassName,L"Mod settings (apply at next start)",WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,parent.left+30,parent.top+50,650,390,owner,nullptr,instance,&state);
        if(!window)throw std::runtime_error("could not create settings window");
        EnableWindow(owner,FALSE);ShowWindow(window,SW_SHOW);RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
        MSG message{};int result=1;
        while(!state.done&&(result=GetMessageW(&message,nullptr,0,0))>0)if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        if(!state.done)DestroyWindow(window);EnableWindow(owner,TRUE);SetForegroundWindow(owner);if(result==0)PostQuitMessage(int(message.wParam));
    }catch(const std::exception& e){MessageBoxW(owner,swml_manifest::wide(e.what()).c_str(),L"Settings schema error",MB_ICONERROR);}
    return true;
}
