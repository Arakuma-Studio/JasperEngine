// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MaterialPbr.h"

#include "Engine.h"
#include "texture/Material.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

using namespace jasperengine;

bool MaterialPbrExtensions::Clearcoat::operator == (const Clearcoat& o) const{
    return enabled == o.enabled && factor == o.factor && roughness == o.roughness && normalScale == o.normalScale &&
           map == o.map && normalMap == o.normalMap && mapTexCoord == o.mapTexCoord && normalTexCoord == o.normalTexCoord;
}

bool MaterialPbrExtensions::Sheen::operator == (const Sheen& o) const{
    return enabled == o.enabled && color == o.color && roughness == o.roughness && map == o.map && mapTexCoord == o.mapTexCoord;
}

bool MaterialPbrExtensions::Anisotropy::operator == (const Anisotropy& o) const{
    return enabled == o.enabled && strength == o.strength && rotation == o.rotation && map == o.map && mapTexCoord == o.mapTexCoord;
}

bool MaterialPbrExtensions::Iridescence::operator == (const Iridescence& o) const{
    return enabled == o.enabled && factor == o.factor && ior == o.ior && thicknessNm == o.thicknessNm && map == o.map &&
           mapTexCoord == o.mapTexCoord;
}

bool MaterialPbrExtensions::Transmission::operator == (const Transmission& o) const{
    return enabled == o.enabled && factor == o.factor && map == o.map && mapTexCoord == o.mapTexCoord;
}

bool MaterialPbrExtensions::Volume::operator == (const Volume& o) const{
    return enabled == o.enabled && thickness == o.thickness && attenuationDistance == o.attenuationDistance &&
           attenuationColor == o.attenuationColor;
}

bool MaterialPbrExtensions::Specular::operator == (const Specular& o) const{
    return enabled == o.enabled && factor == o.factor && ior == o.ior && color == o.color && map == o.map && mapTexCoord == o.mapTexCoord;
}

bool MaterialPbrExtensions::Hex::operator == (const Hex& o) const{
    return enabled == o.enabled && rotation == o.rotation && contrast == o.contrast;
}

bool MaterialPbrExtensions::Hair::operator == (const Hair& o) const{
    return enabled == o.enabled && primaryColor == o.primaryColor && primaryShift == o.primaryShift &&
           primaryWidth == o.primaryWidth && secondaryColor == o.secondaryColor && secondaryShift == o.secondaryShift &&
           secondaryWidth == o.secondaryWidth && rotation == o.rotation && flowMap == o.flowMap &&
           flowMapTexCoord == o.flowMapTexCoord;
}

bool MaterialPbrExtensions::operator == (const MaterialPbrExtensions& o) const{
    return clearcoat == o.clearcoat && sheen == o.sheen && anisotropy == o.anisotropy && iridescence == o.iridescence &&
           transmission == o.transmission && volume == o.volume && specular == o.specular && hex == o.hex &&
           hair == o.hair;
}

namespace {

    const float INF = std::numeric_limits<float>::infinity();

    template<typename T, typename Lobe, Lobe MaterialPbrExtensions::*lobe, T Lobe::*member>
    void* fieldRef(MaterialPbrExtensions& pbr){
        return &(pbr.*lobe.*member);
    }

    #define PBR_REF(lobeMember, LobeType, member, MemberType) \
        &fieldRef<MemberType, MaterialPbrExtensions::LobeType, &MaterialPbrExtensions::lobeMember, &MaterialPbrExtensions::LobeType::member>

    // the project export setting's default (Project Settings > Graphics > Mobile PBR Tier)
    std::atomic<int> mobileTierValue{(int)PbrTier::REDUCED};

    // the lobes' members of u_fs_pbrParams (mesh.frag, includes/pbr_ext.glsl): the vec4 each lobe writes
    enum PbrUniformVector{
        UNIFORM_CLEARCOAT = 0,
        UNIFORM_SHEEN = 4,
        UNIFORM_ANISOTROPY = 8,
        UNIFORM_IRIDESCENCE = 12,
        UNIFORM_TRANSMISSION = 16,
        UNIFORM_ATTENUATION = 20,
        UNIFORM_SPECULAR = 24,
        UNIFORM_IOR_HEX = 28
    };

    // the two-letter codes of a builtin:lit basename, by define
    struct DefineCode{
        const char* define;
        const char* code;
    };
    const DefineCode defineCodes[] = {
        {"PBR_CLEARCOAT", "Cc"}, {"PBR_CLEARCOAT_MAP", "Cm"}, {"PBR_CLEARCOAT_NORMAL_MAP", "Cn"},
        {"PBR_SHEEN", "Sh"}, {"PBR_SHEEN_MAP", "Sm"},
        {"PBR_ANISOTROPY", "An"}, {"PBR_ANISOTROPY_MAP", "Am"},
        {"PBR_IRIDESCENCE", "Ir"}, {"PBR_IRIDESCENCE_MAP", "Im"},
        {"PBR_TRANSMISSION", "Tr"}, {"PBR_TRANSMISSION_MAP", "Tm"},
        {"PBR_VOLUME", "Vo"},
        {"PBR_SPECULAR", "Sp"}, {"PBR_SPECULAR_MAP", "Sq"},
        {"PBR_HEX_TILING", "Hx"}, {"TERRAIN_HEX_TILING", "Th"},
        {"USE_LIT_QUALITY", "Lq"},
        {"TERRAIN_HEX_STOCHASTIC", "Ts"},
        {"PBR_HAIR", "Hr"}, {"PBR_HAIR_MAP", "Hf"},
        {"HAS_VOXEL", "Vx"}, {"VOXEL_TIER_HIGH", "Vh"}, {"VOXEL_TIER_MEDIUM", "Vm"}, {"VOXEL_TIER_LOW", "Vl"},
    };
    // T3 R54: a bit per entry of a 64-bit mask, so the table may grow past 32 and no further
    static_assert(sizeof(defineCodes) / sizeof(defineCodes[0]) <= 64, "MaterialPbr's define bits are 64 wide");

}

const char* const MaterialPbr::LIT_QUALITY_DEFINE = "USE_LIT_QUALITY";
const char* const MaterialPbr::VOXEL_DEFINE = "HAS_VOXEL";
const char* const MaterialPbr::VOXEL_TIER_HIGH_DEFINE = "VOXEL_TIER_HIGH";
const char* const MaterialPbr::VOXEL_TIER_MEDIUM_DEFINE = "VOXEL_TIER_MEDIUM";
const char* const MaterialPbr::VOXEL_TIER_LOW_DEFINE = "VOXEL_TIER_LOW";
const char* const MaterialPbr::TERRAIN_HEX_DEFINE = "TERRAIN_HEX_TILING";
const char* const MaterialPbr::TERRAIN_HEX_STOCHASTIC_DEFINE = "TERRAIN_HEX_STOCHASTIC";
const char* const MaterialPbr::TRANSMISSION_DEFINE = "PBR_TRANSMISSION";

const std::vector<PbrLobeInfo>& MaterialPbr::getLobes(){
    static const std::vector<PbrLobeInfo> lobes = {
        {PbrLobe::CLEARCOAT, "clearcoat", "Clear Coat", "PBR_CLEARCOAT"},
        {PbrLobe::SHEEN, "sheen", "Sheen", "PBR_SHEEN"},
        {PbrLobe::ANISOTROPY, "anisotropy", "Anisotropy", "PBR_ANISOTROPY"},
        {PbrLobe::IRIDESCENCE, "iridescence", "Iridescence", "PBR_IRIDESCENCE"},
        {PbrLobe::TRANSMISSION, "transmission", "Transmission", "PBR_TRANSMISSION"},
        {PbrLobe::VOLUME, "volume", "Volume", "PBR_VOLUME"},
        {PbrLobe::SPECULAR, "specular", "Specular & IOR", "PBR_SPECULAR"},
        {PbrLobe::HEX, "hex", "Hex Tiling", "PBR_HEX_TILING"},
        {PbrLobe::HAIR, "hair", "Hair", "PBR_HAIR"},
    };
    return lobes;
}

const PbrLobeInfo& MaterialPbr::getLobe(PbrLobe lobe){
    return getLobes()[(size_t)lobe];
}

const PbrLobeInfo* MaterialPbr::findLobe(const std::string& name){
    for (const PbrLobeInfo& info : getLobes()){
        if (name == info.name)
            return &info;
    }
    return nullptr;
}

bool& MaterialPbr::lobeEnabled(MaterialPbrExtensions& pbr, PbrLobe lobe){
    switch (lobe){
        case PbrLobe::CLEARCOAT:    return pbr.clearcoat.enabled;
        case PbrLobe::SHEEN:        return pbr.sheen.enabled;
        case PbrLobe::ANISOTROPY:   return pbr.anisotropy.enabled;
        case PbrLobe::IRIDESCENCE:  return pbr.iridescence.enabled;
        case PbrLobe::TRANSMISSION: return pbr.transmission.enabled;
        case PbrLobe::VOLUME:       return pbr.volume.enabled;
        case PbrLobe::SPECULAR:     return pbr.specular.enabled;
        case PbrLobe::HAIR:         return pbr.hair.enabled;
        default:                    return pbr.hex.enabled;
    }
}

bool MaterialPbr::isLobeEnabled(const MaterialPbrExtensions& pbr, PbrLobe lobe){
    return lobeEnabled(const_cast<MaterialPbrExtensions&>(pbr), lobe);
}

const std::vector<PbrField>& MaterialPbr::getFields(){
    static const std::vector<PbrField> fields = {
        {PbrLobe::CLEARCOAT, "enabled", PbrFieldType::Bool, PBR_REF(clearcoat, Clearcoat, enabled, bool), 0, 1, "Clear Coat", "clearcoatEnabled", PbrMap::COUNT},
        {PbrLobe::CLEARCOAT, "factor", PbrFieldType::Float, PBR_REF(clearcoat, Clearcoat, factor, float), 0, 1, "Factor", "clearcoatFactor", PbrMap::COUNT},
        {PbrLobe::CLEARCOAT, "roughness", PbrFieldType::Float, PBR_REF(clearcoat, Clearcoat, roughness, float), 0, 1, "Roughness", "clearcoatRoughness", PbrMap::COUNT},
        {PbrLobe::CLEARCOAT, "normalScale", PbrFieldType::Float, PBR_REF(clearcoat, Clearcoat, normalScale, float), -INF, INF, "Normal Scale", "clearcoatNormalScale", PbrMap::COUNT},
        {PbrLobe::CLEARCOAT, "map", PbrFieldType::Texture, PBR_REF(clearcoat, Clearcoat, map, Texture), 0, 0, "Map", nullptr, PbrMap::CLEARCOAT},
        {PbrLobe::CLEARCOAT, "mapTexCoord", PbrFieldType::TexCoord, PBR_REF(clearcoat, Clearcoat, mapTexCoord, int), 0, 1, "Map UV Set", nullptr, PbrMap::CLEARCOAT},
        {PbrLobe::CLEARCOAT, "normalMap", PbrFieldType::Texture, PBR_REF(clearcoat, Clearcoat, normalMap, Texture), 0, 0, "Normal Map", nullptr, PbrMap::CLEARCOAT_NORMAL},
        {PbrLobe::CLEARCOAT, "normalTexCoord", PbrFieldType::TexCoord, PBR_REF(clearcoat, Clearcoat, normalTexCoord, int), 0, 1, "Normal Map UV Set", nullptr, PbrMap::CLEARCOAT_NORMAL},

        {PbrLobe::SHEEN, "enabled", PbrFieldType::Bool, PBR_REF(sheen, Sheen, enabled, bool), 0, 1, "Sheen", "sheenEnabled", PbrMap::COUNT},
        {PbrLobe::SHEEN, "color", PbrFieldType::Color, PBR_REF(sheen, Sheen, color, Vector3), 0, 1, "Color", "sheenColor", PbrMap::COUNT},
        {PbrLobe::SHEEN, "roughness", PbrFieldType::Float, PBR_REF(sheen, Sheen, roughness, float), 0, 1, "Roughness", "sheenRoughness", PbrMap::COUNT},
        {PbrLobe::SHEEN, "map", PbrFieldType::Texture, PBR_REF(sheen, Sheen, map, Texture), 0, 0, "Map", nullptr, PbrMap::SHEEN},
        {PbrLobe::SHEEN, "mapTexCoord", PbrFieldType::TexCoord, PBR_REF(sheen, Sheen, mapTexCoord, int), 0, 1, "Map UV Set", nullptr, PbrMap::SHEEN},

        {PbrLobe::ANISOTROPY, "enabled", PbrFieldType::Bool, PBR_REF(anisotropy, Anisotropy, enabled, bool), 0, 1, "Anisotropy", "anisotropyEnabled", PbrMap::COUNT},
        {PbrLobe::ANISOTROPY, "strength", PbrFieldType::Float, PBR_REF(anisotropy, Anisotropy, strength, float), 0, 1, "Strength", "anisotropyStrength", PbrMap::COUNT},
        {PbrLobe::ANISOTROPY, "rotation", PbrFieldType::Float, PBR_REF(anisotropy, Anisotropy, rotation, float), -INF, INF, "Rotation", "anisotropyRotation", PbrMap::COUNT},
        {PbrLobe::ANISOTROPY, "map", PbrFieldType::Texture, PBR_REF(anisotropy, Anisotropy, map, Texture), 0, 0, "Map", nullptr, PbrMap::ANISOTROPY},
        {PbrLobe::ANISOTROPY, "mapTexCoord", PbrFieldType::TexCoord, PBR_REF(anisotropy, Anisotropy, mapTexCoord, int), 0, 1, "Map UV Set", nullptr, PbrMap::ANISOTROPY},

        {PbrLobe::IRIDESCENCE, "enabled", PbrFieldType::Bool, PBR_REF(iridescence, Iridescence, enabled, bool), 0, 1, "Iridescence", "iridescenceEnabled", PbrMap::COUNT},
        {PbrLobe::IRIDESCENCE, "factor", PbrFieldType::Float, PBR_REF(iridescence, Iridescence, factor, float), 0, 1, "Factor", "iridescenceFactor", PbrMap::COUNT},
        {PbrLobe::IRIDESCENCE, "ior", PbrFieldType::Float, PBR_REF(iridescence, Iridescence, ior, float), 1, INF, "IOR", "iridescenceIor", PbrMap::COUNT},
        {PbrLobe::IRIDESCENCE, "thicknessNm", PbrFieldType::Vector2, PBR_REF(iridescence, Iridescence, thicknessNm, Vector2), 0, INF, "Thickness (nm)", "iridescenceThicknessRange", PbrMap::COUNT},
        {PbrLobe::IRIDESCENCE, "map", PbrFieldType::Texture, PBR_REF(iridescence, Iridescence, map, Texture), 0, 0, "Map", nullptr, PbrMap::IRIDESCENCE},
        {PbrLobe::IRIDESCENCE, "mapTexCoord", PbrFieldType::TexCoord, PBR_REF(iridescence, Iridescence, mapTexCoord, int), 0, 1, "Map UV Set", nullptr, PbrMap::IRIDESCENCE},

        {PbrLobe::TRANSMISSION, "enabled", PbrFieldType::Bool, PBR_REF(transmission, Transmission, enabled, bool), 0, 1, "Transmission", "transmissionEnabled", PbrMap::COUNT},
        {PbrLobe::TRANSMISSION, "factor", PbrFieldType::Float, PBR_REF(transmission, Transmission, factor, float), 0, 1, "Factor", "transmissionFactor", PbrMap::COUNT},
        {PbrLobe::TRANSMISSION, "map", PbrFieldType::Texture, PBR_REF(transmission, Transmission, map, Texture), 0, 0, "Map", nullptr, PbrMap::TRANSMISSION},
        {PbrLobe::TRANSMISSION, "mapTexCoord", PbrFieldType::TexCoord, PBR_REF(transmission, Transmission, mapTexCoord, int), 0, 1, "Map UV Set", nullptr, PbrMap::TRANSMISSION},

        {PbrLobe::VOLUME, "enabled", PbrFieldType::Bool, PBR_REF(volume, Volume, enabled, bool), 0, 1, "Volume", "volumeEnabled", PbrMap::COUNT},
        {PbrLobe::VOLUME, "thickness", PbrFieldType::Float, PBR_REF(volume, Volume, thickness, float), 0, INF, "Thickness", "thicknessFactor", PbrMap::COUNT},
        {PbrLobe::VOLUME, "attenuationColor", PbrFieldType::Color, PBR_REF(volume, Volume, attenuationColor, Vector3), 0, 1, "Attenuation Color", "attenuationColor", PbrMap::COUNT},
        // > 0; infinity (the default) absorbs nothing
        {PbrLobe::VOLUME, "attenuationDistance", PbrFieldType::Float, PBR_REF(volume, Volume, attenuationDistance, float), 1e-6f, INF, "Attenuation Distance", "attenuationDistance", PbrMap::COUNT},

        {PbrLobe::SPECULAR, "enabled", PbrFieldType::Bool, PBR_REF(specular, Specular, enabled, bool), 0, 1, "Specular & IOR", "specularEnabled", PbrMap::COUNT},
        {PbrLobe::SPECULAR, "factor", PbrFieldType::Float, PBR_REF(specular, Specular, factor, float), 0, 1, "Factor", "specularFactor", PbrMap::COUNT},
        {PbrLobe::SPECULAR, "color", PbrFieldType::Color, PBR_REF(specular, Specular, color, Vector3), 0, INF, "Color", "specularColor", PbrMap::COUNT},
        // >= 1, or 0 (glTF: no dielectric falloff, f0 = 1)
        {PbrLobe::SPECULAR, "ior", PbrFieldType::Float, PBR_REF(specular, Specular, ior, float), 1, INF, "IOR", "ior", PbrMap::COUNT},
        {PbrLobe::SPECULAR, "map", PbrFieldType::Texture, PBR_REF(specular, Specular, map, Texture), 0, 0, "Map", nullptr, PbrMap::SPECULAR},
        {PbrLobe::SPECULAR, "mapTexCoord", PbrFieldType::TexCoord, PBR_REF(specular, Specular, mapTexCoord, int), 0, 1, "Map UV Set", nullptr, PbrMap::SPECULAR},

        {PbrLobe::HEX, "enabled", PbrFieldType::Bool, PBR_REF(hex, Hex, enabled, bool), 0, 1, "Hex Tiling", "hexTilingEnabled", PbrMap::COUNT},
        {PbrLobe::HEX, "rotation", PbrFieldType::Float, PBR_REF(hex, Hex, rotation, float), 0, 1, "Rotation", "hexTilingRotation", PbrMap::COUNT},
        {PbrLobe::HEX, "contrast", PbrFieldType::Float, PBR_REF(hex, Hex, contrast, float), 0, 1, "Contrast", "hexTilingContrast", PbrMap::COUNT},

        // the Hair shading model (Material::shadingModel in the editor, MCP and Lua: its enabled flag)
        {PbrLobe::HAIR, "enabled", PbrFieldType::Bool, PBR_REF(hair, Hair, enabled, bool), 0, 1, "Hair", nullptr, PbrMap::COUNT},
        {PbrLobe::HAIR, "primaryColor", PbrFieldType::Color, PBR_REF(hair, Hair, primaryColor, Vector3), 0, INF, "Primary Color", "hairPrimaryColor", PbrMap::COUNT},
        {PbrLobe::HAIR, "primaryShift", PbrFieldType::Float, PBR_REF(hair, Hair, primaryShift, float), -1, 1, "Primary Shift", "hairPrimaryShift", PbrMap::COUNT},
        {PbrLobe::HAIR, "primaryWidth", PbrFieldType::Float, PBR_REF(hair, Hair, primaryWidth, float), 0.01f, 1, "Primary Width", "hairPrimaryWidth", PbrMap::COUNT},
        {PbrLobe::HAIR, "secondaryColor", PbrFieldType::Color, PBR_REF(hair, Hair, secondaryColor, Vector3), 0, INF, "Secondary Color", "hairSecondaryColor", PbrMap::COUNT},
        {PbrLobe::HAIR, "secondaryShift", PbrFieldType::Float, PBR_REF(hair, Hair, secondaryShift, float), -1, 1, "Secondary Shift", "hairSecondaryShift", PbrMap::COUNT},
        {PbrLobe::HAIR, "secondaryWidth", PbrFieldType::Float, PBR_REF(hair, Hair, secondaryWidth, float), 0.01f, 1, "Secondary Width", "hairSecondaryWidth", PbrMap::COUNT},
        {PbrLobe::HAIR, "rotation", PbrFieldType::Float, PBR_REF(hair, Hair, rotation, float), -INF, INF, "Strand Rotation", "hairRotation", PbrMap::COUNT},
        {PbrLobe::HAIR, "flowMap", PbrFieldType::Texture, PBR_REF(hair, Hair, flowMap, Texture), 0, 0, "Flow Map", nullptr, PbrMap::HAIR_FLOW},
        {PbrLobe::HAIR, "flowMapTexCoord", PbrFieldType::TexCoord, PBR_REF(hair, Hair, flowMapTexCoord, int), 0, 1, "Flow Map UV Set", nullptr, PbrMap::HAIR_FLOW},
    };
    return fields;
}

const PbrField* MaterialPbr::findField(PbrLobe lobe, const std::string& name){
    for (const PbrField& field : getFields()){
        if (field.lobe == lobe && name == field.name)
            return &field;
    }
    return nullptr;
}

const PbrField* MaterialPbr::findField(const std::string& path){
    const size_t dot = path.find('.');
    if (dot == std::string::npos)
        return nullptr;
    const PbrLobeInfo* lobe = findLobe(path.substr(0, dot));
    return lobe ? findField(lobe->lobe, path.substr(dot + 1)) : nullptr;
}

const PbrField* MaterialPbr::findLuaField(const std::string& luaName){
    for (const PbrField& field : getFields()){
        if (field.luaName && luaName == field.luaName)
            return &field;
    }
    return nullptr;
}

std::string MaterialPbr::getFieldLabel(const PbrField& field){
    if (field.type == PbrFieldType::Bool)
        return field.label;
    return std::string(getLobe(field.lobe).label) + " " + field.label;
}

std::string MaterialPbr::getFieldPath(const PbrField& field){
    return std::string(getLobe(field.lobe).name) + "." + field.name;
}

bool MaterialPbr::isInRange(const PbrField& field, float value){
    if (std::isnan(value))
        return false;
    if (field.type == PbrFieldType::TexCoord)
        return value == 0.0f || value == 1.0f;
    if (field.lobe == PbrLobe::SPECULAR && std::string(field.name) == "ior" && value == 0.0f)
        return true;
    // only a range open on that side takes an infinity
    if (std::isinf(value))
        return value > 0 ? std::isinf(field.maxValue) : std::isinf(field.minValue);
    return value >= field.minValue && value <= field.maxValue;
}

bool MaterialPbr::clamp(MaterialPbrExtensions& pbr){
    static const MaterialPbrExtensions defaults;
    bool clamped = false;
    auto clampValue = [&clamped](const PbrField& field, float& value, float fallback){
        if (std::isnan(value) || (std::isinf(value) && !isInRange(field, value))){
            value = fallback;
            clamped = true;
            return;
        }
        if (isInRange(field, value))
            return;
        // the IOR: (0, 1) is no index of refraction, 0 is glTF's own value
        value = std::clamp(value, field.minValue, field.maxValue);
        clamped = true;
    };
    for (const PbrField& field : getFields()){
        void* ref = field.ref(pbr);
        void* def = field.ref(const_cast<MaterialPbrExtensions&>(defaults));
        switch (field.type){
            case PbrFieldType::Float:
                clampValue(field, *(float*)ref, *(float*)def);
                break;
            case PbrFieldType::Vector2:
                clampValue(field, ((Vector2*)ref)->x, ((Vector2*)def)->x);
                clampValue(field, ((Vector2*)ref)->y, ((Vector2*)def)->y);
                break;
            case PbrFieldType::Color:
                clampValue(field, ((Vector3*)ref)->x, ((Vector3*)def)->x);
                clampValue(field, ((Vector3*)ref)->y, ((Vector3*)def)->y);
                clampValue(field, ((Vector3*)ref)->z, ((Vector3*)def)->z);
                break;
            case PbrFieldType::TexCoord:{
                int& texCoord = *(int*)ref;
                if (texCoord != 0 && texCoord != 1){
                    texCoord = texCoord > 1 ? 1 : 0;
                    clamped = true;
                }
                break;
            }
            default:
                break;
        }
    }
    return clamped;
}

const char* MaterialPbr::getMapName(PbrMap map){
    switch (map){
        case PbrMap::CLEARCOAT:        return "clearcoat";
        case PbrMap::SPECULAR:         return "specular";
        case PbrMap::TRANSMISSION:     return "transmission";
        case PbrMap::SHEEN:            return "sheen";
        case PbrMap::ANISOTROPY:       return "anisotropy";
        case PbrMap::IRIDESCENCE:      return "iridescence";
        case PbrMap::CLEARCOAT_NORMAL: return "clearcoatNormal";
        case PbrMap::HAIR_FLOW:        return "hairFlow";
        default:                       return "";
    }
}

const char* MaterialPbr::getMapDefine(PbrMap map){
    switch (map){
        case PbrMap::CLEARCOAT:        return "PBR_CLEARCOAT_MAP";
        case PbrMap::SPECULAR:         return "PBR_SPECULAR_MAP";
        case PbrMap::TRANSMISSION:     return "PBR_TRANSMISSION_MAP";
        case PbrMap::SHEEN:            return "PBR_SHEEN_MAP";
        case PbrMap::ANISOTROPY:       return "PBR_ANISOTROPY_MAP";
        case PbrMap::IRIDESCENCE:      return "PBR_IRIDESCENCE_MAP";
        case PbrMap::CLEARCOAT_NORMAL: return "PBR_CLEARCOAT_NORMAL_MAP";
        case PbrMap::HAIR_FLOW:        return "PBR_HAIR_MAP";
        default:                       return "";
    }
}

PbrLobe MaterialPbr::getMapLobe(PbrMap map){
    switch (map){
        case PbrMap::SPECULAR:     return PbrLobe::SPECULAR;
        case PbrMap::TRANSMISSION: return PbrLobe::TRANSMISSION;
        case PbrMap::SHEEN:        return PbrLobe::SHEEN;
        case PbrMap::ANISOTROPY:   return PbrLobe::ANISOTROPY;
        case PbrMap::IRIDESCENCE:  return PbrLobe::IRIDESCENCE;
        case PbrMap::HAIR_FLOW:    return PbrLobe::HAIR;
        default:                   return PbrLobe::CLEARCOAT;
    }
}

Texture& MaterialPbr::getMap(MaterialPbrExtensions& pbr, PbrMap map){
    switch (map){
        case PbrMap::SPECULAR:         return pbr.specular.map;
        case PbrMap::TRANSMISSION:     return pbr.transmission.map;
        case PbrMap::SHEEN:            return pbr.sheen.map;
        case PbrMap::ANISOTROPY:       return pbr.anisotropy.map;
        case PbrMap::IRIDESCENCE:      return pbr.iridescence.map;
        case PbrMap::CLEARCOAT_NORMAL: return pbr.clearcoat.normalMap;
        case PbrMap::HAIR_FLOW:        return pbr.hair.flowMap;
        default:                       return pbr.clearcoat.map;
    }
}

const Texture& MaterialPbr::getMap(const MaterialPbrExtensions& pbr, PbrMap map){
    return getMap(const_cast<MaterialPbrExtensions&>(pbr), map);
}

int MaterialPbr::getMapTexCoord(const MaterialPbrExtensions& pbr, PbrMap map){
    switch (map){
        case PbrMap::SPECULAR:         return pbr.specular.mapTexCoord;
        case PbrMap::TRANSMISSION:     return pbr.transmission.mapTexCoord;
        case PbrMap::SHEEN:            return pbr.sheen.mapTexCoord;
        case PbrMap::ANISOTROPY:       return pbr.anisotropy.mapTexCoord;
        case PbrMap::IRIDESCENCE:      return pbr.iridescence.mapTexCoord;
        case PbrMap::CLEARCOAT_NORMAL: return pbr.clearcoat.normalTexCoord;
        case PbrMap::HAIR_FLOW:        return pbr.hair.flowMapTexCoord;
        default:                       return pbr.clearcoat.mapTexCoord;
    }
}

bool MaterialPbr::isColorMap(PbrMap map){
    return map == PbrMap::SHEEN || map == PbrMap::SPECULAR;
}

bool MaterialPbr::isMapStripped(PbrMap map, PbrTier tier){
    return tier == PbrTier::REDUCED && (map == PbrMap::ANISOTROPY || map == PbrMap::CLEARCOAT_NORMAL || map == PbrMap::IRIDESCENCE);
}

PbrVariant MaterialPbr::getPbrVariant(const Material& material, MaterialLighting resolved, bool terrain, PbrTier tier, int reservedMaps){
    PbrVariant variant;
    // a Terrain's surface is its layers'; its hex tiling is theirs (TERRAIN_HEX_TILING)
    if (terrain)
        return variant;

    const MaterialPbrExtensions& pbr = material.pbr;
    // the lobes are lighting: vertex lighting and none have none of them
    const bool pixelLit = resolved == MaterialLighting::PIXEL || resolved == MaterialLighting::MIXED || resolved == MaterialLighting::AUTO;
    const bool reduced = tier == PbrTier::REDUCED;

    bool lobes[(size_t)PbrLobe::COUNT] = {};
    if (pixelLit){
        lobes[(size_t)PbrLobe::CLEARCOAT] = pbr.clearcoat.enabled;
        lobes[(size_t)PbrLobe::SHEEN] = pbr.sheen.enabled;
        lobes[(size_t)PbrLobe::ANISOTROPY] = pbr.anisotropy.enabled;
        lobes[(size_t)PbrLobe::IRIDESCENCE] = pbr.iridescence.enabled && !reduced;
        lobes[(size_t)PbrLobe::TRANSMISSION] = pbr.transmission.enabled;
        lobes[(size_t)PbrLobe::VOLUME] = pbr.volume.enabled && pbr.transmission.enabled;
        lobes[(size_t)PbrLobe::SPECULAR] = pbr.specular.enabled;
        lobes[(size_t)PbrLobe::HAIR] = pbr.hair.enabled;
    }
    lobes[(size_t)PbrLobe::HEX] = pbr.hex.enabled;

    for (const PbrLobeInfo& info : getLobes()){
        if (lobes[(size_t)info.lobe])
            variant.defines.push_back(info.define);
    }
    if (pixelLit && pbr.iridescence.enabled && reduced)
        variant.strippedMaps.push_back("iridescence lobe");

    // the maps of the lobes it has, in priority order, the first MAX_EXTENSION_MAPS bound
    const int budget = std::max(MAX_EXTENSION_MAPS - reservedMaps, 0);
    for (int m = 0; m < (int)PbrMap::COUNT; m++){
        const PbrMap map = (PbrMap)m;
        if (!lobes[(size_t)getMapLobe(map)] || getMap(pbr, map).empty())
            continue;
        if (isMapStripped(map, tier)){
            variant.strippedMaps.push_back(getMapName(map));
            continue;
        }
        if ((int)variant.maps.size() < budget){
            variant.maps.push_back(map);
            variant.defines.push_back(getMapDefine(map));
        }else{
            variant.droppedMaps.push_back(getMapName(map));
        }
    }

    std::sort(variant.defines.begin(), variant.defines.end());
    return variant;
}

PbrTier MaterialPbr::resolveTier(bool mobileTarget, PbrTier mobileTier){
    return mobileTarget ? mobileTier : PbrTier::FULL;
}

void MaterialPbr::setMobileTier(PbrTier tier){
    mobileTierValue = (int)tier;
}

PbrTier MaterialPbr::getMobileTier(){
    return (PbrTier)mobileTierValue.load();
}

PbrTier MaterialPbr::getRuntimeTier(){
    const Platform platform = Engine::getPlatform();
    const bool mobile = platform == Platform::Android || platform == Platform::iOS || platform == Platform::Web;
    return resolveTier(mobile, getMobileTier());
}

void MaterialPbr::getUniformBlock(const MaterialPbrExtensions& pbr, float out[UNIFORM_FLOATS]){
    std::fill(out, out + UNIFORM_FLOATS, 0.0f);

    out[UNIFORM_CLEARCOAT + 0] = pbr.clearcoat.factor;
    out[UNIFORM_CLEARCOAT + 1] = pbr.clearcoat.roughness;
    out[UNIFORM_CLEARCOAT + 2] = pbr.clearcoat.normalScale;

    out[UNIFORM_SHEEN + 0] = pbr.sheen.color.x;
    out[UNIFORM_SHEEN + 1] = pbr.sheen.color.y;
    out[UNIFORM_SHEEN + 2] = pbr.sheen.color.z;
    out[UNIFORM_SHEEN + 3] = pbr.sheen.roughness;

    out[UNIFORM_ANISOTROPY + 0] = pbr.anisotropy.strength;
    out[UNIFORM_ANISOTROPY + 1] = std::cos(pbr.anisotropy.rotation);
    out[UNIFORM_ANISOTROPY + 2] = std::sin(pbr.anisotropy.rotation);

    out[UNIFORM_IRIDESCENCE + 0] = pbr.iridescence.factor;
    out[UNIFORM_IRIDESCENCE + 1] = pbr.iridescence.ior;
    out[UNIFORM_IRIDESCENCE + 2] = pbr.iridescence.thicknessNm.x;
    out[UNIFORM_IRIDESCENCE + 3] = pbr.iridescence.thicknessNm.y;

    out[UNIFORM_TRANSMISSION + 0] = pbr.transmission.factor;
    out[UNIFORM_TRANSMISSION + 1] = pbr.volume.thickness;
    // infinity absorbs nothing; the shader takes 0 as that (no infinity in a uniform on GLES)
    out[UNIFORM_TRANSMISSION + 2] = std::isinf(pbr.volume.attenuationDistance) ? 0.0f : pbr.volume.attenuationDistance;

    out[UNIFORM_ATTENUATION + 0] = pbr.volume.attenuationColor.x;
    out[UNIFORM_ATTENUATION + 1] = pbr.volume.attenuationColor.y;
    out[UNIFORM_ATTENUATION + 2] = pbr.volume.attenuationColor.z;

    out[UNIFORM_SPECULAR + 0] = pbr.specular.color.x;
    out[UNIFORM_SPECULAR + 1] = pbr.specular.color.y;
    out[UNIFORM_SPECULAR + 2] = pbr.specular.color.z;
    out[UNIFORM_SPECULAR + 3] = pbr.specular.factor;

    // the IOR is the Specular & IOR lobe's: without it every dielectric is glTF's 1.5
    out[UNIFORM_IOR_HEX + 0] = pbr.specular.enabled ? pbr.specular.ior : 1.5f;
    out[UNIFORM_IOR_HEX + 1] = pbr.hex.rotation;
    out[UNIFORM_IOR_HEX + 2] = pbr.hex.contrast;

    // The Specular & IOR lobe's dielectric reflectance, f0 = min(((ior - 1) / (ior + 1))^2 *
    // colour, 1) * factor (IOR under 1, glTF's 0: a full mirror), a function of these values
    // alone, so it is made once a draw instead of in every fragment, in the vec4s' free last
    // floats: the IOR's term (attenuation.w) and, for a variant without a specular map,
    // f0 itself (clearcoat.w, anisotropy.w, transmission.w). Its chain of a division and
    // products cost the lobe 5.5 of its 8.5 % on an RTX 3080 Ti (GPU campaign perf3)
    const float ior = out[UNIFORM_IOR_HEX + 0];
    const float ratio = (ior - 1.0f) / (ior + 1.0f);
    const float iorF0 = ior < 1.0f ? 1.0f : ratio * ratio;
    out[UNIFORM_ATTENUATION + 3] = iorF0;
    out[UNIFORM_CLEARCOAT + 3] = std::min(iorF0 * pbr.specular.color.x, 1.0f) * pbr.specular.factor;
    out[UNIFORM_ANISOTROPY + 3] = std::min(iorF0 * pbr.specular.color.y, 1.0f) * pbr.specular.factor;
    out[UNIFORM_TRANSMISSION + 3] = std::min(iorF0 * pbr.specular.color.z, 1.0f) * pbr.specular.factor;

    // The Charlie sheen visibility's L(0.5) at the lobe's roughness (pbrCharlieL, includes/
    // pbr_ext.glsl), for a variant without a sheen map: per fragment it was a third of the
    // lambdas' pow, reciprocal and mixes (GPU campaign perf3)
    const float sheenRoughness = std::clamp(pbr.sheen.roughness, 0.0f, 1.0f);
    const float alphaG = std::max(sheenRoughness * sheenRoughness, 0.000001f);
    const float t = (1.0f - alphaG) * (1.0f - alphaG);
    const auto mix = [t](float x, float y){ return x * (1.0f - t) + y * t; };
    const float a = mix(21.5473f, 25.3245f), b = mix(3.82987f, 3.32435f), c = mix(0.19823f, 0.16801f);
    const float d = mix(-1.97760f, -1.27393f), e = mix(-4.32054f, -4.85967f);
    out[UNIFORM_IOR_HEX + 3] = a / (1.0f + b * std::pow(0.5f, c)) + d * 0.5f + e;
}

ShadingModel MaterialPbr::getShadingModel(const MaterialPbrExtensions& pbr){
    return pbr.hair.enabled ? ShadingModel::HAIR : ShadingModel::STANDARD;
}

void MaterialPbr::setShadingModel(MaterialPbrExtensions& pbr, ShadingModel model){
    pbr.hair.enabled = model == ShadingModel::HAIR;
}

float MaterialPbr::hairExponent(float width){
    const float w = std::clamp(width, 0.01f, 1.0f);
    return std::max(2.0f / (w * w) - 2.0f, 1.0f);
}

void MaterialPbr::getHairUniformBlock(const MaterialPbrExtensions& pbr, float out[HAIR_UNIFORM_FLOATS]){
    const MaterialPbrExtensions::Hair& hair = pbr.hair;
    out[0] = hair.primaryColor.x;
    out[1] = hair.primaryColor.y;
    out[2] = hair.primaryColor.z;
    out[3] = hair.primaryShift;
    out[4] = hair.secondaryColor.x;
    out[5] = hair.secondaryColor.y;
    out[6] = hair.secondaryColor.z;
    out[7] = hair.secondaryShift;
    out[8] = hairExponent(hair.primaryWidth);
    out[9] = hairExponent(hair.secondaryWidth);
    out[10] = std::cos(hair.rotation);
    out[11] = std::sin(hair.rotation);
}

std::string MaterialPbr::getVariantBasename(const std::vector<std::string>& defines){
    std::string basename = "lit_";
    for (const DefineCode& entry : defineCodes){
        if (std::find(defines.begin(), defines.end(), entry.define) != defines.end())
            basename += entry.code;
    }
    return basename;
}

bool MaterialPbr::parseVariantBasename(const std::string& basename, std::vector<std::string>& defines){
    defines.clear();
    if (basename.compare(0, 4, "lit_") != 0 || (basename.size() - 4) % 2 != 0 || basename.size() == 4)
        return false;
    for (size_t i = 4; i + 2 <= basename.size(); i += 2){
        const DefineCode* found = nullptr;
        for (const DefineCode& entry : defineCodes){
            if (basename.compare(i, 2, entry.code) == 0)
                found = &entry;
        }
        if (!found){
            defines.clear();
            return false;
        }
        defines.push_back(found->define);
    }
    std::sort(defines.begin(), defines.end());
    return true;
}

uint64_t MaterialPbr::getDefineBits(const std::vector<std::string>& defines){
    uint64_t bits = 0;
    for (size_t i = 0; i < sizeof(defineCodes) / sizeof(defineCodes[0]); i++){
        if (std::find(defines.begin(), defines.end(), defineCodes[i].define) != defines.end())
            bits |= 1ull << i;
    }
    return bits;
}

uint64_t MaterialPbr::getDefineBit(const std::string& define){
    for (size_t i = 0; i < sizeof(defineCodes) / sizeof(defineCodes[0]); i++){
        if (define == defineCodes[i].define)
            return 1ull << i;
    }
    return 0;
}

std::vector<std::string> MaterialPbr::getDefinesFromBits(uint64_t bits){
    std::vector<std::string> defines;
    for (size_t i = 0; i < sizeof(defineCodes) / sizeof(defineCodes[0]); i++){
        if (bits & (1ull << i))
            defines.push_back(defineCodes[i].define);
    }
    std::sort(defines.begin(), defines.end());
    return defines;
}

bool MaterialPbr::isBuiltinLitDefine(const std::string& define){
    for (const DefineCode& entry : defineCodes){
        if (define == entry.define)
            return true;
    }
    return false;
}
