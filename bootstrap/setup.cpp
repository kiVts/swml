// Local bootstrap installation only. No game launch, security setting changes,
// executable patching, unknown DLL replacement or recursive deletion.
#include "contract.h"
#include "mod_manifest.h"
#include "json.h"
#include "steam_install.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <chrono>
#include <sstream>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
namespace fs=std::filesystem;
using swml_json::Value;
namespace {
std::string bytes(const fs::path& path){std::ifstream f(path,std::ios::binary|std::ios::ate);auto n=f.tellg();if(n<0||n>128*1024*1024)throw std::runtime_error("Cannot read file or file too large");std::string out(size_t(n),'\0');f.seekg(0);if(!f.read(out.data(),n))throw std::runtime_error("Cannot read file");return out;}
std::string hash(const fs::path& path){
    if(!fs::exists(path))return {};
    if(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("File links are not accepted");
    auto data=bytes(path);BCRYPT_ALG_HANDLE algorithm=nullptr;unsigned char digest[32]{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 unavailable");
    auto result=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(data.data()),ULONG(data.size()),digest,32);BCryptCloseAlgorithmProvider(algorithm,0);
    if(result<0)throw std::runtime_error("SHA256 failed");const char* digits="0123456789abcdef";std::string out;for(auto c:digest){out+=digits[c>>4];out+=digits[c&15];}return out;
}
void atomic_copy(const fs::path& source,const fs::path& target,bool replace=true){
    fs::path temporary=target;temporary+=L".swml-startup-"+std::to_wstring(GetCurrentProcessId())+L".tmp";
    const auto expected=hash(source);fs::copy_file(source,temporary); // no overwrite of an existing temporary file
    try {
        if(hash(temporary)!=expected)throw std::runtime_error("Temporary copy failed checksum verification");
        if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0)))throw std::runtime_error("Cannot replace file; check folder permissions and close the game/loader");
    }catch(...){if(hash(temporary)==expected)fs::remove(temporary);throw;}
}
std::wstring wide(const std::string& s){return swml_manifest::wide(s);}
std::string text(const fs::path& p){return swml_manifest::utf8(p.wstring());}
std::string field(const Value& value,const char* name){const auto* p=value.find(name);if(!p||!p->is_string())throw std::runtime_error("Invalid installation record");return p->string();}
void regular_directory(const fs::path& dir){for(auto path=dir;!path.empty();){if(fs::exists(path)&&(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Directory links are not accepted");auto next=path.parent_path();if(next==path)break;path=next;}}
void stopped(){
#ifndef SWML_BOOTSTRAP_TEST
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot check running game processes");PROCESSENTRY32W entry{sizeof entry};bool running=false;for(BOOL ok=Process32FirstW(snapshot,&entry);ok;ok=Process32NextW(snapshot,&entry))if(!_wcsicmp(entry.szExeFile,L"swml.exe")||!_wcsicmp(entry.szExeFile,L"stormworks64.exe")||!_wcsicmp(entry.szExeFile,L"server64.exe"))running=true;CloseHandle(snapshot);if(running)throw std::runtime_error("Close Stormworks and SWML first");
#endif
}
void journal(const fs::path& path,const Value& record){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create installation record; no files replaced");auto data=swml_json::dump(record);DWORD n=0;bool ok=WriteFile(file,data.data(),DWORD(data.size()),&n,nullptr)&&n==data.size()&&FlushFileBuffers(file);CloseHandle(file);if(!ok)throw std::runtime_error("Cannot save recovery record");
}
void perform(const std::wstring& action,const fs::path& source,fs::path game,fs::path loader){
    if(swml_manifest::wine())throw std::runtime_error("Use install_linux.sh for Proton");
    stopped();game=fs::absolute(game).lexically_normal();loader=fs::absolute(loader).lexically_normal();regular_directory(game);regular_directory(loader);
    if(!fs::is_regular_file(game/L"stormworks64.exe"))throw std::runtime_error("Choose the Stormworks client folder containing stormworks64.exe");
    const auto proxy=game/L"opengl32.dll",marker=game/L"swml-startup-install.json";
    if(action!=L"--install"){
        auto record=swml_json::parse(bytes(marker));if(field(record,"kind")!="swml-startup-v1"||_wcsicmp(wide(field(record,"game")).c_str(),game.c_str())||_wcsicmp(wide(field(record,"loader")).c_str(),loader.c_str()))throw std::runtime_error("Installation record does not match these folders");
        if(fs::exists(proxy)&&hash(proxy)!=field(record,"proxyHash"))throw std::runtime_error("OpenGL wrapper changed since installation; nothing removed");
        fs::path backup=fs::absolute(wide(field(record,"backup"))).lexically_normal();auto expected=loader/L"backups"/L"startup-transition";
        auto relative=backup.lexically_relative(expected);if(relative.empty()||relative.is_absolute()||*relative.begin()==L"..")throw std::runtime_error("Invalid backup directory");regular_directory(backup);
        const auto* previous=record.find("previous");const auto* installed=record.find("installed");if(!previous||!installed)throw std::runtime_error("Missing recovery hashes");
        if(action==L"--rollback")for(auto name:{"swml.exe","swml_core.dll"}){
            const auto old=field(*previous,name),current=hash(loader/wide(name));
            const auto* replaced=record.find("replaced");
            if(current!=old&&current!=field(*installed,name)&&(!replaced||current!=field(*replaced,name)))throw std::runtime_error("Loader files changed since installation; refusing to overwrite them");
            if(!old.empty()&&hash(backup/wide(name))!=old)throw std::runtime_error("Backup failed checksum verification");
        }
        // All paths/ownership/rollback prerequisites checked before any removal.
        if(fs::exists(proxy))fs::remove(proxy);
        if(action==L"--rollback")for(auto name:{"swml.exe","swml_core.dll"}){
            auto target=loader/wide(name);if(field(*previous,name).empty()){if(fs::exists(target))fs::remove(target);}else atomic_copy(backup/wide(name),target);
        }
        fs::remove(marker);return;
    }
    Value prior;const bool updating=fs::exists(marker);
    if(updating){
        prior=swml_json::parse(bytes(marker));
        if(field(prior,"kind")!="swml-startup-v1"||_wcsicmp(wide(field(prior,"game")).c_str(),game.c_str())||_wcsicmp(wide(field(prior,"loader")).c_str(),loader.c_str())||hash(proxy)!=field(prior,"proxyHash"))throw std::runtime_error("Existing startup installation does not match; nothing replaced");
        const auto* installed=prior.find("installed");if(!installed||!prior.find("previous"))throw std::runtime_error("Invalid installation record");
        for(auto name:{"swml.exe","swml_core.dll"})if(hash(loader/wide(name))!=field(*installed,name))throw std::runtime_error("Loader files changed since installation; nothing replaced");
    }else if(fs::exists(proxy))throw std::runtime_error("Another OpenGL wrapper already exists. Existing wrappers are never replaced");
    auto manifest=swml_json::parse(bytes(source/L"startup-manifest.json"));const auto* files=manifest.find("files");if(!files)throw std::runtime_error("Missing kit manifest");
    Value::Object installed,previous;
    for(auto name:{"swml.exe","swml_core.dll","opengl32.dll"})if(hash(source/L"startup"/wide(name))!=field(*files,name))throw std::runtime_error("Kit checksum mismatch");
    if(!swml_bootstrap::exported((source/L"startup"/L"swml_core.dll").wstring(),"swml_bootstrap_start_v1")||!swml_bootstrap::exported((source/L"startup"/L"opengl32.dll").wstring(),"swml_bootstrap_version"))throw std::runtime_error("Incompatible bootstrap/core pair");
    fs::create_directories(loader);auto backup=loader/L"backups"/L"startup-transition"/std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());fs::create_directories(backup);
    for(auto name:{"swml.exe","swml_core.dll"}){auto old=hash(loader/wide(name));previous[name]=old;installed[name]=field(*files,name);if(!old.empty())fs::copy_file(loader/wide(name),backup/wide(name));}
    Value record=Value::Object{{"kind","swml-startup-v1"},{"game",text(game)},{"loader",text(loader)},{"backup",text(backup)},{"proxyHash",field(*files,"opengl32.dll")},{"previous",previous},{"installed",installed}};
    if(updating){
        fs::copy_file(proxy,backup/L"opengl32.dll");fs::copy_file(marker,backup/L"record.json");
        record=Value::Object{{"kind","swml-startup-v1"},{"game",text(game)},{"loader",text(loader)},{"backup",field(prior,"backup")},{"proxyHash",field(*files,"opengl32.dll")},{"previous",*prior.find("previous")},{"installed",installed},{"replaced",previous}};
        journal(backup/L"next-record.json",record);atomic_copy(backup/L"next-record.json",marker);
    }else journal(marker,record);
    try {
        for(auto name:{"swml.exe","swml_core.dll"})atomic_copy(source/L"startup"/wide(name),loader/wide(name));
        atomic_copy(source/L"startup"/L"opengl32.dll",proxy,updating); // replace only a verified owned wrapper
        for(auto name:{"swml.exe","swml_core.dll"})if(hash(loader/wide(name))!=field(*files,name))throw std::runtime_error("Installed checksum mismatch");
    }catch(...){
        // Recovery remains recorded even if an interrupted restore itself fails.
        try{if(hash(proxy)==field(record,"proxyHash"))fs::remove(proxy);for(auto name:{"swml.exe","swml_core.dll"}){auto old=field(Value(previous),name);auto target=loader/wide(name);if(!old.empty())atomic_copy(backup/wide(name),target);else if(hash(target)==field(*files,name))fs::remove(target);}if(updating){atomic_copy(backup/L"opengl32.dll",proxy,false);atomic_copy(backup/L"record.json",marker);}}catch(...){}
        throw;
    }
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    try {
        wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);fs::path source=fs::path(module).parent_path();
        fs::path game;
        PWSTR local=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))throw std::runtime_error("Cannot find local app data");fs::path loader=fs::path(local)/L"swml";CoTaskMemFree(local);
        int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)throw std::runtime_error("Invalid arguments");std::wstring action=L"--install";bool explicit_game=false;
        for(int i=1;i<argc;++i){std::wstring arg=argv[i];if(arg==L"--install"||arg==L"--remove"||arg==L"--rollback")action=arg;else if(arg==L"--game"&&i+1<argc){game=argv[++i];explicit_game=true;}else if(arg==L"--loader"&&i+1<argc)loader=argv[++i];else {LocalFree(argv);throw std::runtime_error("Invalid command arguments");}}LocalFree(argv);
        CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(!explicit_game){auto installed=swml_steam_install::discover(swml_steam_install::roots());if(installed.size()==1){game=installed.front();explicit_game=true;}}
        if(!explicit_game){BROWSEINFOW browse{};browse.lpszTitle=L"Select the Stormworks folder containing stormworks64.exe";browse.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;auto item=SHBrowseForFolderW(&browse);if(!item)return 2;wchar_t selected[32768]{};bool ok=SHGetPathFromIDListW(item,selected)!=FALSE;CoTaskMemFree(item);if(!ok)throw std::runtime_error("Cannot read selected folder");game=selected;}
        if(MessageBoxW(nullptr,(L"Update SWML files?\n\nGame: "+game.wstring()+L"\nLoader: "+loader.wstring()+L"\n\nClose the game and SWML first. Mods/settings are preserved.").c_str(),L"SWML setup",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 2;
        perform(action,source,game,loader);MessageBoxW(nullptr,L"Operation completed. No game was launched.",L"SWML setup",MB_OK);return 0;
    }catch(const std::exception& e){MessageBoxW(nullptr,wide(e.what()).c_str(),L"SWML startup setup failed",MB_ICONERROR);return 1;}
}
