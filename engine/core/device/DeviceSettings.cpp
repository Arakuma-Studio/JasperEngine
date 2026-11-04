// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceSettings.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

using namespace jasperengine;
using nlohmann::json;

namespace {

    const char* const kDeviceTierNames[] = {"low", "medium", "high", "ultra"};
    const char* const kDeviceTierLabels[] = {"Low", "Medium", "High", "Ultra"};
    const char* const kDeviceThermalNames[] = {"nominal", "fair", "serious", "critical"};
    const char* const kDeviceThermalLabels[] = {"Nominal", "Fair", "Serious", "Critical"};
    const char* const kDeviceGroupNames[] = {"desktop", "mobile", "web"};
    const char* const kDeviceGroupLabels[] = {"Desktop", "Mobile", "Web"};

    const char* const kDeviceShadowNames[] = {"Low", "Medium", "High", "Ultra"};
    const char* const kDeviceLitNames[] = {"Standard", "High"};
    const char* const kDeviceOffNames[] = {"Off"};

    using OverrideKind = DeviceOverrideInfo::Kind;

    // the order of DeviceOverride
    const DeviceOverrideInfo kDeviceOverrides[DEVICE_OVERRIDE_COUNT] = {
        {"render_scale", "Render Scale", "The most this tier renders at (the scene's Render Scale, and dynamic resolution's maximum); 0.5 to 1.",
            OverrideKind::Cap, false, 0.5f, 1.0f, false, nullptr, 0},
        {"msaa_samples", "MSAA", "Multisample anti-aliasing at most (1 off, 2, 4).", OverrideKind::Cap, true, 1.0f, 4.0f, false, nullptr, 0},
        {"shadow_quality", "Shadow Quality", "The shadow tier at most (Low, Medium, High, Ultra).", OverrideKind::Cap, true, 0.0f, 3.0f, true, kDeviceShadowNames, 4},
        {"shadow_distance", "Shadow Distance", "Metres of directional shadows at most.", OverrideKind::Cap, false, 1.0f, 100000.0f, true, nullptr, 0},
        {"lod_bias", "LOD Bias", "Multiplies the project's LOD Bias: below 1 switches to coarser LODs nearer.",
            OverrideKind::Multiplier, false, 0.05f, 8.0f, false, nullptr, 0},
        {"shadow_lod_bias", "Shadow LOD Bias", "Multiplies the project's Shadow LOD Bias (the LODs shadow casters draw).",
            OverrideKind::Multiplier, false, 0.05f, 8.0f, true, nullptr, 0},
        {"lit_quality", "Lit Quality", "The Lit shader's quality at most (Standard, High).", OverrideKind::Cap, true, 0.0f, 1.0f, false, kDeviceLitNames, 2},
        {"texture_quality", "Texture Quality", "Top mips of streamable textures dropped, at least (0 full .. 3 an eighth).",
            OverrideKind::AtLeast, true, 0.0f, 3.0f, false, nullptr, 0},
        {"bloom", "Bloom", "Off turns bloom off on this tier.", OverrideKind::Off, true, 0.0f, 0.0f, true, kDeviceOffNames, 1},
        {"ssao", "SSAO", "Off turns screen-space ambient occlusion off on this tier.", OverrideKind::Off, true, 0.0f, 0.0f, true, kDeviceOffNames, 1},
        {"volumetrics", "Volumetrics", "Off turns volumetric fog and clouds off on this tier.", OverrideKind::Off, true, 0.0f, 0.0f, true, kDeviceOffNames, 1},
        {"target_frame_rate", "Target Frame Rate", "Frames per second at most (the platform's default rate counts as above it).",
            OverrideKind::Cap, true, 1.0f, 1000.0f, false, nullptr, 0},
        {"max_pixel_lights", "Max Pixel Lights", "Lights per object at most.", OverrideKind::Cap, true, 1.0f, 64.0f, false, nullptr, 0},
        {"particle_budget", "Particle Budget", "Live particles of a scene at most.", OverrideKind::Cap, true, 1.0f, 4194304.0f, false, nullptr, 0},
    };

    template<size_t N>
    bool deviceNameIndex(const char* const (&names)[N], const char* name, int& index){
        if (!name)
            return false;
        for (size_t i = 0; i < N; i++){
            if (std::strcmp(names[i], name) == 0){
                index = (int)i;
                return true;
            }
        }
        return false;
    }

    std::string deviceLowerCase(std::string text){
        for (char& c : text)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return text;
    }

}

// ---------------------------------------------------------------- names

const char* DeviceNames::tier(DeviceTier tier){
    return kDeviceTierNames[std::clamp((int)tier, 0, DEVICE_TIER_COUNT - 1)];
}

bool DeviceNames::tierFromName(const char* name, DeviceTier& tier){
    int index = 0;
    if (!name || !deviceNameIndex(kDeviceTierNames, deviceLowerCase(name).c_str(), index))
        return false;
    tier = (DeviceTier)index;
    return true;
}

const char* DeviceNames::tierLabel(DeviceTier tier){
    return kDeviceTierLabels[std::clamp((int)tier, 0, DEVICE_TIER_COUNT - 1)];
}

const char* DeviceNames::thermal(ThermalState state){
    return kDeviceThermalNames[std::clamp((int)state, 0, THERMAL_STATE_COUNT - 1)];
}

bool DeviceNames::thermalFromName(const char* name, ThermalState& state){
    int index = 0;
    if (!name || !deviceNameIndex(kDeviceThermalNames, deviceLowerCase(name).c_str(), index))
        return false;
    state = (ThermalState)index;
    return true;
}

const char* DeviceNames::thermalLabel(ThermalState state){
    return kDeviceThermalLabels[std::clamp((int)state, 0, THERMAL_STATE_COUNT - 1)];
}

const char* DeviceNames::group(DevicePlatformGroup group){
    return kDeviceGroupNames[std::clamp((int)group, 0, DEVICE_PLATFORM_GROUP_COUNT - 1)];
}

bool DeviceNames::groupFromName(const char* name, DevicePlatformGroup& group){
    int index = 0;
    if (!name || !deviceNameIndex(kDeviceGroupNames, deviceLowerCase(name).c_str(), index))
        return false;
    group = (DevicePlatformGroup)index;
    return true;
}

const char* DeviceNames::groupLabel(DevicePlatformGroup group){
    return kDeviceGroupLabels[std::clamp((int)group, 0, DEVICE_PLATFORM_GROUP_COUNT - 1)];
}

// ---------------------------------------------------------------- the cells

const DeviceOverrideInfo& DeviceOverrides::info(DeviceOverride field){
    return kDeviceOverrides[std::clamp((int)field, 0, DEVICE_OVERRIDE_COUNT - 1)];
}

bool DeviceOverrides::fromKey(const std::string& key, DeviceOverride& field){
    for (int i = 0; i < DEVICE_OVERRIDE_COUNT; i++){
        if (key == kDeviceOverrides[i].key){
            field = (DeviceOverride)i;
            return true;
        }
    }
    return false;
}

std::string DeviceOverrides::format(DeviceOverride field, float value){
    const DeviceOverrideInfo& item = info(field);
    if (item.names){
        const int index = item.kind == OverrideKind::Off ? 0 : std::clamp((int)std::lround(value), 0, item.nameCount - 1);
        return item.names[index];
    }
    char text[32];
    if (item.integer)
        std::snprintf(text, sizeof(text), "%d", (int)std::lround(value));
    else
        std::snprintf(text, sizeof(text), "%g", value);
    return text;
}

bool DeviceOverrides::validate(DeviceOverride field, float value, std::string& error){
    const DeviceOverrideInfo& item = info(field);
    if (!std::isfinite(value)){
        error = std::string(item.key) + " must be a finite number";
        return false;
    }
    if (item.kind == OverrideKind::Off){
        if (value != 0.0f){
            error = std::string(item.key) + ": a tier only turns it off (false or \"off\"); null inherits";
            return false;
        }
        return true;
    }
    if (item.integer && value != std::floor(value)){
        error = std::string(item.key) + " must be a whole number";
        return false;
    }
    if (value < item.min || value > item.max){
        char range[96];
        std::snprintf(range, sizeof(range), " must be between %g and %g", item.min, item.max);
        error = std::string(item.key) + range;
        return false;
    }
    if (field == DeviceOverride::MsaaSamples && value != 1.0f && value != 2.0f && value != 4.0f){
        error = "msaa_samples must be 1, 2 or 4";
        return false;
    }
    return true;
}

bool DeviceOverrides::parse(DeviceOverride field, const std::string& text, float& value, std::string& error){
    const DeviceOverrideInfo& item = info(field);
    const std::string lower = deviceLowerCase(text);
    if (item.names){
        for (int i = 0; i < item.nameCount; i++){
            if (lower == deviceLowerCase(item.names[i])){
                value = item.kind == OverrideKind::Off ? 0.0f : (float)i;
                return true;
            }
        }
        if (item.kind == OverrideKind::Off && (lower == "false" || lower == "0")){
            value = 0.0f;
            return true;
        }
    }
    char* end = nullptr;
    const double number = std::strtod(text.c_str(), &end);
    if (text.empty() || !end || *end != '\0'){
        std::string names;
        for (int i = 0; i < item.nameCount; i++)
            names += std::string(i ? ", " : "") + item.names[i];
        error = std::string(item.key) + ": \"" + text + "\" is not " + (item.names ? "one of " + names : std::string("a number"));
        return false;
    }
    value = (float)number;
    return validate(field, value, error);
}

bool DeviceTierOverrides::empty() const{
    for (const auto& cell : cells)
        if (cell.has_value()) return false;
    return true;
}

// ---------------------------------------------------------------- the settings

DeviceSettings::DeviceSettings(){
    for (size_t group = 0; group < tiers.size(); group++){
        DeviceTierOverrides& low = tiers[group][(size_t)DeviceTier::Low];
        low.set(DeviceOverride::RenderScale, 0.5f);
        low.set(DeviceOverride::ShadowQuality, 0.0f);
        low.set(DeviceOverride::LodBias, 0.5f);
        low.set(DeviceOverride::Volumetrics, 0.0f);
        low.set(DeviceOverride::SSAO, 0.0f);
        tiers[group][(size_t)DeviceTier::Medium].set(DeviceOverride::RenderScale, 0.85f);
    }
}

bool DeviceSettings::operator==(const DeviceSettings& other) const{
    return tiers == other.tiers && lodBias == other.lodBias && shadowLodBias == other.shadowLodBias &&
           thermalResponse == other.thermalResponse && uploadBudgetMB == other.uploadBudgetMB;
}

std::string DeviceSettings::toJson() const{
    const DeviceSettings defaults;
    json out = json::object();
    json tierTable = json::object();
    for (int g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++){
        json groupJson = json::object();
        for (int t = 0; t < DEVICE_TIER_COUNT; t++){
            json tierJson = json::object();
            for (int f = 0; f < DEVICE_OVERRIDE_COUNT; f++){
                const std::optional<float>& cell = tiers[g][t].cells[f];
                if (cell == defaults.tiers[g][t].cells[f])
                    continue;
                tierJson[kDeviceOverrides[f].key] = cell.has_value() ? json(*cell) : json(nullptr);
            }
            if (!tierJson.empty())
                groupJson[kDeviceTierNames[t]] = tierJson;
        }
        if (!groupJson.empty())
            tierTable[kDeviceGroupNames[g]] = groupJson;
    }
    if (!tierTable.empty())
        out["tiers"] = tierTable;
    if (lodBias != defaults.lodBias)
        out["lod_bias"] = lodBias;
    if (shadowLodBias != defaults.shadowLodBias)
        out["shadow_lod_bias"] = shadowLodBias;
    for (int g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++){
        if (thermalResponse[g] != defaults.thermalResponse[g])
            out["thermal_response"][kDeviceGroupNames[g]] = thermalResponse[g];
        if (uploadBudgetMB[g] != defaults.uploadBudgetMB[g])
            out["upload_budget_mb"][kDeviceGroupNames[g]] = uploadBudgetMB[g];
    }
    return out.dump();
}

bool DeviceSettings::fromJson(const std::string& text, DeviceSettings& out, std::string& error){
    out = DeviceSettings();
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()){
        error = "the device settings are not a JSON object";
        return false;
    }
    auto number = [&](const json& value, const char* what, float& target) -> bool {
        if (!value.is_number()){
            error = std::string(what) + " must be a number";
            return false;
        }
        target = value.get<float>();
        return true;
    };
    if (auto it = parsed.find("tiers"); it != parsed.end() && it->is_object()){
        for (auto& [groupName, groupJson] : it->items()){
            DevicePlatformGroup group;
            if (!DeviceNames::groupFromName(groupName.c_str(), group) || !groupJson.is_object()){
                error = "tiers." + groupName + ": not a platform group (desktop, mobile, web)";
                return false;
            }
            for (auto& [tierName, tierJson] : groupJson.items()){
                DeviceTier tier;
                if (!DeviceNames::tierFromName(tierName.c_str(), tier) || !tierJson.is_object()){
                    error = "tiers." + groupName + "." + tierName + ": not a tier (low, medium, high, ultra)";
                    return false;
                }
                for (auto& [key, value] : tierJson.items()){
                    DeviceOverride field;
                    if (!DeviceOverrides::fromKey(key, field)){
                        // a newer player's cell: ignored, as any unknown key
                        continue;
                    }
                    if (value.is_null()){
                        out.tier(group, tier).set(field, std::nullopt);
                        continue;
                    }
                    float cell = 0.0f;
                    if (value.is_boolean() && DeviceOverrides::info(field).kind == DeviceOverrideInfo::Kind::Off){
                        if (value.get<bool>()){
                            error = "tiers." + groupName + "." + tierName + "." + key + ": a tier only turns it off";
                            return false;
                        }
                    }else if (!number(value, key.c_str(), cell) || !DeviceOverrides::validate(field, cell, error)){
                        return false;
                    }
                    out.tier(group, tier).set(field, cell);
                }
            }
        }
    }
    using Bias = std::pair<const char*, float*>;
    for (const Bias& bias : {Bias{"lod_bias", &out.lodBias}, Bias{"shadow_lod_bias", &out.shadowLodBias}}){
        const char* key = bias.first;
        float* target = bias.second;
        auto it = parsed.find(key);
        if (it == parsed.end())
            continue;
        if (!number(*it, key, *target))
            return false;
        if (!(*target >= 0.05f && *target <= 8.0f)){
            error = std::string(key) + " must be between 0.05 and 8";
            return false;
        }
    }
    for (int g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++){
        if (auto it = parsed.find("thermal_response"); it != parsed.end() && it->is_object() && it->contains(kDeviceGroupNames[g])){
            const json& value = (*it)[kDeviceGroupNames[g]];
            if (!value.is_boolean()){
                error = std::string("thermal_response.") + kDeviceGroupNames[g] + " must be true or false";
                return false;
            }
            out.thermalResponse[g] = value.get<bool>();
        }
        if (auto it = parsed.find("upload_budget_mb"); it != parsed.end() && it->is_object() && it->contains(kDeviceGroupNames[g])){
            const json& value = (*it)[kDeviceGroupNames[g]];
            if (!value.is_number_integer() || value.get<int>() < MIN_UPLOAD_BUDGET_MB || value.get<int>() > MAX_UPLOAD_BUDGET_MB){
                error = std::string("upload_budget_mb.") + kDeviceGroupNames[g] + " must be a whole number of MB from 1 to 1024";
                return false;
            }
            out.uploadBudgetMB[g] = value.get<int>();
        }
    }
    return true;
}
