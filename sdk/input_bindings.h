#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <utility>
#include <map>
#include <optional>
#include <string>
#include <string_view>
namespace swml_input {
struct Binding {uint32_t key=0,modifiers=0;};
inline std::string lower(std::string text){for(auto& c:text)if(c>='A'&&c<='Z')c=char(c+'a'-'A');return text;}
inline const std::map<std::string,uint32_t>& keys(){
    static const std::map<std::string,uint32_t> names{{"mouse1",1},{"mouse2",2},{"mouse3",4},{"mouse4",5},{"mouse5",6},
        {"backspace",8},{"tab",9},{"enter",13},{"escape",27},{"space",32},{"pageup",33},{"pagedown",34},{"end",35},{"home",36},
        {"left",37},{"up",38},{"right",39},{"down",40},{"insert",45},{"delete",46},{"plus",187},{"minus",189},{"comma",188},{"period",190}};
    return names;
}
inline std::optional<Binding> parse(std::string_view text){
    if(text.empty()||text.size()>95)return {};
    Binding result;size_t at=0;bool have_key=false;
    do{
        auto end=text.find('+',at);std::string token=lower(std::string(text.substr(at,end-at)));
        uint32_t modifier=token=="ctrl"?1u:token=="shift"?2u:token=="alt"?4u:token=="super"?8u:0u;
        if(modifier){if(result.modifiers&modifier)return {};result.modifiers|=modifier;}
        else{
            if(have_key)return {};have_key=true;
            if(token.size()==1&&((token[0]>='a'&&token[0]<='z')||(token[0]>='0'&&token[0]<='9')))result.key=uint32_t(token[0]>='a'?token[0]-'a'+'A':token[0]);
            else if(token.size()>1&&token[0]=='f'){
                unsigned number=0;auto converted=std::from_chars(token.data()+1,token.data()+token.size(),number);
                if(converted.ec!=std::errc{}||converted.ptr!=token.data()+token.size()||number<1||number>24)return {};result.key=111+number;
            }else{auto found=keys().find(token);if(found==keys().end())return {};result.key=found->second;}
        }
        if(end==text.npos)break;at=end+1;
    }while(true);
    return have_key?std::optional(result):std::nullopt;
}
inline std::string format(Binding binding){
    if(binding.modifiers&~15u)return {};
    std::string key;
    if((binding.key>='A'&&binding.key<='Z')||(binding.key>='0'&&binding.key<='9'))key=char(binding.key);
    else if(binding.key>=112&&binding.key<=135)key="F"+std::to_string(binding.key-111);
    else for(const auto& [name,value]:keys())if(value==binding.key){key=name;break;}
    if(key.empty())return {};
    std::string result;
    for(auto [mask,name]:{std::pair{1u,"Ctrl+"},std::pair{2u,"Shift+"},std::pair{4u,"Alt+"},std::pair{8u,"Super+"}})if(binding.modifiers&mask)result+=name;
    return result+key;
}
}
