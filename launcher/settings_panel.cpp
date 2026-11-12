#include "settings_panel.h"
#include "settings_store.h"
#include "settings_schema.h"
#include "mod_manifest.h"
#include <commctrl.h>
#include <cmath>
#include <fstream>
#include <memory>

namespace {
struct Item {std::wstring section,key,initial,description;bool checkbox=false,heading=false;HWND label{},control{};const swml_settings::Field* schema=nullptr;};
struct State {HFONT font{};HWND tooltip{};std::wstring dir;std::vector<Item> items;std::vector<swml_settings::Field> schema;int scroll=0,row=30,dpi=96;bool loading=false;};
int scale(State& s,int px){return MulDiv(px,s.dpi,96);}
std::string bytes(const std::wstring& path){std::ifstream stream(std::filesystem::path(path),std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
std::wstring trim(std::wstring s){auto begin=s.find_first_not_of(L" \t\r");if(begin==s.npos)return {};return s.substr(begin,s.find_last_not_of(L" \t\r")-begin+1);}
void position(HWND window,State& s){
    RECT r;GetClientRect(window,&r);int height=int(s.items.size())*s.row+scale(s,18);s.scroll=std::clamp(s.scroll,0,std::max(0,height-int(r.bottom)));
    SCROLLINFO info{sizeof info,SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,height-1),UINT(r.bottom),s.scroll};SetScrollInfo(window,SB_VERT,&info,TRUE);
    for(size_t i=0;i<s.items.size();++i){auto& item=s.items[i];int y=scale(s,8)+int(i)*s.row-s.scroll;
        if(item.heading)MoveWindow(item.label,scale(s,10),y+scale(s,5),std::max(10,int(r.right)-scale(s,20)),s.row-2,TRUE);
        else if(item.checkbox)MoveWindow(item.control,scale(s,10),y,std::max(10,int(r.right)-scale(s,20)),s.row-2,TRUE);
        else {int width=std::max(scale(s,60),int(r.right)*55/100);MoveWindow(item.label,scale(s,10),y+scale(s,5),width-scale(s,14),s.row-2,TRUE);MoveWindow(item.control,width,y,std::max(scale(s,30),int(r.right)-width-scale(s,12)),s.row-3,TRUE);}
    }InvalidateRect(window,nullptr,TRUE);
}
void save(HWND window,State& s,size_t index){
    if(s.loading||index>=s.items.size()||s.items[index].heading)return;auto& item=s.items[index];
    try {
        std::wstring value;
        if(item.checkbox)value=SendMessageW(item.control,BM_GETCHECK,0,0)==BST_CHECKED?L"1":L"0";
        else {int n=GetWindowTextLengthW(item.control);if(n>4096)throw std::runtime_error("setting value too long");value.resize(n+1);GetWindowTextW(item.control,value.data(),n+1);value.resize(n);}
        if(item.schema){auto decoded=swml_settings::decode(*item.schema,swml_manifest::utf8(value));value=swml_manifest::wide(swml_settings::encode(*item.schema,decoded));}
        else if(!item.checkbox){wchar_t* end=nullptr;std::wcstod(item.initial.c_str(),&end);if(end!=item.initial.c_str()&&!*end){double number=std::wcstod(value.c_str(),&end);if(end==value.c_str()||*end||!std::isfinite(number))throw std::runtime_error("enter a finite numeric value");}}
        swml_settings_store::set(s.dir,item.section,item.key,value,&item.initial);
    }catch(const std::exception& error){
        try {auto values=swml_settings_store::merged(s.dir);auto found=values.find({item.section,item.key});auto value=found==values.end()?item.initial:found->second;s.loading=true;if(item.checkbox)SendMessageW(item.control,BM_SETCHECK,(value==L"1"||value==L"true")?BST_CHECKED:BST_UNCHECKED,0);else SetWindowTextW(item.control,value.c_str());s.loading=false;}catch(...){s.loading=false;}
        MessageBoxW(window,swml_manifest::wide(error.what()).c_str(),L"Could not save setting",MB_ICONERROR);
    }
}
void tooltip(State& s,HWND window,std::wstring& text){TOOLINFOW tool{sizeof tool};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=GetParent(window);tool.uId=reinterpret_cast<UINT_PTR>(window);tool.lpszText=text.data();SendMessageW(s.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));}
void load(HWND window,State& s,const std::wstring& dir){
    s.loading=true;for(auto& item:s.items){DestroyWindow(item.label);DestroyWindow(item.control);}s.items.clear();s.schema.clear();s.dir=dir;s.scroll=0;
    DestroyWindow(s.tooltip);s.tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,window,nullptr,nullptr,nullptr);
    SendMessageW(s.tooltip,TTM_SETDELAYTIME,TTDT_INITIAL,1000);SendMessageW(s.tooltip,TTM_SETDELAYTIME,TTDT_RESHOW,1000);SendMessageW(s.tooltip,TTM_SETMAXTIPWIDTH,0,scale(s,440));
    try {
        if(dir.empty()){s.loading=false;return;}
        swml_settings_store::prepare(dir);
        auto schema=bytes(dir+L"\\settings.schema.json");if(!schema.empty())s.schema=swml_settings::parse(swml_json::parse(schema));
        // INI comments supply descriptions for existing schema-free mods.
        std::map<swml_settings_store::Key,std::wstring> descriptions;
        auto source=swml_manifest::wide(bytes(dir+L"\\settings.default.ini"));std::wstring section,description;
        size_t start=0;while(start<source.size()){auto end=source.find(L'\n',start);if(end==source.npos)end=source.size();auto line=trim(source.substr(start,end-start));start=end+1;
            if(line.starts_with(L";")||line.starts_with(L"#")){if(!description.empty())description+=L" ";description+=trim(line.substr(1));}
            else if(line.starts_with(L"[")){auto close=line.find(L']');section=swml_settings_store::lower(line.substr(1,close-1));description.clear();}
            else if(auto equal=line.find(L'=');equal!=line.npos){descriptions[{section,swml_settings_store::lower(trim(line.substr(0,equal)))}]=description;description.clear();}
        }
        auto base=swml_settings_store::defaults(dir),current=swml_settings_store::merged(dir);
        for(auto& field:s.schema){auto id=swml_settings_store::Key{swml_settings_store::lower(swml_manifest::wide(field.section)),swml_settings_store::lower(swml_manifest::wide(field.key))};if(!base.contains(id))base[id]=swml_manifest::wide(swml_settings::encode(field,field.initial));}
        std::wstring previous_section;
        for(const auto& [id,value]:base){if(id.first!=previous_section){Item heading;heading.heading=true;heading.key=L"["+id.first+L"]";s.items.push_back(std::move(heading));previous_section=id.first;}Item item;item.section=id.first;item.key=id.second;item.initial=value;item.description=descriptions[id];
            for(auto& field:s.schema)if(swml_settings_store::lower(swml_manifest::wide(field.section))==id.first&&swml_settings_store::lower(swml_manifest::wide(field.key))==id.second){item.schema=&field;item.description=swml_manifest::wide(field.description);break;}
            // Ambiguous numeric 0/1 settings stay editable rather than becoming switches.
            auto lower=swml_settings_store::lower(item.description);
            // A key ending in a unit (isolate_s, send_rate_kb, ...) is a quantity even when its default is 0 or 1: as a
            // checkbox it could only ever be saved as 1 (isolate_s=1 = a new stage every second).
            auto unit_suffix=[](const std::wstring& key){for(auto u:{L"_s",L"_ms",L"_m",L"_cm",L"_mm",L"_km",L"_kb",L"_mb",L"_gb",L"_hz",L"_n",L"_pct",L"_deg",L"_px"}){std::wstring_view s(u);if(key.size()>s.size()&&key.compare(key.size()-s.size(),s.size(),s)==0)return true;}return false;};
            item.checkbox=item.schema?item.schema->type=="bool":(value==L"0"||value==L"1")&&
                (lower.find(L"1 =")!=lower.npos||lower.find(L"1=")!=lower.npos||lower.find(L"0 = off")!=lower.npos||item.key==L"enabled")&&lower.find(L"0 = auto")==lower.npos&&
                id.second.find(L"threads")==id.second.npos&&id.second.find(L"ticks")==id.second.npos&&!unit_suffix(id.second)&&lower.find(L"2 =")==lower.npos&&lower.find(L"2=")==lower.npos;
            s.items.push_back(std::move(item));
        }
        // Items are no longer moved after tooltip text pointers are registered.
        if(s.items.empty()){Item empty;empty.heading=true;empty.key=L"This mod has no settings.";s.items.push_back(std::move(empty));}
        for(size_t i=0;i<s.items.size();++i){auto& item=s.items[i];auto label=item.key;
            if(item.heading){item.label=CreateWindowW(L"STATIC",label.c_str(),WS_CHILD|WS_VISIBLE,0,0,10,10,window,nullptr,nullptr,nullptr);SendMessageW(item.label,WM_SETFONT,reinterpret_cast<WPARAM>(s.font),TRUE);continue;}
            if(!item.checkbox)item.label=CreateWindowW(L"STATIC",label.c_str(),WS_CHILD|WS_VISIBLE,0,0,10,10,window,nullptr,nullptr,nullptr);
            item.control=CreateWindowExW(item.checkbox?0:WS_EX_CLIENTEDGE,item.checkbox?L"BUTTON":L"EDIT",item.checkbox?label.c_str():L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|(item.checkbox?BS_AUTOCHECKBOX:ES_AUTOHSCROLL),0,0,10,10,window,reinterpret_cast<HMENU>(INT_PTR(1000+i)),nullptr,nullptr);
            for(HWND control:{item.label,item.control})if(control)SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(s.font),TRUE);
            auto found=current.find({item.section,item.key});auto value=found==current.end()?item.initial:found->second;
            if(item.checkbox)SendMessageW(item.control,BM_SETCHECK,(value==L"1"||value==L"true")?BST_CHECKED:BST_UNCHECKED,0);else {SetWindowTextW(item.control,value.c_str());SendMessageW(item.control,EM_SETLIMITTEXT,4096,0);}
            item.description=L"["+item.section+L"] "+item.key+L"\r\n"+item.description+L"\r\nDefault: "+item.initial+L"\r\nSaved immediately; takes effect when the game next starts.";
            tooltip(s,item.control,item.description);if(item.label)tooltip(s,item.label,item.description);
        }
    }catch(const std::exception& error){MessageBoxW(window,swml_manifest::wide(error.what()).c_str(),L"Could not load settings",MB_ICONERROR);}
    s.loading=false;position(window,s);
}
LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM wp,LPARAM lp){
    auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(msg==WM_NCCREATE){s=new State;s->font=reinterpret_cast<HFONT>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);s->dpi=std::max(96,int(GetDpiForWindow(window)));s->row=scale(*s,30);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
    if(!s)return DefWindowProcW(window,msg,wp,lp);
    switch(msg){
    case WM_SIZE:position(window,*s);return 0;
    case WM_MOUSEWHEEL:s->scroll-=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*s->row*3;position(window,*s);return 0;
    case WM_VSCROLL:{SCROLLINFO info{sizeof info,SIF_ALL};GetScrollInfo(window,SB_VERT,&info);switch(LOWORD(wp)){case SB_LINEUP:s->scroll-=s->row;break;case SB_LINEDOWN:s->scroll+=s->row;break;case SB_PAGEUP:s->scroll-=int(info.nPage);break;case SB_PAGEDOWN:s->scroll+=int(info.nPage);break;case SB_THUMBTRACK:s->scroll=info.nTrackPos;break;}position(window,*s);return 0;}
    case WM_COMMAND:{size_t i=size_t(LOWORD(wp))-1000;if(i<s->items.size()&&((s->items[i].checkbox&&HIWORD(wp)==BN_CLICKED)||(!s->items[i].checkbox&&HIWORD(wp)==EN_KILLFOCUS)))save(window,*s,i);return 0;}
    case WM_NCDESTROY:DestroyWindow(s->tooltip);delete s;SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
    }return DefWindowProcW(window,msg,wp,lp);
}
}
HWND create_settings_panel(HWND parent,HFONT font){WNDCLASSW c{};c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"SWMLInlineSettings";c.lpfnWndProc=procedure;c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&c);return CreateWindowExW(WS_EX_CLIENTEDGE,c.lpszClassName,L"",WS_CHILD|WS_VSCROLL|WS_CLIPCHILDREN,0,0,10,10,parent,nullptr,c.hInstance,font);}
void select_settings_panel(HWND panel,const std::wstring& dir){auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(panel,GWLP_USERDATA));if(s)load(panel,*s,dir);}
