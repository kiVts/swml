#pragma once
#include "json.h"
#include <algorithm>
#include <set>
namespace swml_content {
// Deliberately documented JSON Schema subset, not a claim of full draft support.
inline void validate(const swml_json::Value& value,const swml_json::Value& schema,unsigned depth=0) {
    using swml_json::Value;
    if (depth>24 || !schema.is_object()) throw std::runtime_error("invalid content schema");
    auto type=schema.text("type");
    bool matching=type.empty()||(type=="object"&&value.is_object())||(type=="array"&&value.is_array())||
        (type=="string"&&value.is_string())||(type=="boolean"&&value.is_bool())||(type=="number"&&value.is_number())||
        (type=="integer"&&value.is_number()&&std::trunc(value.number())==value.number())||
        (type=="null"&&std::holds_alternative<std::nullptr_t>(value.data));
    if (!matching) throw std::runtime_error("content type mismatch");
    if (auto choices=schema.find("enum")) {
        if (!choices->is_array()) throw std::runtime_error("invalid enum schema");
        auto encoded=swml_json::dump(value); bool found=false;
        for (const auto& choice : choices->array()) found|=swml_json::dump(choice)==encoded;
        if (!found) throw std::runtime_error("content enum mismatch");
    }
    if (value.is_number()) {
        if (auto lower=schema.find("minimum")) if (!lower->is_number()||value.number()<lower->number()) throw std::runtime_error("content below minimum");
        if (auto upper=schema.find("maximum")) if (!upper->is_number()||value.number()>upper->number()) throw std::runtime_error("content above maximum");
    }
    if (value.is_string()) if (auto maximum=schema.find("maxLength"))
        if (!maximum->is_number()||maximum->number()<0||double(std::count_if(value.string().begin(),value.string().end(),[](unsigned char c){return (c&0xc0)!=0x80;}))>maximum->number()) throw std::runtime_error("content string too long");
    if (value.is_array()) {
        if (auto maximum=schema.find("maxItems")) if (!maximum->is_number()||maximum->number()<0||double(value.array().size())>maximum->number()) throw std::runtime_error("content array too long");
        if (auto items=schema.find("items")) for (const auto& item : value.array()) validate(item,*items,depth+1);
    }
    if (value.is_object()) {
        if (auto required=schema.find("required")) {
            if (!required->is_array()) throw std::runtime_error("invalid required list");
            for (const auto& key : required->array()) if (!key.is_string()||!value.find(key.string())) throw std::runtime_error("required content field missing");
        }
        auto properties=schema.find("properties"), additional=schema.find("additionalProperties");
        if (properties&&!properties->is_object()) throw std::runtime_error("invalid content properties");
        if (additional&&!additional->is_bool()) throw std::runtime_error("invalid additionalProperties");
        for (const auto& [key,item] : value.object()) {
            auto property=properties?properties->find(key):nullptr;
            if (property) validate(item,*property,depth+1);
            else if (additional&&!additional->boolean()) throw std::runtime_error("unknown content field");
        }
    }
}
inline void check_schema(const swml_json::Value& schema,unsigned depth=0) {
    if (depth>24||!schema.is_object()) throw std::runtime_error("invalid content schema");
    const std::set<std::string> supported{"type","enum","minimum","maximum","maxLength","maxItems","items","properties","required","additionalProperties","description"};
    for (const auto& [key,value] : schema.object()) if (!supported.contains(key)) throw std::runtime_error("unsupported content schema keyword: "+key);
    auto type=schema.text("type");
    if (auto field=schema.find("type")) if (!field->is_string()||type.empty()) throw std::runtime_error("invalid schema type");
    if (!type.empty()&&type!="object"&&type!="array"&&type!="string"&&type!="boolean"&&type!="number"&&type!="integer"&&type!="null") throw std::runtime_error("unknown schema type");
    for (auto key : {"minimum","maximum"}) if (auto field=schema.find(key)) if (!field->is_number()) throw std::runtime_error("invalid numeric constraint");
    if (schema.find("minimum")&&schema.find("maximum")&&schema.find("minimum")->number()>schema.find("maximum")->number()) throw std::runtime_error("inverted numeric range");
    for (auto key : {"maxLength","maxItems"}) if (auto field=schema.find(key))
        if (!field->is_number()||field->number()<0||std::trunc(field->number())!=field->number()) throw std::runtime_error("invalid length constraint");
    if (auto field=schema.find("additionalProperties")) if (!field->is_bool()) throw std::runtime_error("invalid additionalProperties");
    if (auto field=schema.find("description")) if (!field->is_string()) throw std::runtime_error("invalid schema description");
    if (auto field=schema.find("enum")) {
        if (!field->is_array()||field->array().empty()) throw std::runtime_error("invalid schema enum");
        std::set<std::string> values;
        for (const auto& value:field->array()) if (!values.insert(swml_json::dump(value)).second) throw std::runtime_error("duplicate enum value");
    }
    if (auto field=schema.find("required")) {
        if (!field->is_array()) throw std::runtime_error("invalid required list");
        std::set<std::string> names;
        for (const auto& name:field->array()) if (!name.is_string()||!names.insert(name.string()).second) throw std::runtime_error("invalid/duplicate required field");
    }
    if (auto properties=schema.find("properties")) {
        if (!properties->is_object()) throw std::runtime_error("invalid content properties");
        for (const auto& [key,property] : properties->object()) { (void)key; check_schema(property,depth+1); }
    }
    if (auto items=schema.find("items")) check_schema(*items,depth+1);
}
}
