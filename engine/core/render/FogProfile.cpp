// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The built-ins bright_haze, murky, infernal and toxic take the looks of Vapor's Bright, Murky,
// Hell and Poison settings (github.com/ArthurBrussee/Vapor, MIT): the ideas only, the values are
// tuned for this fog's medium (docs/specs/S101-vapor.md §4).

#include "FogProfile.h"

#include "Log.h"
#include "asset/AssetMap.h"
#include "io/AtomicFile.h"
#include "io/Data.h"
#include "io/FileData.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

using namespace jasperengine;

using FpJson = nlohmann::ordered_json;

namespace jasperengine::fogprofile {
namespace {

    constexpr FogFieldInfo kFogFieldTable[FOG_FIELD_COUNT] = {
        {FogField::Color, "color", "Color", FogFieldKind::Color},
        {FogField::Density, "density", "Density", FogFieldKind::Float},
        {FogField::LinearStart, "linearStart", "Linear Start", FogFieldKind::Float},
        {FogField::LinearEnd, "linearEnd", "Linear End", FogFieldKind::Float},
        {FogField::ColorMode, "colorMode", "Color Mode", FogFieldKind::Enum},
        {FogField::AttenuationDistance, "attenuationDistance", "Attenuation Distance", FogFieldKind::Extinction},
        {FogField::BaseHeight, "baseHeight", "Base Height", FogFieldKind::Float},
        {FogField::MaximumHeight, "maximumHeight", "Maximum Height", FogFieldKind::Float},
        {FogField::StartDistance, "startDistance", "Start Distance", FogFieldKind::Float},
        {FogField::MaxFogDistance, "maxFogDistance", "Max Fog Distance", FogFieldKind::Float},
        {FogField::MaxOpacity, "maxOpacity", "Max Opacity", FogFieldKind::Float},
        {FogField::DirectionalInscattering, "directionalInscattering", "Directional Inscattering", FogFieldKind::Float},
        {FogField::Anisotropy, "anisotropy", "Anisotropy", FogFieldKind::Float},
        {FogField::SkyFill, "skyFill", "Sky Fill", FogFieldKind::Bool},
        {FogField::SkyFillAngle, "skyFillAngle", "Sky Fill Angle", FogFieldKind::Float},
        {FogField::NoiseIntensity, "noiseIntensity", "Noise Intensity", FogFieldKind::Float},
        {FogField::NoiseScale, "noiseScale", "Noise Scale", FogFieldKind::Float},
        {FogField::Wind, "wind", "Wind", FogFieldKind::Vector3},
        {FogField::Albedo, "albedo", "Albedo", FogFieldKind::Color},
        {FogField::AmbientMultiplier, "ambientMultiplier", "Ambient Multiplier", FogFieldKind::Float},
        {FogField::Emissive, "emissive", "Emissive", FogFieldKind::Color},
    };

    float* fogFloatMember(FogComponent& fog, FogField field){
        switch (field){
            case FogField::Density: return &fog.density;
            case FogField::LinearStart: return &fog.linearStart;
            case FogField::LinearEnd: return &fog.linearEnd;
            case FogField::AttenuationDistance: return &fog.attenuationDistance;
            case FogField::BaseHeight: return &fog.baseHeight;
            case FogField::MaximumHeight: return &fog.maximumHeight;
            case FogField::StartDistance: return &fog.startDistance;
            case FogField::MaxFogDistance: return &fog.maxFogDistance;
            case FogField::MaxOpacity: return &fog.maxOpacity;
            case FogField::DirectionalInscattering: return &fog.directionalInscattering;
            case FogField::Anisotropy: return &fog.anisotropy;
            case FogField::SkyFillAngle: return &fog.skyFillAngle;
            case FogField::NoiseIntensity: return &fog.noiseIntensity;
            case FogField::NoiseScale: return &fog.noiseScale;
            case FogField::AmbientMultiplier: return &fog.ambientMultiplier;
            default: return nullptr;
        }
    }

    Vector3* fogVectorMember(FogComponent& fog, FogField field){
        switch (field){
            case FogField::Color: return &fog.color;
            case FogField::Wind: return &fog.wind;
            case FogField::Albedo: return &fog.albedo;
            case FogField::Emissive: return &fog.emissive;
            default: return nullptr;
        }
    }

    const float* fogFloatMember(const FogComponent& fog, FogField field){
        return fogFloatMember(const_cast<FogComponent&>(fog), field);
    }

    const Vector3* fogVectorMember(const FogComponent& fog, FogField field){
        return fogVectorMember(const_cast<FogComponent&>(fog), field);
    }

    bool fogSameBits(float a, float b){
        return std::memcmp(&a, &b, sizeof(float)) == 0;
    }

    float fogLerp(float a, float b, float w){
        return a + (b - a) * w;
    }

    // the renderer's floor for a distance (AtmosphereMath, the passes): 1 / 0.01
    constexpr float kFogMinAttenuation = 0.01f;

    // ---- JSON ----

    // the shortest text that reads back as this float, read as a double: 0.3f writes 0.3
    std::string fogFloatText(float value){
        if (!std::isfinite(value))
            value = 0.0f;
        char buffer[64];
        char* end = nlohmann::detail::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::string(buffer, end);
    }

    std::string fogVectorText(const Vector3& value){
        return "[" + fogFloatText(value.x) + ", " + fogFloatText(value.y) + ", " + fogFloatText(value.z) + "]";
    }

    std::string fogStringText(const std::string& value){
        return FpJson(value).dump(-1, ' ', false, FpJson::error_handler_t::replace);
    }

    std::string fogValueText(const FogComponent& values, FogField field){
        switch (kFogFieldTable[static_cast<size_t>(field)].kind){
            case FogFieldKind::Float:
            case FogFieldKind::Extinction:
                return fogFloatText(*fogFloatMember(values, field));
            case FogFieldKind::Color:
            case FogFieldKind::Vector3:
                return fogVectorText(*fogVectorMember(values, field));
            case FogFieldKind::Bool:
                return values.skyFill ? "true" : "false";
            case FogFieldKind::Enum:
                return fogStringText(FogFields::colorModeName(values.colorMode));
        }
        return "null";
    }

    bool fogReadNumber(const FpJson& node, float& out){
        if (!node.is_number())
            return false;
        const double value = node.get<double>();
        if (!std::isfinite(value))
            return false;
        out = (float)value;
        return true;
    }

    bool fogReadValue(const FpJson& node, FogComponent& values, FogField field){
        switch (kFogFieldTable[static_cast<size_t>(field)].kind){
            case FogFieldKind::Float:
            case FogFieldKind::Extinction:
                return fogReadNumber(node, *fogFloatMember(values, field));
            case FogFieldKind::Color:
            case FogFieldKind::Vector3: {
                if (!node.is_array() || node.size() != 3)
                    return false;
                Vector3 value;
                if (!fogReadNumber(node[0], value.x) || !fogReadNumber(node[1], value.y) || !fogReadNumber(node[2], value.z))
                    return false;
                *fogVectorMember(values, field) = value;
                return true;
            }
            case FogFieldKind::Bool:
                if (!node.is_boolean())
                    return false;
                values.skyFill = node.get<bool>();
                return true;
            case FogFieldKind::Enum: {
                if (!node.is_string())
                    return false;
                FogColorMode mode;
                if (!FogFields::findColorMode(node.get<std::string>(), mode))
                    return false;
                values.colorMode = mode;
                return true;
            }
        }
        return false;
    }

    const char* fogExpectedText(FogField field){
        switch (kFogFieldTable[static_cast<size_t>(field)].kind){
            case FogFieldKind::Float:
            case FogFieldKind::Extinction: return "a number";
            case FogFieldKind::Color: return "a linear colour [r, g, b]";
            case FogFieldKind::Vector3: return "[x, y, z]";
            case FogFieldKind::Bool: return "true or false";
            case FogFieldKind::Enum: return "\"color\", \"sky\" or \"lit\"";
        }
        return "a value";
    }

    // ---- built-ins ----

    void putFogField(FogProfileData& data, FogField field){
        data.set.set(static_cast<size_t>(field));
    }

    // the fields applyFogPreset writes (AtmospherePresets.cpp): it forces Height, which a
    // profile cannot (the link does, R23)
    FogProfileData legacyFogLook(const char* name, const char* description, Vector3 srgb, float attenuation, float base, float maximum,
                          float start, float maxDistance, float maxOpacity, float inscattering, float anisotropy, float skyFillAngle){
        FogProfileData data;
        data.name = name;
        data.description = description;
        FogComponent& v = data.values;
        v.color = Color::sRGBToLinear(srgb);
        v.attenuationDistance = attenuation;
        v.baseHeight = base;
        v.maximumHeight = maximum;
        v.startDistance = start;
        v.maxFogDistance = maxDistance;
        v.maxOpacity = maxOpacity;
        v.directionalInscattering = inscattering;
        v.anisotropy = anisotropy;
        v.skyFill = true;
        v.skyFillAngle = skyFillAngle;
        for (FogField field : {FogField::Color, FogField::AttenuationDistance, FogField::BaseHeight, FogField::MaximumHeight,
                               FogField::StartDistance, FogField::MaxFogDistance, FogField::MaxOpacity,
                               FogField::DirectionalInscattering, FogField::Anisotropy, FogField::SkyFill, FogField::SkyFillAngle})
            putFogField(data, field);
        return data;
    }

    // a Lit look: every height and Lit field set, the colour too (the distance types and a
    // renderer without Lit read it), noise and wind
    struct FogLitLook{
        const char* name;
        const char* description;
        Vector3 colorSrgb;
        Vector3 albedo;
        float ambientMultiplier;
        Vector3 emissive;
        float attenuation;
        float base;
        float maximum;
        float maxDistance;
        float maxOpacity;
        float inscattering;
        float anisotropy;
        float skyFillAngle;
        float noiseIntensity;
        float noiseScale;
        Vector3 wind;
    };

    FogProfileData litFogLook(const FogLitLook& look){
        FogProfileData data;
        data.name = look.name;
        data.description = look.description;
        FogComponent& v = data.values;
        v.color = Color::sRGBToLinear(look.colorSrgb);
        v.colorMode = FogColorMode::LIT;
        v.albedo = look.albedo;
        v.ambientMultiplier = look.ambientMultiplier;
        v.emissive = look.emissive;
        v.attenuationDistance = look.attenuation;
        v.baseHeight = look.base;
        v.maximumHeight = look.maximum;
        v.startDistance = 0.0f;
        v.maxFogDistance = look.maxDistance;
        v.maxOpacity = look.maxOpacity;
        v.directionalInscattering = look.inscattering;
        v.anisotropy = look.anisotropy;
        v.skyFill = true;
        v.skyFillAngle = look.skyFillAngle;
        v.noiseIntensity = look.noiseIntensity;
        v.noiseScale = look.noiseScale;
        v.wind = look.wind;
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            const FogField field = static_cast<FogField>(i);
            if (field != FogField::Density && field != FogField::LinearStart && field != FogField::LinearEnd)
                putFogField(data, field);
        }
        return data;
    }

    // ---- the library ----

    struct FogLibraryEntry{
        std::shared_ptr<const FogProfileData> data;
        bool tried = false;         // read once (a missing file is not read again until it changes)
        std::string reason;         // why it did not read
        uint32_t version = 0;
    };

    struct FogLibraryState{
        std::map<std::string, FogLibraryEntry> entries;               // by normalized key
        std::unordered_map<std::string, std::string> alias; // a key as given -> its normalized key
        std::map<std::string, std::shared_ptr<FogProfileData>> runtime;
        uint32_t generation = 1;
        uint32_t nextVersion = 1;
        uint64_t nextRuntime = 1;
    };

    // made on first use, never destroyed (a game's scenes can outlive static destruction)
    FogLibraryState& fogLibraryState(){
        static FogLibraryState* instance = new FogLibraryState();
        return *instance;
    }

    void fogTouched(FogLibraryState& state, FogLibraryEntry& entry){
        entry.version = state.nextVersion++;
        state.generation++;
    }

    void readFogProfileFile(const std::string& path, FogLibraryEntry& entry){
        entry.tried = true;
        entry.data.reset();
        entry.reason.clear();
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            entry.reason = "cannot open the file";
            Log::warn("Fog Profile '%s': cannot open the file; the Fogs and Fog Zones using it take no values from it", path.c_str());
            return;
        }
        FogProfileData profile;
        std::string error;
        std::vector<std::string> unknown;
        std::vector<std::string> problems;
        if (!FogProfileIO::parse(data.readString(), profile, &error, &unknown, &problems)){
            entry.reason = error;
            Log::warn("Fog Profile '%s': %s; the Fogs and Fog Zones using it take no values from it", path.c_str(), error.c_str());
            return;
        }
        // one warning per file for what it holds that this engine does not read (R1)
        if (!unknown.empty()){
            std::string names;
            for (const std::string& name : unknown)
                names += (names.empty() ? "" : ", ") + name;
            Log::warn("Fog Profile '%s': unknown fields ignored: %s", path.c_str(), names.c_str());
        }
        for (const std::string& problem : problems)
            Log::warn("Fog Profile '%s': %s", path.c_str(), problem.c_str());
        entry.data = std::make_shared<const FogProfileData>(std::move(profile));
    }

    FogLibraryEntry* fogEntryOf(FogLibraryState& state, const std::string& key){
        auto alias = state.alias.find(key);
        const std::string& normal = alias != state.alias.end() ? alias->second : (state.alias[key] = FogProfileLibrary::normalizeKey(key));
        return &state.entries[normal];
    }

}
}

using namespace jasperengine::fogprofile;

// ---- FogFields ----

const FogFieldInfo& FogFields::info(FogField field){
    return kFogFieldTable[std::min(static_cast<size_t>(field), FOG_FIELD_COUNT - 1)];
}

const char* FogFields::name(FogField field){
    return info(field).name;
}

bool FogFields::find(std::string_view name, FogField& field){
    for (const FogFieldInfo& entry : kFogFieldTable){
        if (name == entry.name){
            field = entry.field;
            return true;
        }
    }
    return false;
}

void FogFields::copy(FogComponent& to, const FogComponent& from, FogField field){
    if (float* value = fogFloatMember(to, field)){
        *value = *fogFloatMember(from, field);
    }else if (Vector3* vector = fogVectorMember(to, field)){
        *vector = *fogVectorMember(from, field);
    }else if (field == FogField::SkyFill){
        to.skyFill = from.skyFill;
    }else if (field == FogField::ColorMode){
        to.colorMode = from.colorMode;
    }
}

bool FogFields::equal(const FogComponent& a, const FogComponent& b, FogField field){
    if (const float* value = fogFloatMember(a, field))
        return fogSameBits(*value, *fogFloatMember(b, field));
    if (const Vector3* vector = fogVectorMember(a, field)){
        const Vector3& other = *fogVectorMember(b, field);
        return fogSameBits(vector->x, other.x) && fogSameBits(vector->y, other.y) && fogSameBits(vector->z, other.z);
    }
    if (field == FogField::SkyFill)
        return a.skyFill == b.skyFill;
    if (field == FogField::ColorMode)
        return a.colorMode == b.colorMode;
    return true;
}

void FogFields::blend(FogComponent& value, const FogComponent& target, FogField field, float w){
    if (w <= 0.0f)
        return;
    if (w >= 1.0f){
        copy(value, target, field);
        return;
    }
    switch (info(field).kind){
        case FogFieldKind::Float: {
            float& v = *fogFloatMember(value, field);
            v = fogLerp(v, *fogFloatMember(target, field), w);
            break;
        }
        case FogFieldKind::Extinction: {
            // blended as extinction (1/d): fog thickens evenly, not all at the thin end (R8)
            float& v = *fogFloatMember(value, field);
            const float a = 1.0f / std::max(v, kFogMinAttenuation);
            const float b = 1.0f / std::max(*fogFloatMember(target, field), kFogMinAttenuation);
            v = 1.0f / fogLerp(a, b, w);
            break;
        }
        case FogFieldKind::Color:
        case FogFieldKind::Vector3: {
            Vector3& v = *fogVectorMember(value, field);
            const Vector3& t = *fogVectorMember(target, field);
            v = Vector3(fogLerp(v.x, t.x, w), fogLerp(v.y, t.y, w), fogLerp(v.z, t.z, w));
            break;
        }
        case FogFieldKind::Bool:
        case FogFieldKind::Enum:
            if (w >= 0.5f)
                copy(value, target, field);
            break;
    }
}

int FogFields::width(FogField field){
    const FogFieldKind kind = info(field).kind;
    return kind == FogFieldKind::Color || kind == FogFieldKind::Vector3 ? 3 : 1;
}

void FogFields::read(const FogComponent& fog, FogField field, float* out){
    if (const float* value = fogFloatMember(fog, field)){
        out[0] = *value;
    }else if (const Vector3* vector = fogVectorMember(fog, field)){
        out[0] = vector->x;
        out[1] = vector->y;
        out[2] = vector->z;
    }else if (field == FogField::SkyFill){
        out[0] = fog.skyFill ? 1.0f : 0.0f;
    }else if (field == FogField::ColorMode){
        out[0] = (float)fog.colorMode;
    }
}

void FogFields::write(FogComponent& fog, FogField field, const float* in){
    if (float* value = fogFloatMember(fog, field)){
        *value = in[0];
    }else if (Vector3* vector = fogVectorMember(fog, field)){
        *vector = Vector3(in[0], in[1], in[2]);
    }else if (field == FogField::SkyFill){
        fog.skyFill = in[0] > 0.5f;
    }else if (field == FogField::ColorMode){
        const int mode = (int)std::lround(in[0]);
        fog.colorMode = mode == 1 ? FogColorMode::SKY : mode == 2 ? FogColorMode::LIT : FogColorMode::COLOR;
    }
}

bool FogFields::readByType(FogType type, FogField field){
    const bool distanceField = field == FogField::Color || field == FogField::Density || field == FogField::LinearStart ||
                               field == FogField::LinearEnd;
    if (type != FogType::HEIGHT)
        return distanceField;
    return field == FogField::Color || !distanceField;
}

const char* FogFields::colorModeName(FogColorMode mode){
    switch (mode){
        case FogColorMode::COLOR: return "color";
        case FogColorMode::SKY: return "sky";
        case FogColorMode::LIT: return "lit";
    }
    return "color";
}

bool FogFields::findColorMode(std::string_view name, FogColorMode& mode){
    for (FogColorMode candidate : {FogColorMode::COLOR, FogColorMode::SKY, FogColorMode::LIT}){
        if (name == colorModeName(candidate)){
            mode = candidate;
            return true;
        }
    }
    return false;
}

// ---- FogProfileData ----

void FogProfileData::setFrom(const FogComponent& from, FogField field){
    FogFields::copy(values, from, field);
    set.set(static_cast<size_t>(field));
}

void FogProfileData::clear(FogField field){
    set.reset(static_cast<size_t>(field));
    FogFields::copy(values, FogComponent(), field);
}

FogProfileData FogProfileData::fromFog(const FogComponent& fog, const std::string& name){
    FogProfileData data;
    data.name = name;
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++)
        data.setFrom(fog, static_cast<FogField>(i));
    return data;
}

void FogProfileData::applyTo(FogComponent& fog) const{
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
        if (set.test(i))
            FogFields::copy(fog, values, static_cast<FogField>(i));
    }
}

bool FogProfileData::operator==(const FogProfileData& other) const{
    if (name != other.name || description != other.description || set != other.set)
        return false;
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
        if (set.test(i) && !FogFields::equal(values, other.values, static_cast<FogField>(i)))
            return false;
    }
    return true;
}

// ---- FogProfileIO ----

bool FogProfileIO::parse(const std::string& json, FogProfileData& out, std::string* error, std::vector<std::string>* unknown,
                         std::vector<std::string>* problems){
    auto fail = [error](const std::string& message){
        if (error)
            *error = message;
        return false;
    };
    const FpJson root = FpJson::parse(json, nullptr, false);
    if (root.is_discarded())
        return fail("not JSON");
    if (!root.is_object())
        return fail("not a Fog Profile (a JSON object)");
    if (!root.contains("version") || !root["version"].is_number_integer())
        return fail("version: expected 1");
    const int version = root["version"].get<int>();
    if (version > VERSION)
        return fail("written by a newer JasperEngine (fog profile version " + std::to_string(version) + "; this one reads " +
                    std::to_string(VERSION) + ")");
    if (version < 1)
        return fail("version: expected 1");

    FogProfileData data;
    if (root.contains("name")){
        if (root["name"].is_string())
            data.name = root["name"].get<std::string>();
        else if (problems)
            problems->push_back("name: expected a string");
    }
    if (root.contains("description")){
        if (root["description"].is_string())
            data.description = root["description"].get<std::string>();
        else if (problems)
            problems->push_back("description: expected a string");
    }
    for (auto it = root.begin(); it != root.end(); ++it){
        if (it.key() != "version" && it.key() != "name" && it.key() != "description" && it.key() != "fields" && unknown)
            unknown->push_back(it.key());
    }
    if (root.contains("fields")){
        const FpJson& fields = root["fields"];
        if (!fields.is_object())
            return fail("fields: expected an object");
        for (auto it = fields.begin(); it != fields.end(); ++it){
            FogField field;
            if (!FogFields::find(it.key(), field)){
                if (unknown)
                    unknown->push_back("fields." + it.key());
                continue;
            }
            if (!fogReadValue(it.value(), data.values, field)){
                FogFields::copy(data.values, FogComponent(), field);
                if (problems)
                    problems->push_back("fields." + it.key() + ": expected " + fogExpectedText(field) + "; the field is left unset");
                continue;
            }
            data.set.set(static_cast<size_t>(field));
        }
    }
    out = std::move(data);
    return true;
}

std::string FogProfileIO::write(const FogProfileData& data){
    std::string text = "{\n";
    text += "  \"version\": " + std::to_string(VERSION) + ",\n";
    text += "  \"name\": " + fogStringText(data.name) + ",\n";
    if (!data.description.empty())
        text += "  \"description\": " + fogStringText(data.description) + ",\n";
    if (data.set.none()){
        text += "  \"fields\": {}\n";
    }else{
        text += "  \"fields\": {\n";
        bool first = true;
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            if (!data.set.test(i))
                continue;
            const FogField field = static_cast<FogField>(i);
            text += first ? "" : ",\n";
            text += "    \"" + std::string(kFogFieldTable[i].name) + "\": " + fogValueText(data.values, field);
            first = false;
        }
        text += "\n  }\n";
    }
    text += "}\n";
    return text;
}

bool FogProfileIO::save(const std::filesystem::path& file, const FogProfileData& data, std::string* error){
    const io::WriteResult result = io::writeFileAtomic(file, write(data));
    if (!result.ok()){
        if (error)
            *error = io::describeWriteError(result);
        return false;
    }
    return true;
}

bool FogProfileIO::isProfilePath(std::string_view path){
    constexpr std::string_view extension = EXTENSION;
    if (path.size() <= extension.size())
        return false;
    const std::string_view tail = path.substr(path.size() - extension.size());
    for (size_t i = 0; i < extension.size(); i++){
        if (std::tolower((unsigned char)tail[i]) != extension[i])
            return false;
    }
    return true;
}

// ---- FogProfiles ----

const std::vector<FogProfileBuiltin>& FogProfiles::builtins(){
    static const std::vector<FogProfileBuiltin> all = {
        // AtmospherePresets' five, field for field (FogProfileTest.BuiltinsMatchLegacyPresets)
        {"morning_haze", legacyFogLook("Morning Haze", "A thin warm haze lit by a low sun, the sky's colour at the horizon",
                                Vector3(0.86f, 0.80f, 0.72f), 600.0f, 0.0f, 120.0f, 0.0f, 5000.0f, 0.9f, 0.6f, 0.7f, 6.0f)},
        {"ground_mist", legacyFogLook("Ground Mist", "A dense white layer a few meters deep, clear above",
                               Vector3(0.82f, 0.84f, 0.87f), 40.0f, 0.0f, 6.0f, 0.0f, 1000.0f, 1.0f, 0.2f, 0.5f, 2.0f)},
        {"mountain_valley", legacyFogLook("Mountain Valley", "Fog filling the valleys, peaks above it, long views",
                                   Vector3(0.74f, 0.79f, 0.86f), 250.0f, 20.0f, 300.0f, 0.0f, 4000.0f, 1.0f, 0.4f, 0.6f, 3.0f)},
        {"horizon_haze", legacyFogLook("Horizon Haze", "Clear nearby, the distance fading into the sky",
                                Vector3(0.72f, 0.80f, 0.90f), 3000.0f, 0.0f, 1500.0f, 200.0f, 20000.0f, 1.0f, 0.3f, 0.75f, 8.0f)},
        {"night", legacyFogLook("Night", "A dark blue fog close to the ground, little sun scattering",
                         Vector3(0.06f, 0.07f, 0.11f), 200.0f, 0.0f, 60.0f, 0.0f, 2000.0f, 0.95f, 0.05f, 0.3f, 4.0f)},
        // Lit looks, after Vapor's Bright, Murky, Hell and Poison
        {"bright_haze", litFogLook({"Bright Haze", "A thin sky-blue haze that takes the daylight, drifting slowly",
                             Vector3(0.70f, 0.80f, 0.93f), Vector3(0.50f, 0.70f, 1.00f), 4.0f, Vector3(0.0f, 0.0f, 0.0f),
                             280.0f, 0.0f, 250.0f, 6000.0f, 0.85f, 0.35f, 0.7f, 6.0f, 0.25f, 60.0f, Vector3(1.2f, 0.0f, 0.4f)})},
        {"murky", litFogLook({"Murky", "Dark, soupy fog that drinks the light",
                       Vector3(0.33f, 0.32f, 0.27f), Vector3(0.09f, 0.085f, 0.06f), 4.0f, Vector3(0.0f, 0.0f, 0.0f),
                       25.0f, 12.0f, 60.0f, 1500.0f, 1.0f, 0.15f, 0.2f, 12.0f, 0.5f, 12.0f, Vector3(0.5f, 0.0f, 0.2f)})},
        {"infernal", litFogLook({"Infernal", "Dense red fog glowing from within, rising like heat",
                          Vector3(0.80f, 0.22f, 0.10f), Vector3(0.25f, 0.03f, 0.01f), 0.5f, Vector3(0.55f, 0.025f, 0.0f),
                          25.0f, 8.0f, 60.0f, 1500.0f, 1.0f, 0.3f, 0.4f, 4.0f, 0.6f, 10.0f, Vector3(0.0f, 1.5f, 0.0f)})},
        {"toxic", litFogLook({"Toxic", "A green glowing gas pooling on the ground, thinning fast with height",
                       Vector3(0.42f, 0.80f, 0.24f), Vector3(0.22f, 0.55f, 0.07f), 0.6f, Vector3(0.07f, 0.45f, 0.02f),
                       8.0f, 1.0f, 7.0f, 1500.0f, 1.0f, 0.2f, 0.3f, 2.0f, 0.35f, 8.0f, Vector3(0.4f, 0.0f, 0.4f)})},
    };
    return all;
}

const FogProfileBuiltin* FogProfiles::findBuiltin(std::string_view idOrKey){
    std::string_view id = idOrKey;
    if (isBuiltinKey(id))
        id = id.substr(std::strlen(BUILTIN_PREFIX));
    for (const FogProfileBuiltin& builtin : builtins()){
        if (id == builtin.id)
            return &builtin;
    }
    return nullptr;
}

bool FogProfiles::isBuiltinKey(std::string_view key){
    return key.rfind(BUILTIN_PREFIX, 0) == 0;
}

std::string FogProfiles::builtinKey(std::string_view id){
    return std::string(BUILTIN_PREFIX) + std::string(id);
}

std::string FogProfiles::builtinCopyPath(std::string_view id){
    return "FogProfiles/" + std::string(id) + FogProfileIO::EXTENSION;
}

// ---- FogProfileLibrary ----

std::string FogProfileLibrary::normalizeKey(const std::string& key){
    if (FogProfiles::isBuiltinKey(key) || isRuntimeKey(key))
        return key;
    std::string text = key;
    std::replace(text.begin(), text.end(), '\\', '/');
    const std::u8string normal = std::filesystem::path(std::u8string(text.begin(), text.end())).lexically_normal().generic_u8string();
    return std::string(normal.begin(), normal.end());
}

std::shared_ptr<const FogProfileData> FogProfileLibrary::get(const std::string& key){
    if (key.empty())
        return nullptr;
    FogLibraryState& state = fogLibraryState();
    if (isRuntimeKey(key)){
        auto found = state.runtime.find(key);
        return found != state.runtime.end() ? found->second : nullptr;
    }
    // a GUID names whatever path the asset map has for it now
    if (key.size() == 32 && Guid::isValidText(key)){
        AssetType type = AssetType::Unknown;
        std::string path;
        if (AssetMap::findPath(Guid::parse(key), path, &type) && type == AssetType::FogProfile && path.size() != 32)
            return get(path);
        return nullptr;
    }
    FogLibraryEntry& entry = *fogEntryOf(state, key);
    if (!entry.tried){
        entry.tried = true;
        if (FogProfiles::isBuiltinKey(key)){
            if (const FogProfileBuiltin* builtin = FogProfiles::findBuiltin(key))
                entry.data = std::make_shared<const FogProfileData>(builtin->data);
            else
                entry.reason = "no built-in fog profile '" + key + "'";
        }else{
            readFogProfileFile(normalizeKey(key), entry);
        }
        fogTouched(state, entry);
    }
    return entry.data;
}

std::shared_ptr<const FogProfileData> FogProfileLibrary::getRef(const AssetRef& ref){
    if (!ref.path.empty())
        return get(ref.path);
    if (!ref.guid.isNull())
        return get(ref.guid.str());
    return nullptr;
}

bool FogProfileLibrary::isMissing(const std::string& key, std::string* reason){
    if (key.empty())
        return false;
    if (isRuntimeKey(key)){
        const bool missing = !getRuntime(key);
        if (missing && reason)
            *reason = "the runtime profile '" + key + "' is gone";
        return missing;
    }
    if (get(key))
        return false;
    if (key.size() == 32 && Guid::isValidText(key)){
        if (reason)
            *reason = "no fog profile has the GUID " + key;
        return true;
    }
    const FogLibraryEntry& entry = *fogEntryOf(fogLibraryState(), key);
    if (entry.data)
        return false;
    if (reason)
        *reason = entry.reason;
    return true;
}

void FogProfileLibrary::reload(const std::string& path){
    FogLibraryState& state = fogLibraryState();
    auto found = state.entries.find(normalizeKey(path));
    // never read: its first use reads it
    if (found == state.entries.end() || !found->second.tried)
        return;
    readFogProfileFile(found->first, found->second);
    fogTouched(state, found->second);
}

void FogProfileLibrary::set(const std::string& path, const FogProfileData& data){
    FogLibraryState& state = fogLibraryState();
    FogLibraryEntry& entry = *fogEntryOf(state, path);
    entry.tried = true;
    entry.reason.clear();
    entry.data = std::make_shared<const FogProfileData>(data);
    fogTouched(state, entry);
}

bool FogProfileLibrary::load(const std::string& path, const std::string& json, std::string* error){
    FogProfileData data;
    if (!FogProfileIO::parse(json, data, error))
        return false;
    set(path, data);
    return true;
}

void FogProfileLibrary::clear(){
    FogLibraryState& state = fogLibraryState();
    state.entries.clear();
    state.alias.clear();
    state.runtime.clear();
    state.generation++;
}

std::string FogProfileLibrary::addRuntime(std::shared_ptr<FogProfileData> profile){
    FogLibraryState& state = fogLibraryState();
    const std::string key = "runtime:" + std::to_string(state.nextRuntime++);
    state.runtime[key] = std::move(profile);
    state.generation++;
    return key;
}

std::shared_ptr<FogProfileData> FogProfileLibrary::getRuntime(const std::string& key){
    FogLibraryState& state = fogLibraryState();
    auto found = state.runtime.find(key);
    return found != state.runtime.end() ? found->second : nullptr;
}

bool FogProfileLibrary::isRuntimeKey(std::string_view key){
    return key.rfind("runtime:", 0) == 0;
}

void FogProfileLibrary::resetRuntime(){
    FogLibraryState& state = fogLibraryState();
    if (state.runtime.empty())
        return;
    state.runtime.clear();
    state.generation++;
}

uint32_t FogProfileLibrary::version(const std::string& key){
    FogLibraryState& state = fogLibraryState();
    auto found = state.entries.find(normalizeKey(key));
    return found != state.entries.end() ? found->second.version : 0;
}

uint32_t FogProfileLibrary::generation(){
    return fogLibraryState().generation;
}
