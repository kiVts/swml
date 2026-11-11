#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <vector>
namespace swml_bootstrap {
// Inspect exports without running DllMain: never probe an old auto-start core by loading it.
inline bool exported(const std::wstring& path,const char* wanted){
    try {
        std::ifstream file(std::filesystem::path(path),std::ios::binary|std::ios::ate);auto size=file.tellg();if(size<256||size>64*1024*1024)return false;
        std::vector<unsigned char> bytes(static_cast<size_t>(size));file.seekg(0);if(!file.read(reinterpret_cast<char*>(bytes.data()),size))return false;
        auto valid=[&](size_t at,size_t n){return at<=bytes.size()&&n<=bytes.size()-at;};
        auto u16=[&](size_t at){WORD v=0;if(!valid(at,2))throw 1;memcpy(&v,bytes.data()+at,2);return v;};
        auto u32=[&](size_t at){DWORD v=0;if(!valid(at,4))throw 1;memcpy(&v,bytes.data()+at,4);return v;};
        if(u16(0)!=IMAGE_DOS_SIGNATURE)return false;size_t pe=u32(60),opt=pe+24;
        if(u32(pe)!=IMAGE_NT_SIGNATURE||u16(pe+4)!=IMAGE_FILE_MACHINE_AMD64||u16(opt)!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return false;
        size_t sections=opt+u16(pe+20);unsigned count=u16(pe+6);if(count>96||!valid(sections,count*40))return false;
        auto raw=[&](DWORD rva)->size_t{for(unsigned i=0;i<count;++i){size_t s=sections+40*i;DWORD start=u32(s+12),n=u32(s+16);if(rva>=start&&rva-start<n){size_t off=size_t(u32(s+20))+(rva-start);if(!valid(off,1))throw 1;return off;}}throw 1;};
        size_t table=raw(u32(opt+112));DWORD n=u32(table+24);if(n>65536)return false;size_t names=raw(u32(table+32));
        for(DWORD i=0;i<n;++i){size_t name=raw(u32(names+4*i));size_t length=strlen(wanted)+1;if(valid(name,length)&&!memcmp(bytes.data()+name,wanted,length))return true;}
    }catch(...){}return false;
}
inline std::wstring environment(const wchar_t* name){DWORD n=GetEnvironmentVariableW(name,nullptr,0);if(!n||n>32768)return {};std::wstring value(n,L'\0');DWORD got=GetEnvironmentVariableW(name,value.data(),n);if(!got||got>=n)return {};value.resize(got);return value;}
}
