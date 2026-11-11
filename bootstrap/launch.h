#pragma once
#include "contract.h"
#include <map>
#include <string>
#include <vector>
namespace swml_bootstrap {
struct CaseLess {bool operator()(const std::wstring& a,const std::wstring& b)const{return _wcsicmp(a.c_str(),b.c_str())<0;}};
inline std::vector<wchar_t> child_environment(const std::wstring& root,bool mods,const std::wstring& event){
    std::map<std::wstring,std::wstring,CaseLess> values;
    wchar_t* block=GetEnvironmentStringsW();if(!block)throw std::runtime_error("cannot read launch environment");
    try {for(const wchar_t* p=block;*p;p+=wcslen(p)+1){std::wstring entry=p;auto equal=entry.find(L'=',entry.starts_with(L'=')?1:0);if(equal!=entry.npos)values[entry.substr(0,equal)]=entry.substr(equal+1);}}catch(...){FreeEnvironmentStringsW(block);throw;}
    FreeEnvironmentStringsW(block);values[L"SWML_ROOT"]=root;values[L"SWML_MODE"]=mods?L"1":L"0";values[L"SWML_READY_EVENT"]=event;
    std::vector<wchar_t> result;for(const auto& [key,value]:values){auto entry=key+L"="+value;result.insert(result.end(),entry.begin(),entry.end());result.push_back(0);}result.push_back(0);return result;
}
}
