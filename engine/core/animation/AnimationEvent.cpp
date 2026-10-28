// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationEvent.h"

using namespace jasperengine;

AnimationEventArg AnimationEvent::numberArg(double value){
    return AnimationEventArg(std::in_place_type<double>, value);
}

AnimationEventArg AnimationEvent::stringArg(std::string value){
    return AnimationEventArg(std::in_place_type<std::string>, std::move(value));
}

AnimationEventArg AnimationEvent::boolArg(bool value){
    return AnimationEventArg(std::in_place_type<bool>, value);
}

size_t AnimationEvent::getArgCount() const{
    return args.size();
}

std::string AnimationEvent::getArgType(size_t index) const{
    if (index >= args.size()) return "";
    if (std::holds_alternative<double>(args[index])) return "number";
    if (std::holds_alternative<std::string>(args[index])) return "string";
    return "boolean";
}

double AnimationEvent::getNumberArg(size_t index) const{
    if (index >= args.size()) return 0.0;
    const double* value = std::get_if<double>(&args[index]);
    return value ? *value : 0.0;
}

std::string AnimationEvent::getStringArg(size_t index) const{
    if (index >= args.size()) return "";
    const std::string* value = std::get_if<std::string>(&args[index]);
    return value ? *value : std::string();
}

bool AnimationEvent::getBoolArg(size_t index) const{
    if (index >= args.size()) return false;
    const bool* value = std::get_if<bool>(&args[index]);
    return value ? *value : false;
}
