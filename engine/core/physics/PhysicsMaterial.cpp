// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsMaterial.h"

#include "pool/PhysicsMaterialPool.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <random>

using namespace jasperengine;

std::atomic<uint64_t> PhysicsMaterialRuntime::globalRevision{1};

namespace{

    using OrderedJson = nlohmann::ordered_json;

    std::string formatNumber(float value){
        if (!std::isfinite(value)) value = 0.0f;
        char text[32];
        std::snprintf(text, sizeof(text), "%.6g", value);
        return text;
    }

    std::string lowerNoSeparators(const std::string& text){
        std::string out;
        for (char c : text){
            if (c == '_' || c == ' ' || c == '-') continue;
            out.push_back((char)std::tolower((unsigned char)c));
        }
        return out;
    }

    bool readNumber(const OrderedJson& value, float& out){
        if (!value.is_number()) return false;
        const double number = value.get<double>();
        if (!std::isfinite(number)) return false;
        out = (float)number;
        return true;
    }

}

PhysicsMaterialData PhysicsMaterialData::preset3D(){
    PhysicsMaterialData data;
    data.friction = 0.6f;
    data.bounciness = 0.0f;
    data.frictionCombine = PhysicsMaterialCombine::AVERAGE;
    data.bounceCombine = PhysicsMaterialCombine::AVERAGE;
    return data;
}

PhysicsMaterialData PhysicsMaterialData::preset2D(){
    PhysicsMaterialData data;
    data.friction = 0.4f;
    data.bounciness = 0.0f;
    data.frictionCombine = PhysicsMaterialCombine::GEOMETRIC_MEAN;
    data.bounceCombine = PhysicsMaterialCombine::MAXIMUM;
    return data;
}

bool PhysicsMaterialData::operator==(const PhysicsMaterialData& other) const{
    return guid == other.guid && friction == other.friction && bounciness == other.bounciness &&
           frictionCombine == other.frictionCombine && bounceCombine == other.bounceCombine &&
           unknownKeys == other.unknownKeys;
}

const char* physicsmaterial::combineName(PhysicsMaterialCombine mode){
    switch (mode){
        case PhysicsMaterialCombine::AVERAGE: return "average";
        case PhysicsMaterialCombine::GEOMETRIC_MEAN: return "geometric_mean";
        case PhysicsMaterialCombine::MINIMUM: return "minimum";
        case PhysicsMaterialCombine::MULTIPLY: return "multiply";
        case PhysicsMaterialCombine::MAXIMUM: return "maximum";
        case PhysicsMaterialCombine::NONE: break;
    }
    return "";
}

bool physicsmaterial::combineFromName(const std::string& name, PhysicsMaterialCombine& mode){
    const std::string key = lowerNoSeparators(name);
    if (key == "average" || key == "mean"){ mode = PhysicsMaterialCombine::AVERAGE; return true; }
    if (key == "geometricmean"){ mode = PhysicsMaterialCombine::GEOMETRIC_MEAN; return true; }
    if (key == "minimum" || key == "min"){ mode = PhysicsMaterialCombine::MINIMUM; return true; }
    if (key == "multiply"){ mode = PhysicsMaterialCombine::MULTIPLY; return true; }
    if (key == "maximum" || key == "max"){ mode = PhysicsMaterialCombine::MAXIMUM; return true; }
    return false;
}

std::string physicsmaterial::generateGuid(){
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

PhysicsMaterialParseResult physicsmaterial::parse(const std::string& text){
    PhysicsMaterialParseResult result;

    OrderedJson root;
    try{
        size_t start = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
        root = OrderedJson::parse(text.begin() + (std::ptrdiff_t)start, text.end());
    }catch (const std::exception& e){
        result.error = std::string("not JSON: ") + e.what();
        return result;
    }
    if (!root.is_object()){
        result.error = "a physics material is a JSON object";
        return result;
    }
    if (root.contains("format")){
        if (!root["format"].is_string() || root["format"].get<std::string>() != PHYSICS_MATERIAL_FORMAT){
            result.error = std::string("format is not \"") + PHYSICS_MATERIAL_FORMAT + "\"";
            return result;
        }
    }else{
        result.warnings.push_back(std::string("format: missing, read as \"") + PHYSICS_MATERIAL_FORMAT + "\"");
    }
    if (root.contains("version")){
        if (!root["version"].is_number_integer() || root["version"].get<int>() > PHYSICS_MATERIAL_VERSION){
            result.error = "version: newer than this engine reads (" + std::to_string(PHYSICS_MATERIAL_VERSION) + ")";
            return result;
        }
    }

    PhysicsMaterialData& data = result.data;
    OrderedJson unknown = OrderedJson::object();

    for (auto it = root.begin(); it != root.end(); ++it){
        const std::string& key = it.key();
        const OrderedJson& value = it.value();
        if (key == "format" || key == "version"){
            continue;
        }else if (key == "guid"){
            if (value.is_string()) data.guid = value.get<std::string>();
            else result.warnings.push_back("guid: not a string, dropped");
        }else if (key == "friction" || key == "dynamicFriction"){
            float friction = 0.0f;
            if (!readNumber(value, friction)){
                result.warnings.push_back(key + ": not a number, default used");
                continue;
            }
            // Unity has both: the dynamic one is what bodies slide with
            if (key == "friction" || !root.contains("friction")){
                if (friction < 0.0f){
                    result.warnings.push_back(key + ": negative, 0 used");
                    friction = 0.0f;
                }
                data.friction = friction;
            }
        }else if (key == "staticFriction"){
            // Unity: no static/dynamic split here, friction is used
            unknown[key] = value;
        }else if (key == "bounciness" || key == "bounce"){
            float bounciness = 0.0f;
            if (!readNumber(value, bounciness)){
                result.warnings.push_back(key + ": not a number, default used");
                continue;
            }
            if (key == "bounciness" || !root.contains("bounciness")){
                if (bounciness < 0.0f || bounciness > 1.0f){
                    result.warnings.push_back(key + ": clamped to 0..1");
                    bounciness = std::min(1.0f, std::max(0.0f, bounciness));
                }
                data.bounciness = bounciness;
            }
        }else if (key == "frictionCombine" || key == "bounceCombine"){
            PhysicsMaterialCombine mode = PhysicsMaterialCombine::AVERAGE;
            if (!value.is_string() || !combineFromName(value.get<std::string>(), mode)){
                result.warnings.push_back(key + ": unknown combine mode, average used "
                                          "(average, geometric_mean, minimum, multiply, maximum)");
                mode = PhysicsMaterialCombine::AVERAGE;
            }
            if (key == "frictionCombine") data.frictionCombine = mode;
            else data.bounceCombine = mode;
        }else{
            unknown[key] = value;
        }
    }

    if (!unknown.empty()){
        data.unknownKeys = unknown.dump();
    }
    result.ok = true;
    return result;
}

std::string physicsmaterial::write(const PhysicsMaterialData& data){
    std::string text = "{\n";
    text += std::string("  \"format\": \"") + PHYSICS_MATERIAL_FORMAT + "\",\n";
    text += "  \"version\": " + std::to_string(PHYSICS_MATERIAL_VERSION) + ",\n";
    if (!data.guid.empty()){
        text += "  \"guid\": " + OrderedJson(data.guid).dump() + ",\n";
    }
    text += "  \"friction\": " + formatNumber(std::max(0.0f, data.friction)) + ",\n";
    text += "  \"bounciness\": " + formatNumber(std::min(1.0f, std::max(0.0f, data.bounciness))) + ",\n";
    const PhysicsMaterialCombine friction = data.frictionCombine == PhysicsMaterialCombine::NONE ? PhysicsMaterialCombine::AVERAGE : data.frictionCombine;
    const PhysicsMaterialCombine bounce = data.bounceCombine == PhysicsMaterialCombine::NONE ? PhysicsMaterialCombine::AVERAGE : data.bounceCombine;
    text += std::string("  \"frictionCombine\": \"") + combineName(friction) + "\",\n";
    text += std::string("  \"bounceCombine\": \"") + combineName(bounce) + "\"";

    if (!data.unknownKeys.empty()){
        try{
            OrderedJson unknown = OrderedJson::parse(data.unknownKeys);
            if (unknown.is_object()){
                for (auto it = unknown.begin(); it != unknown.end(); ++it){
                    text += ",\n  " + OrderedJson(it.key()).dump() + ": " + it.value().dump();
                }
            }
        }catch (...){
        }
    }
    text += "\n}\n";
    return text;
}

PhysicsMaterialRuntime::PhysicsMaterialRuntime(const PhysicsMaterialData& data, const std::string& path):
    path(path),
    friction(std::max(0.0f, data.friction)),
    bounciness(std::min(1.0f, std::max(0.0f, data.bounciness))),
    frictionCombine(data.frictionCombine == PhysicsMaterialCombine::NONE ? PhysicsMaterialCombine::AVERAGE : data.frictionCombine),
    bounceCombine(data.bounceCombine == PhysicsMaterialCombine::NONE ? PhysicsMaterialCombine::AVERAGE : data.bounceCombine){
}

void PhysicsMaterialRuntime::changed(){
    generation++;
    bumpRevision();
}

void PhysicsMaterialRuntime::setFriction(float friction){
    friction = std::isfinite(friction) ? std::max(0.0f, friction) : 0.0f;
    if (this->friction != friction){
        this->friction = friction;
        changed();
    }
}

void PhysicsMaterialRuntime::setBounciness(float bounciness){
    bounciness = std::isfinite(bounciness) ? std::min(1.0f, std::max(0.0f, bounciness)) : 0.0f;
    if (this->bounciness != bounciness){
        this->bounciness = bounciness;
        changed();
    }
}

void PhysicsMaterialRuntime::setFrictionCombine(PhysicsMaterialCombine mode){
    if (mode == PhysicsMaterialCombine::NONE) mode = PhysicsMaterialCombine::AVERAGE;
    if (frictionCombine != mode){
        frictionCombine = mode;
        changed();
    }
}

void PhysicsMaterialRuntime::setBounceCombine(PhysicsMaterialCombine mode){
    if (mode == PhysicsMaterialCombine::NONE) mode = PhysicsMaterialCombine::AVERAGE;
    if (bounceCombine != mode){
        bounceCombine = mode;
        changed();
    }
}

PhysicsMaterialData PhysicsMaterialRuntime::toData() const{
    PhysicsMaterialData data;
    data.friction = friction;
    data.bounciness = bounciness;
    data.frictionCombine = frictionCombine;
    data.bounceCombine = bounceCombine;
    return data;
}

uint64_t PhysicsMaterialRuntime::revision(){
    return globalRevision.load();
}

void PhysicsMaterialRuntime::bumpRevision(){
    globalRevision++;
}

PhysicsMaterial::PhysicsMaterial(std::shared_ptr<PhysicsMaterialRuntime> runtime): runtime(std::move(runtime)){
}

PhysicsMaterial PhysicsMaterial::load(const std::string& path){
    return PhysicsMaterial(PhysicsMaterialPool::getOrLoad(path));
}

PhysicsMaterial PhysicsMaterial::create(float friction, float bounciness, PhysicsMaterialCombine frictionCombine, PhysicsMaterialCombine bounceCombine){
    PhysicsMaterialData data;
    data.friction = friction;
    data.bounciness = bounciness;
    data.frictionCombine = frictionCombine;
    data.bounceCombine = bounceCombine;
    return PhysicsMaterial(std::make_shared<PhysicsMaterialRuntime>(data, std::string()));
}

std::string PhysicsMaterial::getPath() const{
    return runtime ? runtime->getPath() : std::string();
}

float PhysicsMaterial::getFriction() const{
    return runtime ? runtime->getFriction() : 0.0f;
}

void PhysicsMaterial::setFriction(float friction){
    if (runtime) runtime->setFriction(friction);
}

float PhysicsMaterial::getBounciness() const{
    return runtime ? runtime->getBounciness() : 0.0f;
}

void PhysicsMaterial::setBounciness(float bounciness){
    if (runtime) runtime->setBounciness(bounciness);
}

PhysicsMaterialCombine PhysicsMaterial::getFrictionCombine() const{
    return runtime ? runtime->getFrictionCombine() : PhysicsMaterialCombine::NONE;
}

void PhysicsMaterial::setFrictionCombine(PhysicsMaterialCombine mode){
    if (runtime) runtime->setFrictionCombine(mode);
}

PhysicsMaterialCombine PhysicsMaterial::getBounceCombine() const{
    return runtime ? runtime->getBounceCombine() : PhysicsMaterialCombine::NONE;
}

void PhysicsMaterial::setBounceCombine(PhysicsMaterialCombine mode){
    if (runtime) runtime->setBounceCombine(mode);
}
