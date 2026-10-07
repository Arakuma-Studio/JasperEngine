// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIValue.h"
#include "HtmlUIScriptInternal.h"

#include <cstdlib>
#include <utility>

using namespace jasperengine;

UIValue::UIValue(){
}

UIValue::UIValue(bool value): type(Type::BOOL), boolValue(value){
}

UIValue::UIValue(double value): type(Type::NUMBER), numberValue(value){
}

UIValue::UIValue(int value): type(Type::NUMBER), numberValue((double)value){
}

UIValue::UIValue(const char* value): type(value ? Type::STRING : Type::NIL), stringValue(value ? value : ""){
}

UIValue::UIValue(std::string value): type(Type::STRING), stringValue(std::move(value)){
}

UIValue UIValue::array(){
    UIValue value;
    value.type = Type::ARRAY;
    return value;
}

UIValue UIValue::object(){
    UIValue value;
    value.type = Type::OBJECT;
    return value;
}

UIValue::Type UIValue::getType() const{
    return type;
}

bool UIValue::isNil() const{
    return type == Type::NIL;
}

bool UIValue::asBool() const{
    switch (type){
        case Type::BOOL: return boolValue;
        case Type::NUMBER: return numberValue != 0.0;
        case Type::STRING: return !stringValue.empty();
        case Type::ARRAY:
        case Type::OBJECT: return true;
        default: return false;
    }
}

double UIValue::asNumber() const{
    switch (type){
        case Type::NUMBER: return numberValue;
        case Type::BOOL: return boolValue ? 1.0 : 0.0;
        case Type::STRING: {
            const char* begin = stringValue.c_str();
            char* end = nullptr;
            const double number = std::strtod(begin, &end);
            return (end != begin) ? number : 0.0;
        }
        default: return 0.0;
    }
}

const std::string& UIValue::asString() const{
    static const std::string empty;
    return (type == Type::STRING) ? stringValue : empty;
}

size_t UIValue::size() const{
    return (type == Type::ARRAY || type == Type::OBJECT) ? items.size() : 0;
}

UIValue& UIValue::operator[](size_t index){
    if (type != Type::ARRAY){
        *this = array();
    }
    if (index >= items.size()){
        items.resize(index + 1);
    }
    return items[index];
}

UIValue& UIValue::operator[](const std::string& key){
    if (type != Type::OBJECT){
        *this = object();
    }
    for (size_t i = 0; i < itemKeys.size(); i++){
        if (itemKeys[i] == key)
            return items[i];
    }
    itemKeys.push_back(key);
    items.emplace_back();
    return items.back();
}

const UIValue* UIValue::find(const std::string& key) const{
    if (type != Type::OBJECT)
        return nullptr;
    for (size_t i = 0; i < itemKeys.size(); i++){
        if (itemKeys[i] == key)
            return &items[i];
    }
    return nullptr;
}

void UIValue::push(UIValue value){
    if (type != Type::ARRAY){
        *this = array();
    }
    items.push_back(std::move(value));
}

void UIValue::erase(size_t index){
    if (type == Type::ARRAY && index < items.size()){
        items.erase(items.begin() + (std::ptrdiff_t)index);
    }
}

void UIValue::erase(const std::string& key){
    if (type != Type::OBJECT)
        return;
    for (size_t i = 0; i < itemKeys.size(); i++){
        if (itemKeys[i] == key){
            itemKeys.erase(itemKeys.begin() + (std::ptrdiff_t)i);
            items.erase(items.begin() + (std::ptrdiff_t)i);
            return;
        }
    }
}

std::vector<std::string> UIValue::keys() const{
    return (type == Type::OBJECT) ? itemKeys : std::vector<std::string>();
}

bool UIValue::operator==(const UIValue& other) const{
    if (type != other.type)
        return false;
    switch (type){
        case Type::NIL: return true;
        case Type::BOOL: return boolValue == other.boolValue;
        case Type::NUMBER: return numberValue == other.numberValue;
        case Type::STRING: return stringValue == other.stringValue;
        case Type::ARRAY: return items == other.items;
        case Type::OBJECT: return itemKeys == other.itemKeys && items == other.items;
    }
    return false;
}

bool UIValue::operator!=(const UIValue& other) const{
    return !(*this == other);
}

// ---- HtmlUIScriptAccess (HtmlUIScriptInternal.h) ----

std::vector<UIValue>& HtmlUIScriptAccess::items(UIValue& value){
    return value.items;
}

const std::vector<UIValue>& HtmlUIScriptAccess::items(const UIValue& value){
    return value.items;
}

const std::vector<std::string>& HtmlUIScriptAccess::objectKeys(const UIValue& value){
    return value.itemKeys;
}
