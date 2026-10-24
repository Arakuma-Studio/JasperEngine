// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipIO.h"

using namespace jasperengine;

std::string anim::ClipParseError::toString() const{
    if (line > 0) return std::to_string(line) + ":" + std::to_string(column) + ": " + message;
    return message;
}

anim::ClipParseResult anim::parseClip(const std::string& text){
    // TODO(T1a): the lenient parser
    (void)text;
    ClipParseResult result;
    result.error.message = "Animation clip loading is not implemented yet.";
    return result;
}

std::string anim::writeClip(const ClipData& data){
    // TODO(T1a): the canonical writer
    (void)data;
    return std::string();
}

void anim::normalize(ClipData& data, std::vector<ClipWarning>* warnings){
    // TODO(T1a)
    (void)data;
    (void)warnings;
}

bool anim::isUnityYamlClip(const std::string& text){
    // TODO(T1a)
    (void)text;
    return false;
}

const char* anim::unityYamlRefusal(){
    return "This is a Unity .anim (YAML). JasperEngine clips are JSON {\"format\": \"jasperengine.anim\"}: "
           "create one with Create > Animation Clip or edit_animation {clip, create}.";
}

std::string anim::generateClipGuid(){
    // TODO(T1a)
    return std::string();
}

bool anim::isClipGuid(const std::string& text){
    // TODO(T1a)
    (void)text;
    return false;
}

std::string anim::readClipGuid(const std::string& text){
    // TODO(T1a)
    (void)text;
    return std::string();
}

bool anim::replaceClipGuid(std::string& text, const std::string& guid){
    // TODO(T1a)
    (void)text;
    (void)guid;
    return false;
}

const char* anim::channelName(ClipChannel channel){
    // TODO(T1a)
    (void)channel;
    return "";
}

bool anim::channelFromName(const std::string& name, ClipChannel& out){
    // TODO(T1a)
    (void)name;
    (void)out;
    return false;
}

const char* anim::wrapName(ClipWrapMode wrap){
    // TODO(T1a)
    (void)wrap;
    return "";
}

bool anim::wrapFromName(const std::string& name, ClipWrapMode& out){
    // TODO(T1a)
    (void)name;
    (void)out;
    return false;
}

const char* anim::easeName(EaseType ease){
    // TODO(T1a)
    (void)ease;
    return "";
}

bool anim::easeFromName(const std::string& name, EaseType& out){
    // TODO(T1a)
    (void)name;
    (void)out;
    return false;
}
