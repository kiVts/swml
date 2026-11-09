#include "field_dialog.h"
#include <vector>
namespace {
struct State { HWND edit{}; bool done=false, accepted=false; std::wstring value; };
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) { state=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state)); }
    if (state) switch (message) {
    case WM_CREATE: {
        state->edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",state->value.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,12,12,360,25,window,reinterpret_cast<HMENU>(100),nullptr,nullptr);
        SendMessageW(state->edit,EM_SETLIMITTEXT,4096,0);
        auto okay=CreateWindowW(L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,190,52,85,28,window,reinterpret_cast<HMENU>(IDOK),nullptr,nullptr);
        auto cancel=CreateWindowW(L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,287,52,85,28,window,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
        for (auto control : {state->edit,okay,cancel}) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp)==IDOK) {
            std::vector<wchar_t> text(size_t(GetWindowTextLengthW(state->edit))+1);
            GetWindowTextW(state->edit,text.data(),int(text.size())); state->value=text.data(); state->accepted=true; DestroyWindow(window); return 0;
        }
        if (LOWORD(wp)==IDCANCEL) { DestroyWindow(window); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: state->done=true; return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
std::optional<std::wstring> prompt_value(HWND owner, const wchar_t* title, const std::wstring& initial) {
    auto instance=GetModuleHandleW(nullptr); WNDCLASSW type{}; type.lpfnWndProc=procedure; type.hInstance=instance;
    type.lpszClassName=L"swml_value_dialog"; type.hbrBackground=GetSysColorBrush(COLOR_BTNFACE); type.hCursor=LoadCursor(nullptr,IDC_ARROW); RegisterClassW(&type);
    State state; state.value=initial; RECT parent{}; GetWindowRect(owner,&parent);
    auto window=CreateWindowExW(WS_EX_DLGMODALFRAME,type.lpszClassName,title,WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,parent.left+50,parent.top+60,400,135,owner,nullptr,instance,&state);
    if (!window) return {};
    EnableWindow(owner,FALSE); ShowWindow(window,SW_SHOW); SetFocus(state.edit); SendMessageW(state.edit,EM_SETSEL,0,-1);
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    MSG message{}; int result=1;
    while (!state.done && (result=GetMessageW(&message,nullptr,0,0))>0) if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (!state.done) DestroyWindow(window);
    EnableWindow(owner,TRUE); SetForegroundWindow(owner);
    if (result==0) PostQuitMessage(int(message.wParam));
    return state.accepted?std::optional(state.value):std::nullopt;
}
