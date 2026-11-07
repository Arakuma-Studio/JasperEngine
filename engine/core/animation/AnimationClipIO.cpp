// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipIO.h"

#include "animation/AnimatorAssetIO.h"
#include "animation/AnimJson.h"
#include "animation/PropertyBindings.h"
#include "pose/HumanExtraBones.h"
#include "pose/PoseDofs.h"
#include "pose/PoseRig.h"

#include "Engine.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <type_traits>
#include <random>
#include <variant>

using namespace jasperengine;

std::string anim::ClipParseError::toString() const{
    if (line > 0) return std::to_string(line) + ":" + std::to_string(column) + ": " + message;
    return message;
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

    const char* const channelNames[] = {"position", "rotation", "scale", "morph", "lattice", "property", "humanoid", "humanoid_position"};
    const char* const humanoidPositionNames[] = {"hips", "left_foot", "right_foot"};
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

Quaternion anim::eulerDegreesToQuaternion(float x, float y, float z){
    // Quaternion::fromEulerAngles(ZYX) with each factor built in radians (fromAngleAxis'
    // half-angle form), so Engine::useDegrees plays no part
    auto axis = [](float degrees, int which){
        const double pi = 3.14159265358979323846;
        float half = 0.5f * (float)(degrees * (pi / 180.0));
        float s = std::sin(half);
        return Quaternion(std::cos(half), which == 0 ? s : 0.0f, which == 1 ? s : 0.0f, which == 2 ? s : 0.0f);
    };
    return axis(z, 2) * (axis(y, 1) * axis(x, 0));
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
           "create one with Assets > Create > Animation Clip or edit_animation {clip, create}.";
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

static_assert(anim::HUMANOID_TRACK_BONE_COUNT == pose::HUMAN_BONE_COUNT + pose::HUMAN_EXTRA_BONE_COUNT, "humanoid track bones: slots then extra bones");

const char* anim::humanoidTrackBoneName(ClipChannel channel, int humanBone){
    if (channel == ClipChannel::Humanoid){
        if (humanBone >= 0 && humanBone < pose::HUMAN_BONE_COUNT) return pose::humanBoneName(pose::boneAt(humanBone));
        if (humanBone >= pose::HUMAN_BONE_COUNT && humanBone < HUMANOID_TRACK_BONE_COUNT){
            return pose::humanExtraBoneName(pose::extraAt(humanBone - pose::HUMAN_BONE_COUNT));
        }
    }else if (channel == ClipChannel::HumanoidPosition){
        if (humanBone >= 0 && humanBone < HUMANOID_POSITION_COUNT) return humanoidPositionNames[humanBone];
    }
    return "";
}

bool anim::humanoidTrackBoneFromName(ClipChannel channel, const std::string& name, int& out){
    if (channel == ClipChannel::Humanoid){
        pose::HumanBone bone;
        if (pose::humanBoneFromName(name, bone)){
            out = pose::boneIndex(bone);
            return true;
        }
        pose::HumanExtraBone extra;
        if (pose::humanExtraBoneFromName(name, extra)){
            out = pose::HUMAN_BONE_COUNT + pose::extraIndex(extra);
            return true;
        }
        return false;
    }
    if (channel == ClipChannel::HumanoidPosition){
        for (int i = 0; i < HUMANOID_POSITION_COUNT; i++){
            if (name == humanoidPositionNames[i]){
                out = i;
                return true;
            }
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

namespace{
    struct TangentModeName{ ClipTangentMode mode; const char* name; };
    const TangentModeName tangentModeNames[] = {
        {ClipTangentMode::ClampedAuto, "clamped_auto"}, {ClipTangentMode::Auto, "auto"}, {ClipTangentMode::Free, "free"},
        {ClipTangentMode::Linear, "linear"}, {ClipTangentMode::Constant, "constant"}, {ClipTangentMode::Flat, "flat"},
        {ClipTangentMode::Broken, "broken"}
    };
}

const char* anim::tangentModeName(ClipTangentMode mode){
    for (const TangentModeName& entry : tangentModeNames){
        if (entry.mode == mode) return entry.name;
    }
    return "";
}

bool anim::tangentModeFromName(const std::string& name, ClipTangentMode& out){
    for (const TangentModeName& entry : tangentModeNames){
        if (name == entry.name){
            out = entry.mode;
            return true;
        }
    }
    return false;
}

void anim::applyTangentModes(ClipTrackData& track){
    if (track.channel == ClipChannel::Property || track.keys.empty()) return;
    bool any = false;
    for (const ClipKey& key : track.keys) any = any || key.tangentMode != ClipTangentMode::None;
    if (!any) return;
    const size_t n = track.keys.size();
    const bool rotation = track.channel == ClipChannel::Rotation;
    std::vector<float> previous, next;
    for (size_t k = 0; k < n; k++){
        ClipKey& key = track.keys[k];
        ClipTangentMode mode = key.tangentMode;
        if ((mode == ClipTangentMode::None || mode == ClipTangentMode::Free || mode == ClipTangentMode::Broken) && key.hasTangents()) continue;
        if (mode == ClipTangentMode::None) mode = ClipTangentMode::Linear;
        if (mode == ClipTangentMode::Free || mode == ClipTangentMode::Broken) mode = ClipTangentMode::ClampedAuto;
        const size_t width = key.value.size();
        // the neighbours' values (a rotation's on this key's hemisphere: the curve is componentwise)
        auto neighbour = [&](size_t index, std::vector<float>& out){
            out = track.keys[index].value;
            out.resize(width, 0.0f);
            if (rotation){
                float dot = 0.0f;
                for (size_t c = 0; c < width; c++) dot += out[c] * key.value[c];
                if (dot < 0.0f) for (float& v : out) v = -v;
            }
        };
        const bool hasPrevious = k > 0;
        const bool hasNext = k + 1 < n;
        if (hasPrevious) neighbour(k - 1, previous);
        if (hasNext) neighbour(k + 1, next);
        const float dtPrevious = hasPrevious ? key.time - track.keys[k - 1].time : 0.0f;
        const float dtNext = hasNext ? track.keys[k + 1].time - key.time : 0.0f;
        key.inTangent.assign(width, 0.0f);
        key.outTangent.assign(width, 0.0f);
        for (size_t c = 0; c < width; c++){
            const float slopeIn = hasPrevious && dtPrevious > 0.0f ? (key.value[c] - previous[c]) / dtPrevious : 0.0f;
            const float slopeOut = hasNext && dtNext > 0.0f ? (next[c] - key.value[c]) / dtNext : 0.0f;
            float in = 0.0f, out = 0.0f;
            switch (mode){
                case ClipTangentMode::Linear:
                    in = hasPrevious ? slopeIn : slopeOut;
                    out = hasNext ? slopeOut : slopeIn;
                    break;
                case ClipTangentMode::Auto:
                case ClipTangentMode::ClampedAuto:
                    if (hasPrevious && hasNext && dtPrevious + dtNext > 0.0f){
                        float m = (next[c] - previous[c]) / (dtPrevious + dtNext);
                        if (mode == ClipTangentMode::ClampedAuto){
                            if (slopeIn * slopeOut <= 0.0f){
                                m = 0.0f;
                            }else{
                                const float limit = 3.0f * std::min(std::fabs(slopeIn), std::fabs(slopeOut));
                                m = std::max(-limit, std::min(limit, m));
                            }
                        }
                        in = out = m;
                    }
                    break;
                default:    // Flat, Constant
                    break;
            }
            key.inTangent[c] = in;
            key.outTangent[c] = out;
        }
        if (key.tangentMode == ClipTangentMode::Constant) key.ease = EaseType::STEP;
    }
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

// ---- parser, normalizer and canonical writer ----

namespace{

    using OJson = nlohmann::ordered_json;

    // where each part of a ClipData came from in the input, for warning paths
    struct TrackOrigin{
        std::string path;               // "tracks[3]"
        std::vector<std::string> keys;  // JSON path of each key ("tracks[3].keys[2]")
    };

    struct ClipOrigins{
        std::vector<TrackOrigin> tracks;
        std::vector<size_t> events;     // file index of each event
        std::vector<size_t> audio;      // file index of each audio cue
    };

    void warn(std::vector<ClipWarning>* warnings, const std::string& path, const std::string& message){
        if (warnings) warnings->push_back({path, message});
    }

    std::string indexed(const std::string& path, const char* member, size_t index){
        return path + (path.empty() ? "" : ".") + member + "[" + std::to_string(index) + "]";
    }

    std::string member(const std::string& path, const char* name){
        return path.empty() ? std::string(name) : path + "." + name;
    }

    std::string shortNumber(double value){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        return buffer;
    }

    // times live on the 1e-6 s grid the file stores
    float quantizeTime(float time){
        return (float)(std::round((double)time * 1e6) / 1e6);
    }

    // a value as the file stores it (%.6g of the float, read back)
    float quantizeValue(float value){
        if (!std::isfinite(value)) return 0.0f;
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        return (float)std::strtod(buffer, nullptr);
    }

    bool allFinite(const std::vector<float>& values){
        for (float value : values){
            if (!std::isfinite(value)) return false;
        }
        return true;
    }

    // a JSON number as a float, false when it doesn't fit one
    bool toFloat(const OJson& value, float& out){
        if (!value.is_number()) return false;
        double number = value.get<double>();
        if (!std::isfinite(number) || std::fabs(number) > (double)FLT_MAX) return false;
        out = (float)number;
        return true;
    }

    bool readNumbers(const OJson& value, size_t count, std::vector<float>& out){
        if (!value.is_array() || (count > 0 && value.size() != count)) return false;
        out.clear();
        for (const OJson& item : value){
            float number;
            if (!toFloat(item, number)) return false;
            out.push_back(number);
        }
        return true;
    }

    float quaternionLength(const std::vector<float>& q){
        return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    }

    float quaternionDot(const std::vector<float>& a, const std::vector<float>& b){
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    }

    void negate(std::vector<float>& values){
        for (float& value : values) value = -value;
    }

    // a renormalized rotation key: its tangents scale with it (they are relative to the key)
    void scaleRotationKey(ClipKey& key, float factor){
        for (float& v : key.value) v *= factor;
        for (float& v : key.inTangent) v *= factor;
        for (float& v : key.outTangent) v *= factor;
    }

    // hemisphere continuity along a rotation track: each key on the side of the previous one.
    // Only for slerp tracks (any sign gives the same rotations there). A Hermite (CUBICSPLINE)
    // track keeps its signs: its curve is componentwise, and a flipped key changes the segment
    // into it whatever is done to the tangents (negating the key's in/out tangents with it
    // keeps only the segments after it), so the clip would sample differently from the glTF
    // and from the model's entity tracks, which use the values as they are
    void enforceHemisphere(ClipTrackData& track){
        for (const ClipKey& key : track.keys){
            if (key.hasTangents()) return;
        }
        for (size_t k = 1; k < track.keys.size(); k++){
            ClipKey& key = track.keys[k];
            if (quaternionDot(track.keys[k - 1].value, key.value) < 0.0f){
                negate(key.value);
            }
        }
    }

    // Step 0 of a property track: what (component, property) is. Known: kind, width, swizzle and
    // discrete set from the registry (a script field's or shader member's kind from its keys: the
    // parser's, or the first key's width); unknown: kept as read, not played
    void resolvePropertyTrack(ClipTrackData& track, const TrackOrigin& origin, std::vector<ClipWarning>* warnings){
        anim::PropertyRef ref;
        anim::parsePropertyRef(track.component, track.property, ref);
        track.known = ref.known;
        if (!ref.known){
            track.kind = PropertyKind::None;
            track.width = 0;
            track.swizzle = -1;
            track.discrete = false;
            track.keys.clear();
            warn(warnings, origin.path, ref.error + ": kept in the file, not played");
            return;
        }
        track.swizzle = ref.swizzle;
        if (ref.swizzle >= 0){
            track.kind = PropertyKind::Float;
            track.width = 1;
            track.discrete = false;
            return;
        }
        if (ref.kind != PropertyKind::None){
            track.kind = ref.kind;
            track.width = anim::kindWidth(ref.kind);
            track.discrete = ref.discrete;
            return;
        }
        // from the keys: the kind given (parser, editor) when the keys have its width
        size_t width = track.keys.empty() ? 0 : track.keys[0].value.size();
        if (track.kind != PropertyKind::None && (track.keys.empty() || width == anim::kindWidth(track.kind) ||
                                                 (width == 3 && track.kind == PropertyKind::Color4))){
            track.width = anim::kindWidth(track.kind);
        }else{
            width = std::min<size_t>(std::max<size_t>(width, 1), 4);
            track.kind = width == 2 ? PropertyKind::Vector2 : width == 3 ? PropertyKind::Vector3 : width == 4 ? PropertyKind::Vector4 : PropertyKind::Float;
            track.width = width;
        }
        track.discrete = anim::kindIsDiscrete(track.kind);
    }

    // Step 1 of a property track's keys (before the shared checks): widths, bools, ints, and no
    // ease or tangents on discrete properties (they step)
    void sanitizePropertyKeys(ClipTrackData& track, TrackOrigin& origin, std::vector<ClipWarning>* warnings){
        bool easeWarned = false;
        std::vector<ClipKey> kept;
        std::vector<std::string> keptOrigins;
        for (size_t k = 0; k < track.keys.size(); k++){
            ClipKey key = track.keys[k];
            const std::string& keyPath = origin.keys[k];
            if (key.value.size() == 3 && track.width == 4 && track.kind == PropertyKind::Color4){
                key.value.push_back(1.0f);
            }
            if (key.value.size() != track.width){
                warn(warnings, member(keyPath, "value"), "expected " + std::to_string(track.width) + " value" + (track.width == 1 ? "" : "s") +
                     " for the " + anim::kindName(track.kind) + " property, key dropped");
                continue;
            }
            if (track.kind == PropertyKind::Bool){
                for (float& v : key.value) v = v > 0.5f ? 1.0f : 0.0f;
            }else if (track.kind == PropertyKind::Int || track.kind == PropertyKind::Frame){
                for (float& v : key.value){
                    if (std::isfinite(v) && v != std::round(v)){
                        warn(warnings, member(keyPath, "value"), shortNumber(v) + " is not a whole number: rounded");
                        v = std::round(v);
                    }
                }
            }
            if (track.discrete){
                if (key.hasTangents() || !key.inTangent.empty() || !key.outTangent.empty()){
                    warn(warnings, keyPath, "tangents ignored: a discrete property steps");
                    key.inTangent.clear();
                    key.outTangent.clear();
                }
                key.tangentMode = ClipTangentMode::None;
                if (key.ease != EaseType::LINEAR && key.ease != EaseType::STEP && !easeWarned){
                    easeWarned = true;
                    warn(warnings, member(keyPath, "ease"), std::string("ignored: ") + anim::kindName(track.kind) + " properties step");
                }
                key.ease = EaseType::LINEAR;
            }
            kept.push_back(std::move(key));
            keptOrigins.push_back(origin.keys[k]);
        }
        track.keys = std::move(kept);
        origin.keys = std::move(keptOrigins);
    }

    // humanoid tracks after every path track, by channel, then bone (anim-retarget 6.1)
    bool trackLess(const ClipTrackData& a, const ClipTrackData& b){
        const bool ha = isHumanoidChannel(a.channel);
        const bool hb = isHumanoidChannel(b.channel);
        if (ha != hb) return hb;
        if (ha){
            if (a.channel != b.channel) return a.channel < b.channel;
            return a.humanBone < b.humanBone;
        }
        if (a.path != b.path) return a.path < b.path;
        if (a.channel != b.channel) return a.channel < b.channel;
        if (a.component != b.component) return a.component < b.component;
        return a.property < b.property;
    }

    bool sameTrack(const ClipTrackData& a, const ClipTrackData& b){
        if (isHumanoidChannel(a.channel) || isHumanoidChannel(b.channel)){
            return a.channel == b.channel && a.humanBone == b.humanBone;
        }
        return a.path == b.path && a.channel == b.channel &&
               (a.channel != ClipChannel::Property || (a.component == b.component && a.property == b.property));
    }

    // a lattice row (3 floats per point of `from`) in the point order of `to` (missing points 0)
    std::vector<float> remapLatticeRow(const std::vector<float>& row, const std::vector<int>& from, const std::vector<int>& to){
        if (row.empty()) return row;
        std::vector<float> out(3 * to.size(), 0.0f);
        for (size_t i = 0; i < from.size(); i++){
            size_t j = std::lower_bound(to.begin(), to.end(), from[i]) - to.begin();
            if (j < to.size() && to[j] == from[i]){
                for (int c = 0; c < 3; c++) out[3 * j + c] = row[3 * i + c];
            }
        }
        return out;
    }

    // Step 1: each key on its own (widths, numbers, time, tangents, ease); bad keys go
    void sanitizeKeys(ClipTrackData& track, TrackOrigin& origin, std::vector<ClipWarning>* warnings){
        const std::string& path = origin.path;
        if (track.relative && (track.channel == ClipChannel::Morph || track.channel == ClipChannel::Lattice || track.channel == ClipChannel::Property ||
                               isHumanoidChannel(track.channel))){
            warn(warnings, member(path, "relative"), "only position, rotation and scale tracks can be relative, ignored");
            track.relative = false;
        }
        if (track.channel == ClipChannel::Property) sanitizePropertyKeys(track, origin, warnings);

        if (track.channel == ClipChannel::Lattice){
            // points: valid, unique (the later column wins), ascending (columns follow)
            std::vector<int> points;
            std::vector<size_t> column;
            for (size_t i = 0; i < track.points.size(); i++){
                int point = track.points[i];
                if (point < 0){
                    warn(warnings, member(path, "points"), "negative point index " + std::to_string(point) + " dropped");
                    continue;
                }
                auto found = std::find(points.begin(), points.end(), point);
                if (found != points.end()){
                    warn(warnings, member(path, "points"), "point " + std::to_string(point) + " listed twice: the later one wins");
                    column[found - points.begin()] = i;
                    continue;
                }
                points.push_back(point);
                column.push_back(i);
            }
            std::vector<size_t> order(points.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b){ return points[a] < points[b]; });
            std::vector<int> sortedPoints;
            std::vector<size_t> sortedColumns;
            for (size_t i : order){
                sortedPoints.push_back(points[i]);
                sortedColumns.push_back(column[i]);
            }
            size_t oldWidth = 3 * track.points.size();
            for (size_t k = 0; k < track.keys.size(); k++){
                ClipKey& key = track.keys[k];
                const std::string& keyPath = origin.keys[k];
                if (key.value.size() != oldWidth){
                    warn(warnings, member(keyPath, "value"), "expected 3 numbers per point (" + std::to_string(oldWidth) +
                                                              "), padded with 0 or cut");
                    key.value.resize(oldWidth, 0.0f);
                }
                if (key.hasTangents() && (key.inTangent.size() != oldWidth || key.outTangent.size() != oldWidth)){
                    key.inTangent.resize(oldWidth, 0.0f);
                    key.outTangent.resize(oldWidth, 0.0f);
                }
                auto reorder = [&](std::vector<float>& row){
                    if (row.empty()) return;
                    std::vector<float> out;
                    out.reserve(3 * sortedColumns.size());
                    for (size_t c : sortedColumns){
                        for (int i = 0; i < 3; i++) out.push_back(row[3 * c + i]);
                    }
                    row = out;
                };
                reorder(key.value);
                reorder(key.inTangent);
                reorder(key.outTangent);
            }
            track.points = sortedPoints;
        }

        size_t fixedWidth = 0;
        const char* expected = "";
        if (isVectorChannel(track.channel)){
            fixedWidth = 3;
            expected = "expected [x, y, z]";
        }else if (isRotationChannel(track.channel)){
            fixedWidth = 4;
            expected = "expected [x, y, z, w]";
        }else if (track.channel == ClipChannel::Property){
            fixedWidth = track.width;
            expected = "expected the property's values";
        }

        std::vector<ClipKey> kept;
        std::vector<std::string> keptOrigins;
        for (size_t k = 0; k < track.keys.size(); k++){
            ClipKey key = track.keys[k];
            const std::string& keyPath = origin.keys[k];
            if (!std::isfinite(key.time)){
                warn(warnings, member(keyPath, "time"), "not a number, key dropped");
                continue;
            }
            if (fixedWidth > 0 && key.value.size() != fixedWidth){
                warn(warnings, member(keyPath, "value"), std::string(expected) + ", key dropped");
                continue;
            }
            if (!allFinite(key.value)){
                warn(warnings, member(keyPath, "value"), "not a finite number, key dropped");
                continue;
            }
            if (isRotationChannel(track.channel) && quaternionLength(key.value) < 1e-6f){
                warn(warnings, member(keyPath, "value"), "a zero quaternion is no rotation, key dropped");
                continue;
            }
            if (track.channel == ClipChannel::Morph && key.value.size() > MAX_MORPHTARGETS){
                warn(warnings, member(keyPath, "value"), "more than " + std::to_string(MAX_MORPHTARGETS) +
                                                          " morph weights: the rest are dropped");
                key.value.resize(MAX_MORPHTARGETS);
            }
            if (!key.inTangent.empty() || !key.outTangent.empty()){
                bool ok = !key.inTangent.empty() && !key.outTangent.empty();
                if (!ok){
                    warn(warnings, keyPath, "in and out tangents go together: tangents ignored");
                }else if (track.channel == ClipChannel::Morph){
                    key.inTangent.resize(key.value.size(), 0.0f);
                    key.outTangent.resize(key.value.size(), 0.0f);
                }else if (key.inTangent.size() != key.value.size() || key.outTangent.size() != key.value.size()){
                    warn(warnings, keyPath, "tangents must have the value's size: tangents ignored");
                    ok = false;
                }
                if (ok && (!allFinite(key.inTangent) || !allFinite(key.outTangent))){
                    warn(warnings, keyPath, "tangents are not finite numbers: tangents ignored");
                    ok = false;
                }
                if (!ok){
                    key.inTangent.clear();
                    key.outTangent.clear();
                }
            }
            if (key.ease == EaseType::CUSTOM || key.ease == EaseType::CURVE){
                key.ease = EaseType::LINEAR;
            }
            // a Constant key's step stays: its segment holds the key's value (S21 R27)
            if (key.tangentMode == ClipTangentMode::Constant) key.ease = EaseType::STEP;
            if (key.hasTangents() && key.ease != EaseType::LINEAR && key.tangentMode != ClipTangentMode::Constant &&
                    key.ease != EaseType::STEP){
                warn(warnings, member(keyPath, "ease"), "tangents win over the ease: ease ignored");
                key.ease = EaseType::LINEAR;
            }
            if (key.time < 0.0f){
                warn(warnings, member(keyPath, "time"), "negative time " + shortNumber(key.time) + ", using 0");
                key.time = 0.0f;
            }
            key.time = quantizeTime(key.time);
            kept.push_back(std::move(key));
            keptOrigins.push_back(origin.keys[k]);
        }
        track.keys = std::move(kept);
        origin.keys = std::move(keptOrigins);
    }

    // Step 3: keys sorted by time (stable: file order at equal times), equal times collapse
    // into the later key
    void sortAndCollapseKeys(ClipTrackData& track, TrackOrigin& origin, std::vector<ClipWarning>* warnings){
        std::vector<size_t> order(track.keys.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        bool sorted = true;
        for (size_t i = 1; i < track.keys.size(); i++){
            if (track.keys[i].time < track.keys[i - 1].time) sorted = false;
        }
        if (!sorted){
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){ return track.keys[a].time < track.keys[b].time; });
        }
        std::vector<ClipKey> keys;
        std::vector<std::string> origins;
        for (size_t i : order){
            if (!keys.empty() && keys.back().time == track.keys[i].time){
                warn(warnings, origins.back(),
                     "at the time of another key (closer than 1e-6 s): merged, the later key wins");
                keys.back() = track.keys[i];
                origins.back() = origin.keys[i];
                continue;
            }
            keys.push_back(track.keys[i]);
            origins.push_back(origin.keys[i]);
        }
        track.keys = std::move(keys);
        origin.keys = std::move(origins);
    }

    // Step 5: a track with tangents on some keys only: the others get the tangents of the
    // straight line to their neighbours (the segments between two of them stay linear)
    void completeTangents(ClipTrackData& track, const TrackOrigin& origin, std::vector<ClipWarning>* warnings){
        size_t with = 0;
        for (const ClipKey& key : track.keys){
            if (key.hasTangents()) with++;
        }
        if (with == 0 || with == track.keys.size()) return;
        warn(warnings, member(origin.path, "keys"), "some keys have tangents and others not: the others got linear tangents");
        // a rotation key getting tangents made from the values takes the side of its neighbour
        // towards the keys with stored tangents first (its sign is free: no stored tangent depends
        // on it), so its segments don't flip: after the first key with tangents each takes the key
        // before it, before it (key 0 too) each takes the key after it
        if (isRotationChannel(track.channel)){
            size_t first = 0;
            while (first < track.keys.size() && !track.keys[first].hasTangents()) first++;
            for (size_t k = first + 1; k < track.keys.size(); k++){
                if (!track.keys[k].hasTangents()) anim::alignQuaternionKey(track.keys[k].value, track.keys[k - 1].value);
            }
            for (size_t k = first; k-- > 0;){
                anim::alignQuaternionKey(track.keys[k].value, track.keys[k + 1].value);
            }
        }
        for (size_t k = 0; k < track.keys.size(); k++){
            ClipKey& key = track.keys[k];
            if (key.hasTangents()) continue;
            size_t width = key.value.size();
            std::vector<float> before(width, 0.0f);
            std::vector<float> after(width, 0.0f);
            if (k > 0){
                const ClipKey& previous = track.keys[k - 1];
                float dt = key.time - previous.time;
                for (size_t i = 0; i < width && dt > 0; i++) before[i] = (key.value[i] - previous.value[i]) / dt;
            }
            if (k + 1 < track.keys.size()){
                const ClipKey& next = track.keys[k + 1];
                float dt = next.time - key.time;
                for (size_t i = 0; i < width && dt > 0; i++) after[i] = (next.value[i] - key.value[i]) / dt;
            }
            key.inTangent = k > 0 ? before : after;
            key.outTangent = k + 1 < track.keys.size() ? after : before;
            // tangents win over the ease (a Constant key's step stays)
            if (key.ease != EaseType::LINEAR && key.tangentMode != ClipTangentMode::Constant && key.ease != EaseType::STEP){
                warn(warnings, member(origin.keys[k], "ease"), "ignored: the track uses tangents");
                key.ease = EaseType::LINEAR;
            }
        }
    }

    void normalizeClip(ClipData& data, std::vector<ClipWarning>* warnings, ClipOrigins* given){
        ClipOrigins defaults;
        ClipOrigins& origins = given ? *given : defaults;
        if (origins.tracks.size() != data.tracks.size()){
            origins.tracks.clear();
            for (size_t i = 0; i < data.tracks.size(); i++){
                TrackOrigin origin;
                origin.path = "tracks[" + std::to_string(i) + "]";
                for (size_t k = 0; k < data.tracks[i].keys.size(); k++) origin.keys.push_back(indexed(origin.path, "keys", k));
                origins.tracks.push_back(origin);
            }
        }
        if (origins.events.size() != data.events.size()){
            origins.events.clear();
            for (size_t i = 0; i < data.events.size(); i++) origins.events.push_back(i);
        }
        if (origins.audio.size() != data.audio.size()){
            origins.audio.clear();
            for (size_t i = 0; i < data.audio.size(); i++) origins.audio.push_back(i);
        }

        // settings
        if (!std::isfinite(data.frameRate)){
            warn(warnings, "frame_rate", "not a number, using 30");
            data.frameRate = 30.0f;
        }else if (data.frameRate < anim::ANIM_MIN_FRAME_RATE || data.frameRate > anim::ANIM_MAX_FRAME_RATE){
            float clamped = std::min(std::max(data.frameRate, anim::ANIM_MIN_FRAME_RATE), anim::ANIM_MAX_FRAME_RATE);
            warn(warnings, "frame_rate", shortNumber(data.frameRate) + " is outside 1..240, using " + shortNumber(clamped));
            data.frameRate = clamped;
        }
        if (data.wrap == ClipWrapMode::Default){
            warn(warnings, "wrap", "\"default\" is for Animation Player entries only: using \"once\"");
            data.wrap = ClipWrapMode::Once;
        }
        if (!std::isfinite(data.length)){
            warn(warnings, "length", "not a number: automatic length");
            data.length = -1.0f;
        }else if (data.length < 0.0f){
            data.length = -1.0f;
        }else{
            data.length = quantizeTime(data.length);
        }
        if (!data.guid.empty() && !anim::isClipGuid(data.guid)){
            warn(warnings, "guid", "not 32 lowercase hex digits: dropped");
            data.guid.clear();
        }

        // humanoid tracks (anim-retarget 6.1): a known bone, no path; other tracks have no bone
        {
            std::vector<ClipTrackData> kept;
            std::vector<TrackOrigin> keptOrigins;
            for (size_t i = 0; i < data.tracks.size(); i++){
                ClipTrackData& track = data.tracks[i];
                if (isHumanoidChannel(track.channel)){
                    if (anim::humanoidTrackBoneName(track.channel, track.humanBone)[0] == '\0'){
                        warn(warnings, member(origins.tracks[i].path, "bone"), "not a humanoid bone, track dropped");
                        continue;
                    }
                    if (!track.path.empty()){
                        warn(warnings, member(origins.tracks[i].path, "path"), "humanoid tracks have no path (they are identified by \"bone\"): path ignored");
                        track.path.clear();
                    }
                    track.points.clear();
                }else{
                    track.humanBone = -1;
                }
                kept.push_back(std::move(track));
                keptOrigins.push_back(std::move(origins.tracks[i]));
            }
            data.tracks = std::move(kept);
            origins.tracks = std::move(keptOrigins);
        }

        // tracks, step 1: every key on its own (a property track's identity first)
        for (size_t i = 0; i < data.tracks.size(); i++){
            if (data.tracks[i].channel == ClipChannel::Property){
                resolvePropertyTrack(data.tracks[i], origins.tracks[i], warnings);
            }
            if (data.tracks[i].channel != ClipChannel::Property || data.tracks[i].known){
                sanitizeKeys(data.tracks[i], origins.tracks[i], warnings);
                // reported per track as written (a merge concatenates sorted tracks out of order)
                const std::vector<ClipKey>& keys = data.tracks[i].keys;
                for (size_t k = 1; k < keys.size(); k++){
                    if (keys[k].time < keys[k - 1].time){
                        warn(warnings, member(origins.tracks[i].path, "keys"), "keys were not sorted by time: sorted");
                        break;
                    }
                }
            }
        }

        // step 2: tracks in canonical order; duplicates merge (later keys after earlier ones,
        // so the later track wins at equal times)
        std::vector<size_t> order(data.tracks.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){ return trackLess(data.tracks[a], data.tracks[b]); });
        std::vector<ClipTrackData> tracks;
        std::vector<TrackOrigin> trackOrigins;
        for (size_t i : order){
            ClipTrackData& track = data.tracks[i];
            TrackOrigin& origin = origins.tracks[i];
            if (tracks.empty() || !sameTrack(tracks.back(), track)){
                tracks.push_back(std::move(track));
                trackOrigins.push_back(std::move(origin));
                continue;
            }
            ClipTrackData& into = tracks.back();
            TrackOrigin& intoOrigin = trackOrigins.back();
            if (track.channel == ClipChannel::Property){
                warn(warnings, origin.path, "the same property track as " + intoOrigin.path + ": the later one wins");
                into = std::move(track);
                intoOrigin = std::move(origin);
                continue;
            }
            warn(warnings, origin.path, "the same path and channel as " + intoOrigin.path +
                                        ": merged, the later keys win at equal times");
            if (track.channel == ClipChannel::Lattice && track.points != into.points){
                std::vector<int> points = into.points;
                points.insert(points.end(), track.points.begin(), track.points.end());
                std::sort(points.begin(), points.end());
                points.erase(std::unique(points.begin(), points.end()), points.end());
                for (ClipKey& key : into.keys){
                    key.value = remapLatticeRow(key.value, into.points, points);
                    key.inTangent = remapLatticeRow(key.inTangent, into.points, points);
                    key.outTangent = remapLatticeRow(key.outTangent, into.points, points);
                }
                for (ClipKey& key : track.keys){
                    key.value = remapLatticeRow(key.value, track.points, points);
                    key.inTangent = remapLatticeRow(key.inTangent, track.points, points);
                    key.outTangent = remapLatticeRow(key.outTangent, track.points, points);
                }
                into.points = points;
            }
            into.relative = track.relative;
            into.loop = track.loop;
            into.keys.insert(into.keys.end(), track.keys.begin(), track.keys.end());
            intoOrigin.keys.insert(intoOrigin.keys.end(), origin.keys.begin(), origin.keys.end());
        }

        // steps 3-5 per track
        for (size_t t = 0; t < tracks.size(); t++){
            ClipTrackData& track = tracks[t];
            TrackOrigin& origin = trackOrigins[t];
            if (track.channel == ClipChannel::Property && !track.known) continue;

            if (track.channel == ClipChannel::Morph){
                // shorter arrays mean 0 for the rest
                size_t width = 0;
                for (const ClipKey& key : track.keys) width = std::max(width, key.value.size());
                for (ClipKey& key : track.keys){
                    key.value.resize(width, 0.0f);
                    if (key.hasTangents()){
                        key.inTangent.resize(width, 0.0f);
                        key.outTangent.resize(width, 0.0f);
                    }
                }
            }

            sortAndCollapseKeys(track, origin, warnings);

            if (isRotationChannel(track.channel)){
                for (size_t k = 0; k < track.keys.size(); k++){
                    ClipKey& key = track.keys[k];
                    float length = quaternionLength(key.value);
                    if (std::fabs(length - 1.0f) > 1e-5f){
                        warn(warnings, member(origin.keys[k], "value"),
                             "not a unit quaternion (length " + shortNumber(length) + "): normalized");
                        scaleRotationKey(key, 1.0f / length);
                    }
                }
                enforceHemisphere(track);
            }

            // the keys' tangent modes (the editor's): computed tangents follow the keys
            anim::applyTangentModes(track);
            completeTangents(track, origin, warnings);
            // the editor hint only on keys with tangents
            for (ClipKey& key : track.keys){
                if (!key.hasTangents()) key.tangentMode = ClipTangentMode::None;
            }
        }
        data.tracks = std::move(tracks);

        // events
        std::vector<ClipEventData> events;
        std::vector<size_t> eventOrigins;
        for (size_t i = 0; i < data.events.size(); i++){
            ClipEventData event = data.events[i];
            std::string path = "events[" + std::to_string(origins.events[i]) + "]";
            if (!std::isfinite(event.time)){
                warn(warnings, member(path, "time"), "not a number, event dropped");
                continue;
            }
            if (!anim::isLuaIdentifier(event.function)){
                warn(warnings, member(path, "function"), "\"" + event.function + "\" is not a Lua function name, event dropped");
                continue;
            }
            if (event.args.size() > MAX_ANIMATION_EVENT_ARGS){
                warn(warnings, member(path, "args"), "more than " + std::to_string(MAX_ANIMATION_EVENT_ARGS) +
                                                     " args: the rest are dropped");
                event.args.resize(MAX_ANIMATION_EVENT_ARGS);
            }
            for (AnimationEventArg& arg : event.args){
                if (std::holds_alternative<double>(arg) && !std::isfinite(std::get<double>(arg))){
                    warn(warnings, member(path, "args"), "not a finite number, using 0");
                    arg = AnimationEvent::numberArg(0.0);
                }
            }
            if (event.time < 0.0f){
                warn(warnings, member(path, "time"), "negative time " + shortNumber(event.time) + ", using 0");
                event.time = 0.0f;
            }
            event.time = quantizeTime(event.time);
            events.push_back(std::move(event));
            eventOrigins.push_back(origins.events[i]);
        }
        std::vector<size_t> eventOrder(events.size());
        for (size_t i = 0; i < eventOrder.size(); i++) eventOrder[i] = i;
        std::stable_sort(eventOrder.begin(), eventOrder.end(), [&](size_t a, size_t b){
            if (events[a].time != events[b].time) return events[a].time < events[b].time;
            return events[a].function < events[b].function;
        });
        data.events.clear();
        origins.events.clear();
        for (size_t i : eventOrder){
            data.events.push_back(std::move(events[i]));
            origins.events.push_back(eventOrigins[i]);
        }
        origins.tracks = std::move(trackOrigins);

        // audio cues (S40 R2): an unplayable cue is dropped, a value out of range clamped
        std::vector<ClipAudioCue> cues;
        std::vector<size_t> cueOrigins;
        for (size_t i = 0; i < data.audio.size(); i++){
            ClipAudioCue cue = data.audio[i];
            const std::string path = "audio[" + std::to_string(origins.audio[i]) + "]";
            if (!std::isfinite(cue.time)){
                warn(warnings, member(path, "time"), "not a number, cue dropped");
                continue;
            }
            const std::string problem = anim::validateCue(cue);
            if (!problem.empty()){
                warn(warnings, path, problem + ", cue dropped");
                continue;
            }
            if (cue.time < 0.0f){
                warn(warnings, member(path, "time"), "negative time " + shortNumber(cue.time) + ", using 0");
                cue.time = 0.0f;
            }
            cue.time = quantizeTime(cue.time);
            auto clampValue = [&](float& value, float low, float high, const char* key){
                const float clamped = std::isfinite(value) ? std::min(std::max(value, low), high) : 0.0f;
                if (clamped != value){
                    warn(warnings, member(path, key), shortNumber(value) + " is outside " + shortNumber(low) + ".." + shortNumber(high) +
                                                      ", using " + shortNumber(clamped));
                }
                value = quantizeValue(clamped);
            };
            clampValue(cue.volumeDb, anim::CUE_MIN_VOLUME_DB, anim::CUE_MAX_VOLUME_DB, "volume_db");
            clampValue(cue.pitchSemitones, -anim::CUE_MAX_PITCH_SEMITONES, anim::CUE_MAX_PITCH_SEMITONES, "pitch_semitones");
            clampValue(cue.seconds, 0.0f, FLT_MAX, "seconds");
            // the fields another kind uses mean nothing here: dropped, so the file says one thing
            if (cue.kind != ClipCueKind::Sound){
                cue.sound.clear();
                cue.group.clear();
                cue.spatial = false;
                cue.volumeDb = 0.0f;
                cue.pitchSemitones = 0.0f;
            }
            if (cue.kind == ClipCueKind::Snapshot){
                cue.path.clear();
                cue.followSpeed = false;
                cue.stop = ClipCueStop::None;
            }else{
                cue.snapshot.clear();
                cue.seconds = 0.0f;
                cue.unscaled.reset();
            }
            if (cue.kind == ClipCueKind::Sound && !cue.spatial) cue.path.clear();
            cues.push_back(std::move(cue));
            cueOrigins.push_back(origins.audio[i]);
        }
        std::vector<size_t> cueOrder(cues.size());
        for (size_t i = 0; i < cueOrder.size(); i++) cueOrder[i] = i;
        std::stable_sort(cueOrder.begin(), cueOrder.end(), [&](size_t a, size_t b){
            if (cues[a].time != cues[b].time) return cues[a].time < cues[b].time;
            return (int)cues[a].kind < (int)cues[b].kind;
        });
        data.audio.clear();
        origins.audio.clear();
        for (size_t i : cueOrder){
            data.audio.push_back(std::move(cues[i]));
            origins.audio.push_back(cueOrigins[i]);
        }

        if (!std::isfinite(data.humanoid.humanScale) || data.humanoid.humanScale < 0.0f){
            warn(warnings, "humanoid.human_scale", "expected a positive number: dropped");
            data.humanoid.humanScale = 0.0f;
        }
        if (data.source.kind != "" && data.source.kind != "model" && data.source.kind != "entity" && data.source.kind != "pose"){
            warn(warnings, "source.kind", "expected model, entity or pose: source dropped");
            data.source = ClipSource();
        }

        // the additive reference time (anim layers spec 6.1): in [0, length]
        if (!std::isfinite(data.additiveReference)){
            warn(warnings, "additive_reference", "not a number, using 0");
            data.additiveReference = 0.0f;
        }else{
            const float length = data.effectiveLength();
            const float clamped = std::min(std::max(data.additiveReference, 0.0f), length);
            if (clamped != data.additiveReference){
                warn(warnings, "additive_reference", shortNumber(data.additiveReference) + " is outside the clip (0.." +
                                                     shortNumber(length) + "), using " + shortNumber(clamped));
            }
            data.additiveReference = quantizeTime(clamped);
        }
    }

    // ---- parsing ----

    // one key value (or tangent) as read: dense floats, or the sparse lattice points
    struct ParsedValue{
        std::vector<float> values;
        std::vector<std::pair<int, std::array<float, 3>>> points;
        bool euler = false;
        float eulerAngles[3] = {0, 0, 0};
    };

    // A property track's value context: the kind its keys have (None: taken from the first key
    // read, a script field or a shader member)
    struct PropertyValueContext{
        PropertyKind kind = PropertyKind::None;
        size_t width = 0;
    };

    // one property key value (or tangent) in key space
    bool readPropertyValue(const OJson& json, PropertyValueContext& context, bool tangent, const std::string& path,
                           ParsedValue& out, std::vector<ClipWarning>& warnings){
        out.values.clear();
        PropertyKind kind = context.kind;
        if (kind == PropertyKind::None && !tangent){
            // the first key decides: true/false a bool, a number a float, n numbers a vector,
            // "#RRGGBB[AA]" a colour
            if (json.is_boolean()) kind = PropertyKind::Bool;
            else if (json.is_number()) kind = PropertyKind::Float;
            else if (json.is_string()) kind = json.get<std::string>().size() == 9 ? PropertyKind::Color4 : PropertyKind::Color3;
            else if (json.is_array() && json.size() == 1) kind = PropertyKind::Float;
            else if (json.is_array() && json.size() >= 2 && json.size() <= 4){
                kind = json.size() == 2 ? PropertyKind::Vector2 : json.size() == 3 ? PropertyKind::Vector3 : PropertyKind::Vector4;
            }else{
                warn(&warnings, path, "expected true/false, a number, an array of 2 to 4 numbers or \"#RRGGBB[AA]\"");
                return false;
            }
        }
        const size_t width = anim::kindWidth(kind);
        switch (kind){
            case PropertyKind::Bool: {
                if (tangent){
                    warn(&warnings, path, "a bool has no tangents");
                    return false;
                }
                if (json.is_boolean()){
                    out.values.push_back(json.get<bool>() ? 1.0f : 0.0f);
                }else{
                    float number = 0.0f;
                    if (!toFloat(json, number)){
                        warn(&warnings, path, "expected true or false");
                        return false;
                    }
                    if (number != 0.0f && number != 1.0f) warn(&warnings, path, "expected true or false (or 0 / 1): " + shortNumber(number) + " read as " + (number != 0.0f ? "true" : "false"));
                    out.values.push_back(number != 0.0f ? 1.0f : 0.0f);
                }
                break;
            }
            case PropertyKind::Int:
            case PropertyKind::Frame:
            case PropertyKind::Float: {
                float number = 0.0f;
                if (json.is_array() && json.size() == 1 && toFloat(json[0], number)){
                    out.values.push_back(number);
                }else if (toFloat(json, number)){
                    out.values.push_back(number);
                }else{
                    warn(&warnings, path, kind == PropertyKind::Float ? "expected a number" : "expected a whole number");
                    return false;
                }
                break;
            }
            case PropertyKind::Vector2:
            case PropertyKind::Vector3:
            case PropertyKind::Vector4:
                if (!readNumbers(json, width, out.values)){
                    warn(&warnings, path, "expected an array of " + std::to_string(width) + " numbers");
                    return false;
                }
                break;
            case PropertyKind::Color3:
            case PropertyKind::Color4: {
                if (json.is_string() && !tangent){
                    if (!anim::parseHexColor(json.get<std::string>(), out.values)){
                        warn(&warnings, path, "expected \"#RRGGBB\" or \"#RRGGBBAA\"");
                        return false;
                    }
                }else if (!readNumbers(json, 0, out.values) || (out.values.size() != 3 && out.values.size() != 4)){
                    warn(&warnings, path, kind == PropertyKind::Color4 ? "expected [r, g, b, a] (sRGB), [r, g, b] or \"#RRGGBB[AA]\""
                                                                      : "expected [r, g, b] (sRGB) or \"#RRGGBB\"");
                    return false;
                }
                if (kind == PropertyKind::Color4 && out.values.size() == 3){
                    // a colour without alpha is opaque (tangents: no change of alpha)
                    out.values.push_back(tangent ? 0.0f : 1.0f);
                }else if (kind == PropertyKind::Color3 && out.values.size() == 4){
                    warn(&warnings, path, "the property has no alpha: dropped");
                    out.values.resize(3);
                }
                break;
            }
            case PropertyKind::None:
                return false;
        }
        if (context.kind == PropertyKind::None && !tangent){
            context.kind = kind;
            context.width = width;
        }
        return true;
    }

    bool readValue(const OJson& json, ClipChannel channel, int humanBone, bool tangent, const std::string& path,
                   ParsedValue& out, std::vector<ClipWarning>& warnings, PropertyValueContext* property = nullptr){
        switch (channel){
            case ClipChannel::Humanoid:
                // a slot's joint angles (the pose system's DOFs, degrees), written back as E
                if (!tangent && json.is_object() && json.contains("angles")){
                    std::vector<float> angles;
                    if (humanBone < 0 || humanBone >= pose::HUMAN_BONE_COUNT){
                        warn(&warnings, path, "{\"angles\"} is for the 22 body slots: an extra bone takes [x, y, z, w]");
                        return false;
                    }
                    if (json.size() != 1 || !readNumbers(json["angles"], 3, angles)){
                        const pose::BoneDofInfo& dofs = pose::boneDofInfo(pose::boneAt(humanBone));
                        warn(&warnings, path, std::string("expected {\"angles\": [") + dofs.dofs[0].name + ", " + dofs.dofs[1].name + ", " +
                                              dofs.dofs[2].name + "]} degrees (" + pose::humanBoneName(pose::boneAt(humanBone)) + "'s joint angles)");
                        return false;
                    }
                    out.values.resize(4);
                    quaternionToXYZW(pose::jointRotation(pose::boneAt(humanBone), angles.data()).normalized().toQuaternion(), out.values.data());
                    return true;
                }
                if (!readNumbers(json, 4, out.values)){
                    warn(&warnings, path, tangent || humanBone >= pose::HUMAN_BONE_COUNT ? "expected [x, y, z, w]"
                                                                                   : "expected [x, y, z, w] or {\"angles\": [dof1, dof2, twist]} degrees");
                    return false;
                }
                return true;
            case ClipChannel::Position:
            case ClipChannel::Scale:
            case ClipChannel::HumanoidPosition:
                if (!readNumbers(json, 3, out.values)){
                    warn(&warnings, path, "expected [x, y, z]");
                    return false;
                }
                return true;
            case ClipChannel::Rotation:
                if (!tangent && json.is_object() && json.contains("euler")){
                    std::vector<float> euler;
                    if (json.size() != 1 || !readNumbers(json["euler"], 3, euler)){
                        warn(&warnings, path, "expected {\"euler\": [x, y, z]} degrees");
                        return false;
                    }
                    // degrees whatever Engine::useDegrees says, in edit_animation's order
                    out.values.resize(4);
                    quaternionToXYZW(anim::eulerDegreesToQuaternion(euler[0], euler[1], euler[2]), out.values.data());
                    out.euler = true;
                    for (int i = 0; i < 3; i++) out.eulerAngles[i] = euler[i];
                    return true;
                }
                if (!readNumbers(json, 4, out.values)){
                    warn(&warnings, path, tangent ? "expected [x, y, z, w]" : "expected [x, y, z, w] or {\"euler\": [x, y, z]} degrees");
                    return false;
                }
                return true;
            case ClipChannel::Morph:
                if (!readNumbers(json, 0, out.values)){
                    warn(&warnings, path, "expected an array of morph weights");
                    return false;
                }
                return true;
            case ClipChannel::Lattice:{
                if (!json.is_object() || !json.contains("points") || !json["points"].is_array()){
                    warn(&warnings, path, "expected {\"points\": [[flatIndex, [x, y, z]], ...]}");
                    return false;
                }
                for (auto it = json.begin(); it != json.end(); ++it){
                    if (it.key() != "points") warn(&warnings, member(path, it.key().c_str()), "unknown key ignored");
                }
                const OJson& points = json["points"];
                for (size_t i = 0; i < points.size(); i++){
                    std::string pointPath = indexed(path, "points", i);
                    const OJson& entry = points[i];
                    std::vector<float> offset;
                    float index = -1;
                    if (!entry.is_array() || entry.size() != 2 || !toFloat(entry[0], index) || index < 0 ||
                        index != std::floor(index) || index > 1e8f || !readNumbers(entry[1], 3, offset)){
                        warn(&warnings, pointPath, "expected [flatIndex, [x, y, z]], point dropped");
                        continue;
                    }
                    int point = (int)index;
                    bool duplicate = false;
                    for (auto& existing : out.points){
                        if (existing.first == point){
                            warn(&warnings, pointPath, "point " + std::to_string(point) + " listed twice: the later one wins");
                            existing.second = {offset[0], offset[1], offset[2]};
                            duplicate = true;
                        }
                    }
                    if (!duplicate) out.points.push_back({point, {offset[0], offset[1], offset[2]}});
                }
                return true;
            }
            case ClipChannel::Property:
                if (!property) return false;
                return readPropertyValue(json, *property, tangent, path, out, warnings);
        }
        return false;
    }

    struct ParsedKey{
        float time = 0.0f;
        EaseType ease = EaseType::LINEAR;
        ParsedValue value;
        ParsedValue in;
        ParsedValue out;
        bool tangents = false;
        ClipTangentMode tangentMode = ClipTangentMode::None;
        size_t origin = 0;
    };

    bool readEase(const OJson& json, const std::string& path, EaseType& out, std::vector<ClipWarning>& warnings){
        if (!json.is_string() || !anim::easeFromName(json.get<std::string>(), out)){
            warn(&warnings, path, "expected an ease name (linear, step, quad_out, ...): using linear");
            out = EaseType::LINEAR;
            return false;
        }
        return true;
    }

    bool readKey(const OJson& json, ClipChannel channel, int humanBone, const std::string& path, ParsedKey& key,
                 std::vector<ClipWarning>& warnings, PropertyValueContext* property = nullptr){
        const OJson* time = nullptr;
        const OJson* value = nullptr;
        const OJson* ease = nullptr;
        const OJson* in = nullptr;
        const OJson* out = nullptr;
        std::string valuePath = member(path, "value");
        std::string easePath = member(path, "ease");
        if (json.is_array()){
            if (json.size() != 2 && json.size() != 3){
                warn(&warnings, path, "expected [time, value] or [time, value, ease], key dropped");
                return false;
            }
            time = &json[0];
            value = &json[1];
            if (json.size() == 3) ease = &json[2];
        }else if (json.is_object()){
            for (auto it = json.begin(); it != json.end(); ++it){
                const std::string& name = it.key();
                if (name == "time") time = &*it;
                else if (name == "value") value = &*it;
                else if (name == "ease") ease = &*it;
                else if (name == "in") in = &*it;
                else if (name == "out") out = &*it;
                else if (name == "tangent_mode"){
                    if (!it->is_string() || !anim::tangentModeFromName(it->get<std::string>(), key.tangentMode)){
                        warn(&warnings, member(path, "tangent_mode"), "expected clamped_auto, auto, free, linear, constant, flat or broken: ignored");
                    }
                }
                else warn(&warnings, member(path, name.c_str()), "unknown key ignored");
            }
            if (!value){
                warn(&warnings, path, "missing \"value\", key dropped");
                return false;
            }
        }else{
            warn(&warnings, path, "expected [time, value] or {\"time\", \"value\", ...}, key dropped");
            return false;
        }
        if (!time || !toFloat(*time, key.time)){
            warn(&warnings, member(path, "time"), "expected a time in seconds, key dropped");
            return false;
        }
        if (!readValue(*value, channel, humanBone, false, valuePath, key.value, warnings, property)){
            warn(&warnings, path, "key dropped");
            return false;
        }
        if (ease) readEase(*ease, easePath, key.ease, warnings);
        if (in || out){
            bool ok = in && out;
            if (!ok){
                warn(&warnings, path, "in and out tangents go together: tangents ignored");
            }else{
                ok = readValue(*in, channel, humanBone, true, member(path, "in"), key.in, warnings, property) &&
                     readValue(*out, channel, humanBone, true, member(path, "out"), key.out, warnings, property);
                if (!ok) warn(&warnings, path, "tangents ignored");
            }
            key.tangents = ok;
        }
        return true;
    }

    // Euler angles (degrees) of the keys written as Euler, by key path
    using EulerKeys = std::map<std::string, std::array<float, 3>>;

    bool readTrack(const OJson& json, size_t index, ClipTrackData& track, TrackOrigin& origin,
                   EulerKeys& eulerKeys, std::vector<ClipWarning>& warnings){
        std::string path = "tracks[" + std::to_string(index) + "]";
        origin.path = path;
        if (!json.is_object()){
            warn(&warnings, path, "expected a track object, track dropped");
            return false;
        }
        auto channel = json.find("channel");
        if (channel == json.end() || !channel->is_string() || !anim::channelFromName(channel->get<std::string>(), track.channel)){
            warn(&warnings, member(path, "channel"), "expected position, rotation, scale, morph, lattice, property, humanoid or humanoid_position, track dropped");
            return false;
        }
        auto trackPath = json.find("path");
        if (isHumanoidChannel(track.channel)){
            // identified by its bone (anim-retarget 6.1)
            if (trackPath != json.end()){
                warn(&warnings, member(path, "path"), "humanoid tracks are identified by \"bone\", not a path: track dropped");
                return false;
            }
            auto bone = json.find("bone");
            if (bone == json.end() || !bone->is_string()){
                warn(&warnings, member(path, "bone"), "missing: a humanoid track names its bone (left_upper_arm, hips...), track dropped");
                return false;
            }
            if (!anim::humanoidTrackBoneFromName(track.channel, bone->get<std::string>(), track.humanBone)){
                warn(&warnings, member(path, "bone"), track.channel == ClipChannel::Humanoid
                     ? "\"" + bone->get<std::string>() + "\" is not a humanoid slot or extra bone, track dropped"
                     : "\"" + bone->get<std::string>() + "\" is not hips, left_foot or right_foot, track dropped");
                return false;
            }
        }else if (trackPath == json.end()){
            warn(&warnings, member(path, "path"), "missing: the track animates the root");
        }else if (!trackPath->is_string()){
            warn(&warnings, member(path, "path"), "expected a string, track dropped");
            return false;
        }else{
            track.path = trackPath->get<std::string>();
        }

        PropertyValueContext propertyContext;
        if (track.channel == ClipChannel::Property){
            auto component = json.find("component");
            auto property = json.find("property");
            if (component != json.end() && component->is_string()) track.component = component->get<std::string>();
            if (property != json.end() && property->is_string()) track.property = property->get<std::string>();
            anim::PropertyRef ref;
            if (!anim::parsePropertyRef(track.component, track.property, ref)){
                // not animatable (normalize says so): kept as read, written back verbatim
                track.preserved = json.dump(-1, ' ', false, OJson::error_handler_t::replace);
                return true;
            }
            propertyContext.kind = ref.swizzle >= 0 ? PropertyKind::Float : ref.kind;
            propertyContext.width = anim::kindWidth(propertyContext.kind);
        }

        for (auto it = json.begin(); it != json.end(); ++it){
            const std::string& name = it.key();
            if (name == "path" || name == "channel" || name == "keys" || (name == "bone" && isHumanoidChannel(track.channel))) continue;
            if (track.channel == ClipChannel::Property && (name == "component" || name == "property")) continue;
            if (name == "relative" || name == "loop"){
                if (!it->is_boolean()){
                    warn(&warnings, member(path, name.c_str()), "expected true or false, ignored");
                }else if (name == "relative"){
                    track.relative = it->get<bool>();
                }else{
                    track.loop = it->get<bool>();
                }
                continue;
            }
            warn(&warnings, member(path, name.c_str()), "unknown key ignored");
        }

        auto keys = json.find("keys");
        if (keys == json.end()){
            warn(&warnings, member(path, "keys"), "missing: the track has no keys");
            return true;
        }
        if (!keys->is_array()){
            warn(&warnings, member(path, "keys"), "expected an array: the track has no keys");
            return true;
        }

        std::vector<ParsedKey> parsed;
        for (size_t k = 0; k < keys->size(); k++){
            std::string keyPath = indexed(path, "keys", k);
            ParsedKey key;
            key.origin = k;
            if (!readKey((*keys)[k], track.channel, track.humanBone, keyPath, key, warnings,
                         track.channel == ClipChannel::Property ? &propertyContext : nullptr)){
                parsed.push_back(ParsedKey());
                parsed.back().origin = SIZE_MAX;
                continue;
            }
            // the >180 degrees check runs on the keys in time order, after normalize
            if (key.value.euler){
                eulerKeys[keyPath] = {key.value.eulerAngles[0], key.value.eulerAngles[1], key.value.eulerAngles[2]};
            }
            parsed.push_back(key);
        }

        if (track.channel == ClipChannel::Lattice){
            // the track's points are the union of its keys' points; a key lacking one holds 0
            std::vector<int> points;
            for (const ParsedKey& key : parsed){
                if (key.origin == SIZE_MAX) continue;
                for (const auto& point : key.value.points) points.push_back(point.first);
            }
            std::sort(points.begin(), points.end());
            points.erase(std::unique(points.begin(), points.end()), points.end());
            track.points = points;
            auto dense = [&](const ParsedValue& value, const std::string& where, bool report){
                std::vector<float> row(3 * points.size(), 0.0f);
                std::vector<bool> seen(points.size(), false);
                for (const auto& point : value.points){
                    size_t j = std::lower_bound(points.begin(), points.end(), point.first) - points.begin();
                    if (j >= points.size() || points[j] != point.first){
                        warn(&warnings, where, "tangent of point " + std::to_string(point.first) +
                                               ", which no key's value lists: dropped");
                        continue;
                    }
                    for (int c = 0; c < 3; c++) row[3 * j + c] = point.second[c];
                    seen[j] = true;
                }
                if (report){
                    for (size_t j = 0; j < points.size(); j++){
                        if (!seen[j]) warn(&warnings, where, "point " + std::to_string(points[j]) + " missing, using 0");
                    }
                }
                return row;
            };
            for (ParsedKey& key : parsed){
                if (key.origin == SIZE_MAX) continue;
                std::string keyPath = indexed(path, "keys", key.origin);
                key.value.values = dense(key.value, keyPath, true);
                if (key.tangents){
                    key.in.values = dense(key.in, member(keyPath, "in"), false);
                    key.out.values = dense(key.out, member(keyPath, "out"), false);
                }
            }
        }

        for (ParsedKey& key : parsed){
            if (key.origin == SIZE_MAX) continue;
            ClipKey out;
            out.time = key.time;
            out.ease = key.ease;
            out.tangentMode = key.tangentMode;
            out.value = std::move(key.value.values);
            if (key.tangents){
                out.inTangent = std::move(key.in.values);
                out.outTangent = std::move(key.out.values);
            }
            out.tangentMode = key.tangentMode;
            track.keys.push_back(std::move(out));
            origin.keys.push_back(indexed(path, "keys", key.origin));
        }
        // a script field's or shader member's kind, as its keys said (normalize keeps it)
        if (track.channel == ClipChannel::Property) track.kind = propertyContext.kind;
        return true;
    }

    bool readEvent(const OJson& json, size_t index, ClipEventData& event, std::vector<ClipWarning>& warnings){
        std::string path = "events[" + std::to_string(index) + "]";
        if (!json.is_object()){
            warn(&warnings, path, "expected {\"time\", \"function\", \"args\"}, event dropped");
            return false;
        }
        for (auto it = json.begin(); it != json.end(); ++it){
            const std::string& name = it.key();
            if (name != "time" && name != "function" && name != "args"){
                warn(&warnings, member(path, name.c_str()), "unknown key ignored");
            }
        }
        auto time = json.find("time");
        if (time == json.end() || !toFloat(*time, event.time)){
            warn(&warnings, member(path, "time"), "expected a time in seconds, event dropped");
            return false;
        }
        auto function = json.find("function");
        if (function == json.end() || !function->is_string()){
            warn(&warnings, member(path, "function"), "expected a Lua function name, event dropped");
            return false;
        }
        event.function = function->get<std::string>();
        auto args = json.find("args");
        if (args != json.end()){
            if (!args->is_array()){
                warn(&warnings, member(path, "args"), "expected an array of numbers, strings and booleans, ignored");
            }else{
                for (size_t i = 0; i < args->size(); i++){
                    const OJson& arg = (*args)[i];
                    if (arg.is_boolean()){
                        event.args.push_back(AnimationEvent::boolArg(arg.get<bool>()));
                    }else if (arg.is_number()){
                        event.args.push_back(AnimationEvent::numberArg(arg.get<double>()));
                    }else if (arg.is_string()){
                        event.args.push_back(AnimationEvent::stringArg(arg.get<std::string>()));
                    }else{
                        warn(&warnings, indexed(path, "args", i), "expected a number, string or boolean, dropped");
                    }
                }
            }
        }
        return true;
    }

    // One audio cue (S40 R1): exactly one of sound, source and snapshot
    bool readCue(const OJson& json, size_t index, ClipAudioCue& cue, std::vector<ClipWarning>& warnings){
        const std::string path = "audio[" + std::to_string(index) + "]";
        if (!json.is_object()){
            warn(&warnings, path, "expected {\"time\", and one of \"sound\", \"source\", \"snapshot\"}, cue dropped");
            return false;
        }
        auto time = json.find("time");
        if (time == json.end() || !toFloat(*time, cue.time)){
            warn(&warnings, member(path, "time"), "expected a time in seconds, cue dropped");
            return false;
        }
        const bool hasSound = json.contains("sound");
        const bool hasSource = json.contains("source");
        const bool hasSnapshot = json.contains("snapshot");
        if ((int)hasSound + (int)hasSource + (int)hasSnapshot != 1){
            warn(&warnings, path, "expected exactly one of \"sound\", \"source\" and \"snapshot\", cue dropped");
            return false;
        }
        auto text = [&](const char* key, std::string& out){
            auto it = json.find(key);
            if (it == json.end()) return true;
            if (!it->is_string()){
                warn(&warnings, member(path, key), "expected a string, ignored");
                return false;
            }
            out = it->get<std::string>();
            return true;
        };
        auto flag = [&](const char* key, bool& out){
            auto it = json.find(key);
            if (it == json.end()) return;
            if (!it->is_boolean()) warn(&warnings, member(path, key), "expected true or false, ignored");
            else out = it->get<bool>();
        };
        auto numberField = [&](const char* key, float& out){
            auto it = json.find(key);
            if (it == json.end()) return;
            if (!toFloat(*it, out)) warn(&warnings, member(path, key), "expected a number, ignored");
        };
        if (hasSound){
            cue.kind = ClipCueKind::Sound;
            const OJson& sound = json["sound"];
            if (sound.is_string()){
                cue.sound = sound.get<std::string>();
            }else if (sound.is_object()){
                auto refPath = sound.find("path");
                if (refPath != sound.end() && refPath->is_string()) cue.sound.path = refPath->get<std::string>();
                auto guid = sound.find("guid");
                if (guid != sound.end() && guid->is_string()) cue.sound.guid = Guid::parse(guid->get<std::string>());
            }else{
                warn(&warnings, member(path, "sound"), "expected {\"guid\", \"path\"} or a path, cue dropped");
                return false;
            }
        }else if (hasSource){
            cue.kind = ClipCueKind::Source;
            if (!text("source", cue.path)){
                warn(&warnings, member(path, "source"), "expected the path of a GameObject with an AudioSource, cue dropped");
                return false;
            }
        }else{
            cue.kind = ClipCueKind::Snapshot;
            if (!text("snapshot", cue.snapshot)){
                warn(&warnings, member(path, "snapshot"), "expected a snapshot name, cue dropped");
                return false;
            }
        }
        if (cue.kind == ClipCueKind::Sound) text("path", cue.path);
        text("group", cue.group);
        text("name", cue.name);
        flag("spatial", cue.spatial);
        flag("follow_speed", cue.followSpeed);
        if (json.contains("unscaled")){
            bool unscaled = false;
            if (json["unscaled"].is_boolean()){
                unscaled = json["unscaled"].get<bool>();
                cue.unscaled = unscaled;
            }else{
                warn(&warnings, member(path, "unscaled"), "expected true or false, ignored");
            }
        }
        numberField("volume_db", cue.volumeDb);
        numberField("pitch_semitones", cue.pitchSemitones);
        numberField("seconds", cue.seconds);
        std::string stop;
        if (text("stop", stop) && !stop.empty() && !anim::cueStopFromName(stop, cue.stop)){
            warn(&warnings, member(path, "stop"), "expected none or state_exit, using none");
        }
        static const char* const known[] = {"time", "sound", "source", "snapshot", "path", "spatial", "volume_db", "pitch_semitones",
                                            "group", "stop", "follow_speed", "name", "seconds", "unscaled"};
        for (auto it = json.begin(); it != json.end(); ++it){
            bool isKnown = false;
            for (const char* key : known) isKnown = isKnown || it.key() == key;
            if (!isKnown) warn(&warnings, member(path, it.key().c_str()), "unknown key ignored");
        }
        return true;
    }

    // A segment between two keys written as Euler that turns more than 180 degrees on an axis
    // can't be kept by quaternions: warned on the later key, in time order
    void checkEulerSegments(const ClipData& data, const ClipOrigins& origins, const EulerKeys& eulerKeys,
                            std::vector<ClipWarning>& warnings){
        static const char* const axes[] = {"x", "y", "z"};
        for (size_t t = 0; t < data.tracks.size() && t < origins.tracks.size(); t++){
            if (data.tracks[t].channel != ClipChannel::Rotation) continue;
            const std::vector<std::string>& keys = origins.tracks[t].keys;
            for (size_t k = 1; k < keys.size(); k++){
                auto previous = eulerKeys.find(keys[k - 1]);
                auto current = eulerKeys.find(keys[k]);
                if (previous == eulerKeys.end() || current == eulerKeys.end()) continue;
                for (int axis = 0; axis < 3; axis++){
                    if (std::fabs(current->second[axis] - previous->second[axis]) > 180.0f){
                        warn(&warnings, member(keys[k], "value"), std::string("turns more than 180 degrees on ") + axes[axis] +
                             " from the previous key, which a quaternion can't keep: insert a middle key");
                        break;
                    }
                }
            }
        }
    }

    // ---- writing ----

    std::string quote(const std::string& text){
        return OJson(text).dump(-1, ' ', false, OJson::error_handler_t::replace);
    }

    std::string number(float value){
        if (!std::isfinite(value)) return "0";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        std::string text = buffer;
        return text == "-0" ? "0" : text;
    }

    // seconds with at most 6 decimals, no trailing zeros
    std::string timeNumber(float value){
        if (!std::isfinite(value)) return "0";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6f", value);
        std::string text = buffer;
        size_t end = text.size();
        while (end > 0 && text[end - 1] == '0') end--;
        if (end > 0 && text[end - 1] == '.') end--;
        text.resize(end);
        return (text == "-0" || text.empty()) ? "0" : text;
    }

    std::string argText(const AnimationEventArg& arg){
        if (std::holds_alternative<bool>(arg)) return std::get<bool>(arg) ? "true" : "false";
        if (std::holds_alternative<std::string>(arg)) return quote(std::get<std::string>(arg));
        double value = std::get<double>(arg);
        if (!std::isfinite(value)) return "0";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.15g", value);
        std::string text = buffer;
        return text == "-0" ? "0" : text;
    }

    std::string floatsText(const std::vector<float>& values, size_t from, size_t count){
        std::string text = "[";
        for (size_t i = 0; i < count; i++){
            if (i > 0) text += ", ";
            text += number(values[from + i]);
        }
        return text + "]";
    }

    // a property value: bools as literals, ints without decimals, one float as a number
    std::string propertyValueText(const ClipTrackData& track, const std::vector<float>& value, bool tangent){
        if (!tangent && track.kind == PropertyKind::Bool && value.size() == 1) return value[0] > 0.5f ? "true" : "false";
        if (!tangent && (track.kind == PropertyKind::Int || track.kind == PropertyKind::Frame) && value.size() == 1){
            return number((float)std::round(value[0]));
        }
        if (value.size() == 1) return number(value[0]);
        return floatsText(value, 0, value.size());
    }

    std::string valueText(const ClipTrackData& track, const std::vector<float>& value, bool tangent = false){
        if (track.channel == ClipChannel::Property) return propertyValueText(track, value, tangent);
        if (track.channel != ClipChannel::Lattice) return floatsText(value, 0, value.size());
        std::string text = "{\"points\": [";
        for (size_t i = 0; i < track.points.size(); i++){
            if (i > 0) text += ", ";
            text += "[" + std::to_string(track.points[i]) + ", " + floatsText(value, 3 * i, 3) + "]";
        }
        return text + "]}";
    }

    std::string keyText(const ClipTrackData& track, const ClipKey& key){
        if (key.hasTangents()){
            std::string text = "{\"time\": " + timeNumber(key.time) + ", \"value\": " + valueText(track, key.value);
            if (key.ease != EaseType::LINEAR) text += std::string(", \"ease\": ") + quote(anim::easeName(key.ease));
            text += ", \"in\": " + valueText(track, key.inTangent, true) + ", \"out\": " + valueText(track, key.outTangent, true);
            if (key.tangentMode != ClipTangentMode::None) text += std::string(", \"tangent_mode\": ") + quote(anim::tangentModeName(key.tangentMode));
            return text + "}";
        }
        std::string text = "[" + timeNumber(key.time) + ", " + valueText(track, key.value);
        if (key.ease != EaseType::LINEAR) text += std::string(", ") + quote(anim::easeName(key.ease));
        return text + "]";
    }

    std::string trackText(const ClipTrackData& track){
        if (track.channel == ClipChannel::Property && !track.known){
            // the track as read, with the fields an editor may have changed (a Retarget renames
            // the path) written over it; compact like any preserved JSON
            OJson object = OJson::parse(track.preserved, nullptr, false);
            if (!object.is_object()) object = OJson::object();
            object["path"] = track.path;
            object["channel"] = "property";
            object["component"] = track.component;
            object["property"] = track.property;
            return "    " + object.dump(-1, ' ', false, OJson::error_handler_t::replace);
        }
        std::string text = isHumanoidChannel(track.channel)
            ? "    {\"channel\": " + quote(anim::channelName(track.channel)) + ", \"bone\": " + quote(anim::humanoidTrackBoneName(track.channel, track.humanBone))
            : "    {\"path\": " + quote(track.path) + ", \"channel\": " + quote(anim::channelName(track.channel));
        if (track.channel == ClipChannel::Property){
            text += ", \"component\": " + quote(track.component) + ", \"property\": " + quote(track.property);
        }
        if (track.relative) text += ", \"relative\": true";
        if (track.loop) text += ", \"loop\": true";
        if (track.keys.empty()) return text + ", \"keys\": []}";
        text += ", \"keys\": [\n";
        for (size_t k = 0; k < track.keys.size(); k++){
            text += "      " + keyText(track, track.keys[k]);
            text += (k + 1 < track.keys.size()) ? ",\n" : "\n";
        }
        return text + "    ]}";
    }

    std::string eventText(const ClipEventData& event){
        std::string text = "    {\"time\": " + timeNumber(event.time) + ", \"function\": " + quote(event.function);
        if (!event.args.empty()){
            text += ", \"args\": [";
            for (size_t i = 0; i < event.args.size(); i++){
                if (i > 0) text += ", ";
                text += argText(event.args[i]);
            }
            text += "]";
        }
        return text + "}";
    }

    // {"time", "sound": {"guid", "path"}, "path"?, "spatial"?, ...}: the keys whose value is not the default, in R1's order
    std::string cueText(const ClipAudioCue& cue){
        std::string text = "    {\"time\": " + timeNumber(cue.time);
        switch (cue.kind){
            case ClipCueKind::Sound: {
                text += ", \"sound\": {";
                if (!cue.sound.guid.isNull()) text += "\"guid\": " + quote(cue.sound.guid.str()) + ", ";
                text += "\"path\": " + quote(cue.sound.path) + "}";
                if (cue.spatial) text += ", \"path\": " + quote(cue.path) + ", \"spatial\": true";
                if (cue.volumeDb != 0.0f) text += ", \"volume_db\": " + number(cue.volumeDb);
                if (cue.pitchSemitones != 0.0f) text += ", \"pitch_semitones\": " + number(cue.pitchSemitones);
                if (!cue.group.empty()) text += ", \"group\": " + quote(cue.group);
                break;
            }
            case ClipCueKind::Source:
                text += ", \"source\": " + quote(cue.path);
                break;
            case ClipCueKind::Snapshot:
                text += ", \"snapshot\": " + quote(cue.snapshot);
                if (cue.seconds != 0.0f) text += ", \"seconds\": " + number(cue.seconds);
                if (cue.unscaled) text += std::string(", \"unscaled\": ") + (*cue.unscaled ? "true" : "false");
                break;
        }
        if (cue.stop != ClipCueStop::None) text += std::string(", \"stop\": ") + quote(anim::cueStopName(cue.stop));
        if (cue.followSpeed) text += ", \"follow_speed\": true";
        if (!cue.name.empty()) text += ", \"name\": " + quote(cue.name);
        return text + "}";
    }

    const char* const knownTopLevelKeys[] = {"format", "version", "guid", "frame_rate", "wrap", "length", "additive_reference",
                                             "humanoid", "tracks", "events", "audio", "preview", "source"};

    bool isKnownTopLevelKey(const std::string& name){
        for (const char* known : knownTopLevelKeys){
            if (name == known) return true;
        }
        return false;
    }

}

static anim::ClipParseResult parseClipText(const std::string& text);

anim::ClipParseResult anim::parseClip(const std::string& text){
    // the parser reports every problem as a result; anything thrown below is a bug, never a
    // reason to take the caller (a loader thread, play mode, a game) down
    try{
        return parseClipText(text);
    }catch (const std::exception& e){
        ClipParseResult result;
        result.error.message = std::string("cannot read the clip: ") + e.what();
        return result;
    }catch (...){
        ClipParseResult result;
        result.error.message = "cannot read the clip";
        return result;
    }
}

static anim::ClipParseResult parseClipText(const std::string& text){
    using namespace anim;
    ClipParseResult result;
    if (isUnityYamlClip(text)){
        result.error.message = unityYamlRefusal();
        return result;
    }

    OJson root;
    if (!json::parseDocument(text, root, result.error)){
        return result;
    }
    if (!root.is_object()){
        result.error.message = "not an animation clip: the top level must be a JSON object";
        return result;
    }

    auto format = root.find("format");
    if (format == root.end() || !format->is_string() || format->get<std::string>() != ANIM_FORMAT){
        result.error.message = std::string("not an animation clip: \"format\" must be \"") + ANIM_FORMAT + "\"";
        return result;
    }
    auto version = root.find("version");
    if (version == root.end()){
        result.error.message = "missing \"version\" (1)";
        return result;
    }
    if (!version->is_number() || version->get<double>() != std::floor(version->get<double>()) || version->get<double>() < 1.0){
        result.error.message = "invalid \"version\": expected a whole number from 1";
        return result;
    }
    if (version->get<double>() > ANIM_VERSION){
        result.error.message = "unsupported animation clip version " + shortNumber(version->get<double>()) +
                               " (this engine reads versions up to " + std::to_string(ANIM_VERSION) + ")";
        return result;
    }

    std::vector<ClipWarning>& warnings = result.warnings;
    ClipData data;
    ClipOrigins origins;
    EulerKeys eulerKeys;
    std::string unknownNames;
    for (auto it = root.begin(); it != root.end(); ++it){
        const std::string& name = it.key();
        const OJson& value = *it;
        if (name == "format" || name == "version") continue;
        if (name == "guid"){
            if (!value.is_string() || !isClipGuid(value.get<std::string>())){
                warn(&warnings, "guid", "expected 32 lowercase hex digits: dropped (a new one is given on save)");
            }else{
                data.guid = value.get<std::string>();
            }
        }else if (name == "frame_rate"){
            if (!toFloat(value, data.frameRate)){
                warn(&warnings, "frame_rate", "expected samples per second (1..240): using 30");
                data.frameRate = 30.0f;
            }
        }else if (name == "wrap"){
            ClipWrapMode wrap = ClipWrapMode::Once;
            if (!value.is_string() || !wrapFromName(value.get<std::string>(), wrap) || wrap == ClipWrapMode::Default){
                warn(&warnings, "wrap", "expected once, loop, ping_pong or clamp_forever: using once");
                wrap = ClipWrapMode::Once;
            }
            data.wrap = wrap;
        }else if (name == "length"){
            if (value.is_null()) continue;
            float length = -1.0f;
            if (!toFloat(value, length) || length < 0.0f){
                warn(&warnings, "length", "expected seconds >= 0 (or no length for the last key's time): automatic length");
                length = -1.0f;
            }
            data.length = length;
        }else if (name == "additive_reference"){
            float reference = 0.0f;
            if (!toFloat(value, reference) || reference < 0.0f){
                warn(&warnings, "additive_reference", "expected seconds >= 0 (the time an additive layer measures from): using 0");
                reference = 0.0f;
            }
            data.additiveReference = reference;
        }else if (name == "humanoid"){
            if (!value.is_object()){
                warn(&warnings, "humanoid", "expected {\"source_model\", \"human_scale\"}, ignored");
                continue;
            }
            for (auto entry = value.begin(); entry != value.end(); ++entry){
                if (entry.key() == "source_model" && entry->is_string()){
                    data.humanoid.sourceModel = entry->get<std::string>();
                }else if (entry.key() == "human_scale" && entry->is_number()){
                    float scale = 0.0f;
                    if (!toFloat(*entry, scale) || !(scale > 0.0f)){
                        warn(&warnings, "humanoid.human_scale", "expected a positive number, ignored");
                    }else{
                        data.humanoid.humanScale = scale;
                    }
                }else if (entry.key() == "source_model" || entry.key() == "human_scale"){
                    warn(&warnings, "humanoid." + entry.key(), entry.key() == "source_model" ? "expected a model path, ignored" : "expected a number, ignored");
                }else{
                    warn(&warnings, "humanoid." + entry.key(), "unknown key ignored");
                }
            }
        }else if (name == "tracks"){
            if (!value.is_array()){
                warn(&warnings, "tracks", "expected an array of tracks, ignored");
                continue;
            }
            for (size_t i = 0; i < value.size(); i++){
                ClipTrackData track;
                TrackOrigin origin;
                if (readTrack(value[i], i, track, origin, eulerKeys, warnings)){
                    data.tracks.push_back(std::move(track));
                    origins.tracks.push_back(std::move(origin));
                }
            }
        }else if (name == "events"){
            if (!value.is_array()){
                warn(&warnings, "events", "expected an array of events, ignored");
                continue;
            }
            for (size_t i = 0; i < value.size(); i++){
                ClipEventData event;
                if (readEvent(value[i], i, event, warnings)){
                    data.events.push_back(std::move(event));
                    origins.events.push_back(i);
                }
            }
        }else if (name == "audio"){
            if (!value.is_array()){
                warn(&warnings, "audio", "expected an array of audio cues, ignored");
                continue;
            }
            for (size_t i = 0; i < value.size(); i++){
                ClipAudioCue cue;
                if (readCue(value[i], i, cue, warnings)){
                    data.audio.push_back(std::move(cue));
                    origins.audio.push_back(i);
                }
            }
        }else if (name == "preview"){
            if (!value.is_object()){
                warn(&warnings, "preview", "expected {\"model\": \"<model or prefab path>\"}, ignored");
                continue;
            }
            for (auto entry = value.begin(); entry != value.end(); ++entry){
                if (entry.key() == "model" && entry->is_string()){
                    data.previewModel = entry->get<std::string>();
                }else if (entry.key() == "model"){
                    warn(&warnings, "preview.model", "expected a model or prefab path, ignored");
                }else{
                    warn(&warnings, "preview." + entry.key(), "unknown key ignored");
                }
            }
        }else if (name == "source"){
            if (!value.is_object()){
                warn(&warnings, "source", "expected {\"kind\", \"path\", \"name\"}, ignored");
                continue;
            }
            ClipSource source;
            for (auto entry = value.begin(); entry != value.end(); ++entry){
                const std::string& key = entry.key();
                std::string* field = key == "kind" ? &source.kind : key == "path" ? &source.path : key == "name" ? &source.name : nullptr;
                if (!field){
                    warn(&warnings, "source." + key, "unknown key ignored");
                }else if (!entry->is_string()){
                    warn(&warnings, "source." + key, "expected a string, ignored");
                }else{
                    *field = entry->get<std::string>();
                }
            }
            data.source = source;
        }else{
            data.unknownKeys.push_back({name, value.dump(-1, ' ', false, OJson::error_handler_t::replace)});
            // the Animator's members (root_motion, editor: S21) are kept without a warning
            if (!isClipExtensionKey(name)) unknownNames += (unknownNames.empty() ? "" : ", ") + name;
        }
    }
    if (!unknownNames.empty()){
        warn(&warnings, "", "unknown keys kept as they are (written back on save): " + unknownNames);
    }

    for (const ClipTrackData& track : data.tracks){
        if (!isRotationChannel(track.channel)) continue;
        for (size_t k = 1; k < track.keys.size(); k++){
            const ClipKey& previous = track.keys[k - 1];
            const ClipKey& key = track.keys[k];
            if (previous.hasTangents() || key.hasTangents() || previous.value.size() != 4 || key.value.size() != 4) continue;
            if (quaternionDot(previous.value, key.value) < 0.0f){
                result.rotationSignFlips.push_back({track.path, key.time});
                break;
            }
        }
    }
    normalizeClip(data, &warnings, &origins);
    checkEulerSegments(data, origins, eulerKeys, warnings);
    if (version->get<double>() < 2.0 && data.isHumanoid()){
        warn(&warnings, "version", "humanoid tracks in a version 1 file: the clip is saved as version 2");
    }
    if (version->get<double>() < 3.0 && !data.audio.empty()){
        warn(&warnings, "version", "audio cues in a version " + shortNumber(version->get<double>()) + " file: the clip is saved as version 3");
    }
    result.data = std::move(data);
    return result;
}

void anim::normalize(ClipData& data, std::vector<ClipWarning>* warnings){
    normalizeClip(data, warnings, nullptr);
}


std::string anim::writeClip(const ClipData& input){
    ClipData data = input;
    normalizeClip(data, nullptr, nullptr);

    // values as the file stores them, so that reading the text back takes the same normalize
    // and hemisphere decisions on the same numbers (the canonical text is a fixed point)
    auto quantize = [](ClipKey& key){
        for (float& v : key.value) v = quantizeValue(v);
        for (float& v : key.inTangent) v = quantizeValue(v);
        for (float& v : key.outTangent) v = quantizeValue(v);
    };
    for (ClipTrackData& track : data.tracks){
        if (track.channel == ClipChannel::Property && !track.known) continue;
        for (ClipKey& key : track.keys){
            quantize(key);
            // a quaternion just inside the 1e-5 band can leave it once rounded: normalized
            // and rounded again it lands within ~1e-6, where the reader keeps it
            if (isRotationChannel(track.channel)){
                float length = quaternionLength(key.value);
                if (std::fabs(length - 1.0f) > 1e-5f && length > 0.0f){
                    scaleRotationKey(key, 1.0f / length);
                    quantize(key);
                }
            }
        }
        if (isRotationChannel(track.channel)) enforceHemisphere(track);
    }

    std::string text = "{\n";
    text += std::string("  \"format\": ") + quote(ANIM_FORMAT) + ",\n";
    // version 1 files stay byte-identical: 2 only for humanoid clips (anim-retarget 6.1), 3 only
    // for clips with audio cues (S40 R2: an older engine refuses them instead of dropping sounds)
    text += "  \"version\": " + std::to_string(!data.audio.empty() ? 3 : data.isHumanoid() ? 2 : 1) + ",\n";
    if (!data.guid.empty()) text += "  \"guid\": " + quote(data.guid) + ",\n";
    text += "  \"frame_rate\": " + number(data.frameRate) + ",\n";
    text += std::string("  \"wrap\": ") + quote(wrapName(data.wrap)) + ",\n";
    if (data.hasExplicitLength()) text += "  \"length\": " + timeNumber(data.length) + ",\n";
    if (data.additiveReference != 0.0f) text += "  \"additive_reference\": " + timeNumber(data.additiveReference) + ",\n";
    if (!data.humanoid.empty()){
        std::string info;
        if (!data.humanoid.sourceModel.empty()) info += "\"source_model\": " + quote(data.humanoid.sourceModel);
        if (data.humanoid.humanScale > 0.0f) info += std::string(info.empty() ? "" : ", ") + "\"human_scale\": " + number(data.humanoid.humanScale);
        text += "  \"humanoid\": {" + info + "},\n";
    }

    if (data.tracks.empty()){
        text += "  \"tracks\": [],\n";
    }else{
        text += "  \"tracks\": [\n";
        for (size_t i = 0; i < data.tracks.size(); i++){
            text += trackText(data.tracks[i]);
            text += (i + 1 < data.tracks.size()) ? ",\n" : "\n";
        }
        text += "  ],\n";
    }
    if (data.events.empty()){
        text += "  \"events\": [],\n";
    }else{
        text += "  \"events\": [\n";
        for (size_t i = 0; i < data.events.size(); i++){
            text += eventText(data.events[i]);
            text += (i + 1 < data.events.size()) ? ",\n" : "\n";
        }
        text += "  ],\n";
    }
    if (!data.audio.empty()){
        text += "  \"audio\": [\n";
        for (size_t i = 0; i < data.audio.size(); i++){
            text += cueText(data.audio[i]);
            text += (i + 1 < data.audio.size()) ? ",\n" : "\n";
        }
        text += "  ],\n";
    }
    if (!data.previewModel.empty()) text += "  \"preview\": {\"model\": " + quote(data.previewModel) + "},\n";
    if (!data.source.empty()){
        text += "  \"source\": {\"kind\": " + quote(data.source.kind) + ", \"path\": " + quote(data.source.path) +
                ", \"name\": " + quote(data.source.name) + "},\n";
    }
    // unknown keys in file order (the last of a repeated name), verbatim
    for (size_t i = 0; i < data.unknownKeys.size(); i++){
        const auto& entry = data.unknownKeys[i];
        if (isKnownTopLevelKey(entry.first)) continue;
        bool repeated = false;
        for (size_t j = i + 1; j < data.unknownKeys.size(); j++){
            if (data.unknownKeys[j].first == entry.first) repeated = true;
        }
        if (repeated) continue;
        text += "  " + quote(entry.first) + ": " + (entry.second.empty() ? std::string("null") : entry.second) + ",\n";
    }
    // the last member has no comma
    text.erase(text.size() - 2, 1);
    text += "}\n";
    return text;
}

bool anim::isLuaIdentifier(const std::string& name){
    static const char* const keywords[] = {"and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true",
        "until", "while"};
    if (name.empty() || !(std::isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (char c : name){
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    }
    for (const char* keyword : keywords){
        if (name == keyword) return false;
    }
    return true;
}

void anim::alignQuaternionKey(std::vector<float>& value, const std::vector<float>& reference){
    if (value.size() != 4 || reference.size() != 4) return;
    float dot = 0.0f;
    for (size_t i = 0; i < 4; i++) dot += value[i] * reference[i];
    if (dot < 0.0f){
        for (float& component : value) component = -component;
    }
}
