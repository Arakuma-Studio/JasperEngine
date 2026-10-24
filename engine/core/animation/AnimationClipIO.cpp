// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipIO.h"

#include <cctype>
#include <cstring>
#include <random>

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

namespace{

    size_t skipBom(const std::string& text){
        return text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    }

    // A top-level member of the JSON object `text`: where its key starts and its value's span.
    // A light scanner (strings, escapes, nesting), not a validator.
    struct TopLevelMember{
        size_t keyStart = std::string::npos;
        size_t valueStart = std::string::npos;
        size_t valueEnd = std::string::npos;    // one past the value
    };

    size_t skipSpace(const std::string& text, size_t i){
        while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
        return i;
    }

    // one past the string starting at text[i] == '"', npos when unterminated
    size_t skipString(const std::string& text, size_t i){
        for (i = i + 1; i < text.size(); i++){
            if (text[i] == '\\') i++;
            else if (text[i] == '"') return i + 1;
        }
        return std::string::npos;
    }

    // one past the JSON value starting at text[i]
    size_t skipValue(const std::string& text, size_t i){
        if (i >= text.size()) return std::string::npos;
        if (text[i] == '"') return skipString(text, i);
        if (text[i] == '{' || text[i] == '['){
            int depth = 0;
            while (i < text.size()){
                char c = text[i];
                if (c == '"'){
                    i = skipString(text, i);
                    if (i == std::string::npos) return i;
                    continue;
                }
                if (c == '{' || c == '[') depth++;
                if (c == '}' || c == ']'){
                    depth--;
                    if (depth == 0) return i + 1;
                }
                i++;
            }
            return std::string::npos;
        }
        while (i < text.size() && text[i] != ',' && text[i] != '}' && text[i] != ']' && !std::isspace((unsigned char)text[i])) i++;
        return i;
    }

    bool findTopLevelMember(const std::string& text, const std::string& key, TopLevelMember& out){
        size_t i = skipSpace(text, skipBom(text));
        if (i >= text.size() || text[i] != '{') return false;
        i = skipSpace(text, i + 1);
        while (i < text.size() && text[i] == '"'){
            size_t keyStart = i;
            size_t keyEnd = skipString(text, i);
            if (keyEnd == std::string::npos) return false;
            std::string name = text.substr(keyStart + 1, keyEnd - keyStart - 2);
            i = skipSpace(text, keyEnd);
            if (i >= text.size() || text[i] != ':') return false;
            i = skipSpace(text, i + 1);
            size_t valueEnd = skipValue(text, i);
            if (valueEnd == std::string::npos) return false;
            if (name == key){
                out.keyStart = keyStart;
                out.valueStart = i;
                out.valueEnd = valueEnd;
                return true;
            }
            i = skipSpace(text, valueEnd);
            if (i < text.size() && text[i] == ',') i = skipSpace(text, i + 1);
            else break;
        }
        return false;
    }

    const char* const channelNames[] = {"position", "rotation", "scale", "morph", "lattice", "property"};
    const char* const wrapNames[] = {"default", "once", "loop", "ping_pong", "clamp_forever"};

    struct EaseName{
        EaseType ease;
        const char* name;
    };
    const EaseName easeNames[] = {
        {EaseType::LINEAR, "linear"}, {EaseType::STEP, "step"},
        {EaseType::QUAD_IN, "quad_in"}, {EaseType::QUAD_OUT, "quad_out"}, {EaseType::QUAD_IN_OUT, "quad_in_out"},
        {EaseType::CUBIC_IN, "cubic_in"}, {EaseType::CUBIC_OUT, "cubic_out"}, {EaseType::CUBIC_IN_OUT, "cubic_in_out"},
        {EaseType::QUART_IN, "quart_in"}, {EaseType::QUART_OUT, "quart_out"}, {EaseType::QUART_IN_OUT, "quart_in_out"},
        {EaseType::QUINT_IN, "quint_in"}, {EaseType::QUINT_OUT, "quint_out"}, {EaseType::QUINT_IN_OUT, "quint_in_out"},
        {EaseType::SINE_IN, "sine_in"}, {EaseType::SINE_OUT, "sine_out"}, {EaseType::SINE_IN_OUT, "sine_in_out"},
        {EaseType::EXPO_IN, "expo_in"}, {EaseType::EXPO_OUT, "expo_out"}, {EaseType::EXPO_IN_OUT, "expo_in_out"},
        {EaseType::CIRC_IN, "circ_in"}, {EaseType::CIRC_OUT, "circ_out"}, {EaseType::CIRC_IN_OUT, "circ_in_out"},
        {EaseType::ELASTIC_IN, "elastic_in"}, {EaseType::ELASTIC_OUT, "elastic_out"}, {EaseType::ELASTIC_IN_OUT, "elastic_in_out"},
        {EaseType::BACK_IN, "back_in"}, {EaseType::BACK_OUT, "back_out"}, {EaseType::BACK_IN_OUT, "back_in_out"},
        {EaseType::BOUNCE_IN, "bounce_in"}, {EaseType::BOUNCE_OUT, "bounce_out"}, {EaseType::BOUNCE_IN_OUT, "bounce_in_out"}
    };

}

bool anim::isUnityYamlClip(const std::string& text){
    size_t i = skipSpace(text, skipBom(text));
    if (text.compare(i, 5, "%YAML") == 0) return true;
    // a Unity serialized document: "--- !u!74 &7400000" then "AnimationClip:" at a line start
    size_t pos = 0;
    while ((pos = text.find("AnimationClip:", pos)) != std::string::npos){
        if (pos == 0 || text[pos - 1] == '\n') return true;
        pos++;
    }
    return false;
}

const char* anim::unityYamlRefusal(){
    return "This is a Unity .anim (YAML). JasperEngine clips are JSON {\"format\": \"jasperengine.anim\"}: "
           "create one with Create > Animation Clip or edit_animation {clip, create}.";
}

std::string anim::generateClipGuid(){
    static const char* hex = "0123456789abcdef";
    std::random_device device;
    std::mt19937_64 generator(((uint64_t)device() << 32) ^ device());
    std::string guid(32, '0');
    uint64_t bits = 0;
    for (size_t i = 0; i < guid.size(); i++){
        if (i % 16 == 0) bits = generator();
        guid[i] = hex[bits & 0xF];
        bits >>= 4;
    }
    return guid;
}

bool anim::isClipGuid(const std::string& text){
    if (text.size() != 32) return false;
    for (char c : text){
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string anim::readClipGuid(const std::string& text){
    TopLevelMember member;
    if (!findTopLevelMember(text, "guid", member) || text[member.valueStart] != '"') return std::string();
    std::string value = text.substr(member.valueStart + 1, member.valueEnd - member.valueStart - 2);
    return isClipGuid(value) ? value : std::string();
}

bool anim::replaceClipGuid(std::string& text, const std::string& guid){
    if (!isClipGuid(guid)) return false;
    TopLevelMember member;
    if (findTopLevelMember(text, "guid", member)){
        text.replace(member.valueStart, member.valueEnd - member.valueStart, "\"" + guid + "\"");
        return true;
    }
    TopLevelMember version;
    if (!findTopLevelMember(text, "version", version)) return false;
    text.insert(version.valueEnd, ",\n  \"guid\": \"" + guid + "\"");
    return true;
}

const char* anim::channelName(ClipChannel channel){
    size_t index = (size_t)channel;
    return index < sizeof(channelNames) / sizeof(channelNames[0]) ? channelNames[index] : "";
}

bool anim::channelFromName(const std::string& name, ClipChannel& out){
    for (size_t i = 0; i < sizeof(channelNames) / sizeof(channelNames[0]); i++){
        if (name == channelNames[i]){
            out = (ClipChannel)i;
            return true;
        }
    }
    return false;
}

const char* anim::wrapName(ClipWrapMode wrap){
    size_t index = (size_t)wrap;
    return index < sizeof(wrapNames) / sizeof(wrapNames[0]) ? wrapNames[index] : "";
}

bool anim::wrapFromName(const std::string& name, ClipWrapMode& out){
    for (size_t i = 0; i < sizeof(wrapNames) / sizeof(wrapNames[0]); i++){
        if (name == wrapNames[i]){
            out = (ClipWrapMode)i;
            return true;
        }
    }
    return false;
}

const char* anim::easeName(EaseType ease){
    for (const EaseName& entry : easeNames){
        if (entry.ease == ease) return entry.name;
    }
    // CUSTOM has no per-segment function: keyframes treat it as linear
    return "linear";
}

bool anim::easeFromName(const std::string& name, EaseType& out){
    for (const EaseName& entry : easeNames){
        if (name == entry.name){
            out = entry.ease;
            return true;
        }
    }
    return false;
}
