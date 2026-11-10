// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShakePreset.h"

#include "animation/AnimationClipIO.h"
#include "json.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::camera;

namespace{

    using OJson = nlohmann::ordered_json;

    const char* const knownKeys[] = {"format", "version", "guid", "type", "strength", "roughness", "fade_in",
                                     "fade_out", "position_influence", "rotation_influence", "units", "tick"};

    bool isKnownKey(const std::string& name){
        for (const char* key : knownKeys){
            if (name == key) return true;
        }
        return false;
    }

    std::string number(float value){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        return buffer;
    }

    std::string vector(const Vector3& v){
        return "[" + number(v.x) + ", " + number(v.y) + ", " + number(v.z) + "]";
    }

    std::string quote(const std::string& text){
        return OJson(text).dump(-1, ' ', false, OJson::error_handler_t::replace);
    }

    bool toFloat(const OJson& value, float& out){
        if (!value.is_number()) return false;
        double number = value.get<double>();
        if (!std::isfinite(number) || std::fabs(number) > (double)FLT_MAX) return false;
        out = (float)number;
        return true;
    }

    bool toVector(const OJson& value, Vector3& out){
        if (!value.is_array() || value.size() != 3) return false;
        float x, y, z;
        if (!toFloat(value[0], x) || !toFloat(value[1], y) || !toFloat(value[2], z)) return false;
        out = Vector3(x, y, z);
        return true;
    }

    // nlohmann's DOM builder recording where and why the parse stopped, without exceptions (the
    // Web build has no exception catching)
    class SaxParser : public nlohmann::detail::json_sax_dom_parser<OJson>{
    public:
        explicit SaxParser(OJson& root): json_sax_dom_parser<OJson>(root, false){}

        template<class Exception>
        bool parse_error(std::size_t position, const std::string& lastToken, const Exception& ex){
            failed = true;
            errorPosition = position;
            message = ex.what();
            return json_sax_dom_parser<OJson>::parse_error(position, lastToken, ex);
        }

        bool failed = false;
        std::size_t errorPosition = 0;   // characters read when the parse stopped
        std::string message;
    };

    // the 1-based line and column of byte `offset`
    void textPosition(const std::string& text, size_t offset, int& line, int& column){
        line = 1;
        column = 1;
        for (size_t i = 0; i < offset && i < text.size(); i++){
            if (text[i] == '\n'){
                line++;
                column = 1;
            }else{
                column++;
            }
        }
    }

    ShakeParseResult parseText(const std::string& text){
        ShakeParseResult result;
        OJson root;
        SaxParser sax(root);
        OJson::sax_parse(text, &sax);
        if (sax.failed){
            textPosition(text, sax.errorPosition > 0 ? sax.errorPosition - 1 : 0, result.line, result.column);
            size_t at = sax.message.find("syntax error");
            result.error = "not valid JSON: " + (at != std::string::npos ? sax.message.substr(at) : sax.message);
            return result;
        }
        if (!root.is_object()){
            result.error = "not a shake preset: the top level must be a JSON object";
            return result;
        }
        auto format = root.find("format");
        if (format == root.end() || !format->is_string() || format->get<std::string>() != SHAKE_FORMAT){
            result.error = std::string("not a shake preset: \"format\" must be \"") + SHAKE_FORMAT + "\"";
            return result;
        }
        auto version = root.find("version");
        if (version == root.end()){
            result.error = "missing \"version\" (1)";
            return result;
        }
        if (!version->is_number() || version->get<double>() != std::floor(version->get<double>()) || version->get<double>() < 1.0){
            result.error = "invalid \"version\": expected a whole number from 1";
            return result;
        }
        if (version->get<double>() > SHAKE_VERSION){
            result.error = "unsupported shake preset version " + number((float)version->get<double>()) +
                           " (this engine reads versions up to " + std::to_string(SHAKE_VERSION) + ")";
            return result;
        }

        ShakePresetFile file;
        ShakePreset& preset = file.preset;
        const ShakePreset defaults;
        std::vector<std::string>& warnings = result.warnings;
        std::string unknownNames;

        auto readFloat = [&](const std::string& key, const OJson& value, float& out, float fallback){
            if (!toFloat(value, out)){
                warnings.push_back(key + ": expected a number, using " + number(fallback));
                out = fallback;
            }
        };
        auto readVector = [&](const std::string& key, const OJson& value, Vector3& out, const Vector3& fallback){
            if (!toVector(value, out)){
                warnings.push_back(key + ": expected [x, y, z] numbers, using " + vector(fallback));
                out = fallback;
            }
        };

        for (auto it = root.begin(); it != root.end(); ++it){
            const std::string& name = it.key();
            const OJson& value = *it;
            if (name == "format" || name == "version") continue;
            if (name == "guid"){
                if (!value.is_string() || !anim::isClipGuid(value.get<std::string>())){
                    warnings.push_back("guid: expected 32 lowercase hex digits, dropped");
                }else{
                    file.guid = value.get<std::string>();
                }
            }else if (name == "type"){
                if (!value.is_string() || !shakeTypeFromName(value.get<std::string>(), preset.type)){
                    warnings.push_back("type: expected \"one_shot\", \"sustained\" or \"kick\", using \"one_shot\"");
                    preset.type = ShakeType::OneShot;
                }
            }else if (name == "strength"){
                readFloat(name, value, preset.strength, defaults.strength);
            }else if (name == "roughness"){
                readFloat(name, value, preset.roughness, defaults.roughness);
            }else if (name == "fade_in"){
                readFloat(name, value, preset.fadeIn, defaults.fadeIn);
            }else if (name == "fade_out"){
                readFloat(name, value, preset.fadeOut, defaults.fadeOut);
            }else if (name == "position_influence"){
                readVector(name, value, preset.positionInfluence, defaults.positionInfluence);
            }else if (name == "rotation_influence"){
                readVector(name, value, preset.rotationInfluence, defaults.rotationInfluence);
            }else if (name == "units"){
                if (!value.is_string() || !shakeUnitsFromName(value.get<std::string>(), preset.units)){
                    warnings.push_back("units: expected \"view\" or \"pixels\", using \"view\"");
                    preset.units = ShakeUnits::View;
                }
            }else if (name == "tick"){
                readFloat(name, value, preset.tick, defaults.tick);
            }else{
                file.unknownKeys.push_back({name, value.dump(-1, ' ', false, OJson::error_handler_t::replace)});
                unknownNames += (unknownNames.empty() ? "" : ", ") + name;
            }
        }
        if (!unknownNames.empty()){
            warnings.push_back("unknown keys kept as they are (written back on save): " + unknownNames);
        }
        sanitizeShakePreset(preset, &warnings);

        result.file = std::move(file);
        return result;
    }

}

bool ShakePreset::operator==(const ShakePreset& other) const{
    return type == other.type && strength == other.strength && roughness == other.roughness &&
           fadeIn == other.fadeIn && fadeOut == other.fadeOut &&
           positionInfluence == other.positionInfluence && rotationInfluence == other.rotationInfluence &&
           units == other.units && tick == other.tick;
}

std::string ShakeParseResult::errorText() const{
    if (line > 0) return std::to_string(line) + ":" + std::to_string(column) + ": " + error;
    return error;
}

ShakeParseResult camera::parseShakePreset(const std::string& text){
    // problems are results; anything thrown below is a bug, never a reason to take a game down
    try{
        return parseText(text);
    }catch (const std::exception& e){
        ShakeParseResult result;
        result.error = std::string("cannot read the shake preset: ") + e.what();
        return result;
    }catch (...){
        ShakeParseResult result;
        result.error = "cannot read the shake preset";
        return result;
    }
}

std::string camera::writeShakePreset(const ShakePresetFile& file){
    ShakePreset preset = file.preset;
    sanitizeShakePreset(preset);

    std::string text = "{\n";
    text += std::string("  \"format\": ") + quote(SHAKE_FORMAT) + ",\n";
    text += "  \"version\": " + std::to_string(SHAKE_VERSION) + ",\n";
    if (!file.guid.empty()) text += "  \"guid\": " + quote(file.guid) + ",\n";
    text += std::string("  \"type\": ") + quote(shakeTypeName(preset.type)) + ",\n";
    text += "  \"strength\": " + number(preset.strength) + ",\n";
    text += "  \"roughness\": " + number(preset.roughness) + ",\n";
    text += "  \"fade_in\": " + number(preset.fadeIn) + ",\n";
    text += "  \"fade_out\": " + number(preset.fadeOut) + ",\n";
    text += "  \"position_influence\": " + vector(preset.positionInfluence) + ",\n";
    text += "  \"rotation_influence\": " + vector(preset.rotationInfluence) + ",\n";
    // S48's keys only when set: a file without them re-saves byte for byte
    if (preset.units != ShakeUnits::View) text += std::string("  \"units\": ") + quote(shakeUnitsName(preset.units)) + ",\n";
    if (preset.tick > 0.0f) text += "  \"tick\": " + number(preset.tick) + ",\n";
    // unknown keys in file order (the last of a repeated name), verbatim
    for (size_t i = 0; i < file.unknownKeys.size(); i++){
        const auto& entry = file.unknownKeys[i];
        if (isKnownKey(entry.first)) continue;
        bool repeated = false;
        for (size_t j = i + 1; j < file.unknownKeys.size(); j++){
            if (file.unknownKeys[j].first == entry.first) repeated = true;
        }
        if (repeated) continue;
        text += "  " + quote(entry.first) + ": " + (entry.second.empty() ? std::string("null") : entry.second) + ",\n";
    }
    // the last member has no comma
    text.erase(text.size() - 2, 1);
    text += "}\n";
    return text;
}

std::string camera::writeShakePreset(const ShakePreset& preset){
    ShakePresetFile file;
    file.preset = preset;
    return writeShakePreset(file);
}

const char* camera::shakeTypeName(ShakeType type){
    if (type == ShakeType::Kick) return "kick";
    return type == ShakeType::Sustained ? "sustained" : "one_shot";
}

const char* camera::shakeUnitsName(ShakeUnits units){
    return units == ShakeUnits::Pixels ? "pixels" : "view";
}

bool camera::shakeUnitsFromName(const std::string& name, ShakeUnits& out){
    if (name == "view"){
        out = ShakeUnits::View;
        return true;
    }
    if (name == "pixels"){
        out = ShakeUnits::Pixels;
        return true;
    }
    return false;
}

ShakePreset camera::makeKickPreset(float amplitudePx, float seconds){
    ShakePreset preset;
    preset.type = ShakeType::Kick;
    preset.units = ShakeUnits::Pixels;
    preset.strength = std::isfinite(amplitudePx) ? std::max(0.0f, amplitudePx) : 0.0f;
    preset.fadeOut = std::isfinite(seconds) ? std::max(0.0f, seconds) : 0.0f;
    preset.roughness = 1.0f;
    preset.rotationInfluence = Vector3(0, 0, 0);
    return preset;
}

bool camera::shakeTypeFromName(const std::string& name, ShakeType& out){
    if (name == "one_shot"){
        out = ShakeType::OneShot;
        return true;
    }
    if (name == "sustained"){
        out = ShakeType::Sustained;
        return true;
    }
    if (name == "kick"){
        out = ShakeType::Kick;
        return true;
    }
    return false;
}

void camera::sanitizeShakePreset(ShakePreset& preset, std::vector<std::string>* warnings){
    const ShakePreset defaults;
    auto clampField = [&](const char* key, float& value, float fallback){
        if (!std::isfinite(value)){
            value = fallback;
            if (warnings) warnings->push_back(std::string(key) + ": not a finite number, using " + number(fallback));
        }else if (value < 0.0f){
            value = 0.0f;
            if (warnings) warnings->push_back(std::string(key) + ": negative, using 0");
        }
    };
    clampField("strength", preset.strength, defaults.strength);
    clampField("roughness", preset.roughness, defaults.roughness);
    clampField("fade_in", preset.fadeIn, defaults.fadeIn);
    clampField("fade_out", preset.fadeOut, defaults.fadeOut);
    clampField("tick", preset.tick, defaults.tick);
    auto finiteVector = [&](const char* key, Vector3& value, const Vector3& fallback){
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)){
            value = fallback;
            if (warnings) warnings->push_back(std::string(key) + ": not finite numbers, using the default");
        }
    };
    finiteVector("position_influence", preset.positionInfluence, defaults.positionInfluence);
    finiteVector("rotation_influence", preset.rotationInfluence, defaults.rotationInfluence);
}

float camera::shakePreviewDuration(const ShakePreset& preset){
    // a kick is over at fade_out (plus a beat of rest for the graph)
    if (preset.type == ShakeType::Kick) return std::max(0.05f, preset.fadeOut) + 0.25f;
    if (preset.fadeIn <= 0.0f && preset.fadeOut <= 0.0f) return 3.0f;
    return preset.fadeIn + 1.0f + preset.fadeOut;
}
