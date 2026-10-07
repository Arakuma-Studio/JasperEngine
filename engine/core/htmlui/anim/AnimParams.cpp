// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimParams.h"

#include "AnimCommon.h"

#include <cmath>

using namespace jasperengine;

AnimParam::AnimParam(bool v): type(Type::BOOL), boolValue(v) {}
AnimParam::AnimParam(double v): type(Type::NUMBER), numberValue(v) {}
AnimParam::AnimParam(int v): type(Type::NUMBER), numberValue(v) {}
AnimParam::AnimParam(const char* v): type(Type::STRING), stringValue(v ? v : "") {}
AnimParam::AnimParam(std::string v): type(Type::STRING), stringValue(std::move(v)) {}

AnimParam::AnimParam(const AnimRawValue& v){
    if (v.isNumber()){
        type = Type::NUMBER;
        numberValue = v.number;
    } else if (v.isString()){
        type = Type::STRING;
        stringValue = v.text;
    }
}

AnimParam AnimParam::array(std::initializer_list<AnimParam> values){
    AnimParam p;
    p.type = Type::ARRAY;
    p.items.assign(values.begin(), values.end());
    return p;
}

AnimParam AnimParam::array(std::vector<AnimParam> values){
    AnimParam p;
    p.type = Type::ARRAY;
    p.items = std::move(values);
    return p;
}

AnimParam AnimParam::object(std::initializer_list<Member> values){
    AnimParam p;
    p.type = Type::OBJECT;
    for (const Member& member : values)
        p.set(member.first, member.second);
    return p;
}

AnimParam AnimParam::function(Function fn){
    AnimParam p;
    p.type = Type::FUNCTION;
    p.functionValue = std::move(fn);
    return p;
}

AnimParam AnimParam::ease(EaseFn fn){
    AnimParam p;
    p.type = Type::EASE;
    p.easeValue = std::move(fn);
    return p;
}

AnimParam AnimParam::spring(std::shared_ptr<AnimSpring> spring){
    AnimParam p;
    p.type = Type::SPRING;
    p.springValue = std::move(spring);
    return p;
}

AnimParam AnimParam::callback(Callback fn){
    AnimParam p;
    p.type = Type::CALLBACK;
    p.callbackValue = std::move(fn);
    return p;
}

AnimParam AnimParam::timer(std::shared_ptr<AnimTimer> timer){
    AnimParam p;
    p.type = Type::TIMER;
    p.timerValue = std::move(timer);
    return p;
}

bool AnimParam::isTruthy() const{
    switch (type){
        case Type::NIL: return false;
        case Type::BOOL: return boolValue;
        case Type::NUMBER: return numberValue != 0.0 && !std::isnan(numberValue);
        case Type::STRING: return !stringValue.empty();
        default: return true;
    }
}

double AnimParam::toNumber() const{
    switch (type){
        case Type::NIL: return std::nan("");
        case Type::BOOL: return boolValue ? 1.0 : 0.0;
        case Type::NUMBER: return numberValue;
        case Type::STRING: return AnimMath::toNumber(stringValue);
        case Type::ARRAY:
            // +[] is 0, +[x] is +x
            if (items.empty()) return 0.0;
            if (items.size() == 1) return items[0].isString() || items[0].isNumber() ? items[0].toNumber() : std::nan("");
            return std::nan("");
        default: return std::nan("");
    }
}

AnimRawValue AnimParam::toRaw() const{
    switch (type){
        case Type::BOOL: return AnimRawValue(boolValue ? 1.0 : 0.0);
        case Type::NUMBER: return AnimRawValue(numberValue);
        case Type::STRING: return AnimRawValue(stringValue);
        default: return AnimRawValue();
    }
}

size_t AnimParam::size() const{
    return type == Type::ARRAY ? items.size() : type == Type::OBJECT ? members.size() : 0;
}

const AnimParam& AnimParam::at(size_t index) const{
    static const AnimParam nil;
    return index < items.size() ? items[index] : nil;
}

void AnimParam::push(AnimParam value){
    if (type != Type::ARRAY){
        *this = AnimParam();
        type = Type::ARRAY;
    }
    items.push_back(std::move(value));
}

const AnimParam* AnimParam::find(const std::string& key) const{
    for (const Member& member : members){
        if (member.first == key)
            return member.second.isNil() ? nullptr : &member.second;
    }
    return nullptr;
}

const AnimParam& AnimParam::get(const std::string& key) const{
    static const AnimParam nil;
    const AnimParam* p = find(key);
    return p ? *p : nil;
}

void AnimParam::set(const std::string& key, AnimParam value){
    if (type != Type::OBJECT){
        *this = AnimParam();
        type = Type::OBJECT;
    }
    for (Member& member : members){
        if (member.first == key){
            member.second = std::move(value);
            return;
        }
    }
    members.emplace_back(key, std::move(value));
}

void AnimParam::erase(const std::string& key){
    for (auto it = members.begin(); it != members.end(); ++it){
        if (it->first == key){
            members.erase(it);
            return;
        }
    }
}
