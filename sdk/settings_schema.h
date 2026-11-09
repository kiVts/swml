#pragma once
#include "json.h"
#include "input_bindings.h"
#include <algorithm>
#include <limits>
#include <set>

namespace swml_settings {
using swml_json::Value;
struct Field {
    std::string id, section, key, type, description, category, authority="local";
    Value initial;
    double minimum=-9007199254740991.0, maximum=9007199254740991.0;
    std::vector<std::string> choices;
    bool restart=false;
};
inline bool name(std::string_view text) {
    return !text.empty() && text.size()<=95 && std::all_of(text.begin(),text.end(),[](char c) {
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';
    });
}
inline void validate(const Field& field, const Value& value) {
    auto fail=[] { throw std::runtime_error("setting value does not match schema"); };
    if (field.type=="bool") { if (!value.is_bool()) fail(); }
    else if (field.type=="int" || field.type=="number") {
        if (!value.is_number() || !std::isfinite(value.number()) || value.number()<field.minimum || value.number()>field.maximum ||
            (field.type=="int" && std::trunc(value.number())!=value.number())) fail();
    } else {
        if (!value.is_string() || value.string().size()>4096 || value.string().find_first_of("\r\n\0",0,3)!=std::string::npos || !swml_json::valid_utf8(value.string())) fail();
        if (field.type=="enum" && std::find(field.choices.begin(),field.choices.end(),value.string())==field.choices.end()) fail();
        if (field.type=="keybind" && !swml_input::parse(value.string())) fail();
    }
}
inline std::vector<Field> parse(const Value& schema) {
    auto version=schema.find("version"), fields=schema.find("settings");
    if (!version || !version->is_number() || version->number()!=1 || !fields || !fields->is_array() || fields->array().size()>128) throw std::runtime_error("invalid settings schema");
    std::vector<Field> out; std::set<std::string> ids, locations;
    for (const auto& item : fields->array()) {
        Field field; field.id=item.text("id"); field.section=item.text("section","general"); field.key=item.text("key",field.id);
        field.type=item.text("type"); field.description=item.text("description"); field.category=item.text("category",field.section); field.authority=item.text("authority","local");
        if (!name(field.id)||!name(field.section)||!name(field.key)||!ids.insert(field.id).second||!locations.insert(field.section+"/"+field.key).second||
            field.description.size()>2048||field.category.size()>128||(field.authority!="local"&&field.authority!="host")||
            (field.type!="bool"&&field.type!="int"&&field.type!="number"&&field.type!="string"&&field.type!="enum"&&field.type!="keybind")) throw std::runtime_error("invalid or duplicate setting declaration");
        auto initial=item.find("default"); if (!initial) throw std::runtime_error("setting default is required"); field.initial=*initial;
        for (auto bound : {"min","max"}) if (auto v=item.find(bound)) {
            if (!v->is_number() || !std::isfinite(v->number())) throw std::runtime_error("invalid setting bounds");
            (std::string_view(bound)=="min"?field.minimum:field.maximum)=v->number();
        }
        if (field.minimum>field.maximum || field.minimum<-9007199254740991.0 || field.maximum>9007199254740991.0) throw std::runtime_error("invalid setting bounds");
        if (auto choices=item.find("choices")) {
            if (!choices->is_array() || choices->array().size()>128) throw std::runtime_error("invalid choices");
            for (const auto& choice : choices->array()) {
                if (!choice.is_string() || choice.string().size()>256 || std::find(field.choices.begin(),field.choices.end(),choice.string())!=field.choices.end()) throw std::runtime_error("invalid choice");
                field.choices.push_back(choice.string());
            }
        }
        if (auto restart=item.find("restart")) { if (!restart->is_bool()) throw std::runtime_error("restart must be boolean"); field.restart=restart->boolean(); }
        validate(field,field.initial); out.push_back(std::move(field));
    }
    return out;
}
inline Value decode(const Field& field, std::string_view text) {
    Value value;
    if (field.type=="string"||field.type=="enum"||field.type=="keybind") value=std::string(text);
    else if (field.type=="bool") {
        std::string lower(text); std::transform(lower.begin(),lower.end(),lower.begin(),[](char c){return c>='A'&&c<='Z'?char(c+'a'-'A'):c;});
        if (lower=="1"||lower=="true"||lower=="yes"||lower=="on") value=true;
        else if (lower=="0"||lower=="false"||lower=="no"||lower=="off") value=false;
        else throw std::runtime_error("invalid boolean setting");
    } else value=swml_json::parse(text);
    validate(field,value); return value;
}
inline std::string encode(const Field& field,const Value& value) {
    validate(field,value); return value.is_string()?"\""+value.string()+"\"":swml_json::dump(value);
}
} // namespace swml_settings
