// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "QualityResolver.h"

#include "Engine.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

std::atomic<uint32_t> QualityResolver::revision{1};

namespace {

    struct ResolverState {
        DeviceSettings settings;
        DevicePlatformGroup group = QualityResolver::platformGroupOf((int)Engine::getPlatform());
        std::optional<DeviceTier> tier;
        ThermalState reported = ThermalState::Nominal;
        ThermalState applied = ThermalState::Nominal;
        // seconds the reported state has stayed below the applied one
        double below = 0.0;
        int memoryDrop = 0;

        // what the getters read, rebuilt on every change
        DeviceTierOverrides effective;
        // the cells a Critical step took from the tier below
        std::array<bool, DEVICE_OVERRIDE_COUNT> fromThermal = {};
        ThermalState response = ThermalState::Nominal;
        bool active = false;
    };

    ResolverState& state(){
        static ResolverState* s = new ResolverState();
        return *s;
    }

    // the stricter of two cells of a field
    std::optional<float> stricter(DeviceOverride field, const std::optional<float>& a, const std::optional<float>& b){
        if (!a) return b;
        if (!b) return a;
        switch (DeviceOverrides::info(field).kind){
            case DeviceOverrideInfo::Kind::AtLeast: return std::max(*a, *b);
            case DeviceOverrideInfo::Kind::Multiplier: return std::min(*a, *b);
            case DeviceOverrideInfo::Kind::Off: return 0.0f;
            case DeviceOverrideInfo::Kind::Cap: return std::min(*a, *b);
        }
        return a;
    }

    const std::optional<float>& cell(DeviceOverride field){
        return state().effective.get(field);
    }

    bool serious(){
        return state().response >= ThermalState::Serious;
    }

}

void QualityResolver::changed(){
    rebuild();
    revision.fetch_add(1, std::memory_order_acq_rel);
}

void QualityResolver::rebuild(){
    ResolverState& s = state();
    s.response = s.settings.thermalResponse[(size_t)s.group] ? s.applied : ThermalState::Nominal;
    s.effective = s.tier ? s.settings.tier(s.group, *s.tier) : DeviceTierOverrides();
    s.fromThermal.fill(false);
    if (s.response == ThermalState::Critical){
        // the next tier down's shadows and post-processing (the project's values count as Ultra)
        const DeviceTier down = s.tier ? (DeviceTier)std::max((int)*s.tier - 1, 0) : DeviceTier::High;
        const DeviceTierOverrides& lower = s.settings.tier(s.group, down);
        for (int f = 0; f < DEVICE_OVERRIDE_COUNT; f++){
            const DeviceOverride field = (DeviceOverride)f;
            if (!DeviceOverrides::info(field).thermalCritical)
                continue;
            const std::optional<float> merged = stricter(field, s.effective.get(field), lower.get(field));
            s.fromThermal[f] = merged != s.effective.get(field);
            s.effective.set(field, merged);
        }
    }
    s.active = !s.effective.empty() || s.response >= ThermalState::Serious || s.memoryDrop > 0 ||
               s.settings.lodBias != 1.0f || s.settings.shadowLodBias != 1.0f;
}

void QualityResolver::setSettings(const DeviceSettings& settings){
    if (settings == state().settings)
        return;
    state().settings = settings;
    changed();
}

const DeviceSettings& QualityResolver::getSettings(){
    return state().settings;
}

void QualityResolver::setPlatformGroup(DevicePlatformGroup group){
    if (group == state().group)
        return;
    state().group = group;
    changed();
}

DevicePlatformGroup QualityResolver::getPlatformGroup(){
    return state().group;
}

DevicePlatformGroup QualityResolver::platformGroupOf(int enginePlatform){
    switch ((Platform)enginePlatform){
        case Platform::iOS:
        case Platform::Android:
            return DevicePlatformGroup::Mobile;
        case Platform::Web:
            return DevicePlatformGroup::Web;
        default:
            return DevicePlatformGroup::Desktop;
    }
}

void QualityResolver::setTier(std::optional<DeviceTier> tier){
    if (tier == state().tier)
        return;
    state().tier = tier;
    changed();
}

std::optional<DeviceTier> QualityResolver::getTier(){
    return state().tier;
}

void QualityResolver::setThermalState(ThermalState reported){
    ResolverState& s = state();
    s.reported = reported;
    if (reported > s.applied){
        s.applied = reported;
        s.below = 0.0;
        changed();
    }else if (reported == s.applied){
        s.below = 0.0;
    }
}

ThermalState QualityResolver::getReportedThermalState(){
    return state().reported;
}

ThermalState QualityResolver::getThermalResponse(){
    return state().response;
}

double QualityResolver::getRecoverySeconds(){
    const ResolverState& s = state();
    return s.reported < s.applied ? std::max(RECOVERY_SECONDS - s.below, 0.0) : 0.0;
}

void QualityResolver::update(double seconds){
    ResolverState& s = state();
    if (s.reported >= s.applied || !(seconds > 0.0))
        return;
    s.below += seconds;
    if (s.below >= RECOVERY_SECONDS){
        // one state down, and another wait for the next
        s.applied = (ThermalState)((int)s.applied - 1);
        s.below = 0.0;
        changed();
    }
}

void QualityResolver::addMemoryPressure(){
    if (state().memoryDrop >= MAX_MEMORY_DROP)
        return;
    state().memoryDrop++;
    changed();
}

int QualityResolver::getMemoryDrop(){
    return state().memoryDrop;
}

void QualityResolver::resetResponse(){
    ResolverState& s = state();
    if (s.reported == ThermalState::Nominal && s.applied == ThermalState::Nominal && s.memoryDrop == 0)
        return;
    s.reported = ThermalState::Nominal;
    s.applied = ThermalState::Nominal;
    s.below = 0.0;
    s.memoryDrop = 0;
    changed();
}

void QualityResolver::reset(){
    ResolverState& s = state();
    s = ResolverState();
    changed();
}

// ---------------------------------------------------------------- the values

float QualityResolver::renderScale(float sceneScale){
    if (!state().active)
        return sceneScale;
    float scale = sceneScale;
    if (const auto& c = cell(DeviceOverride::RenderScale))
        scale = std::min(scale, *c);
    if (serious())
        scale *= THERMAL_SCALE;
    return std::max(scale, 0.5f);
}

int QualityResolver::msaaSamples(int sceneSamples){
    const auto& c = cell(DeviceOverride::MsaaSamples);
    return c ? std::min(sceneSamples, (int)*c) : sceneSamples;
}

ShadowTier QualityResolver::shadowTier(ShadowTier tier){
    const auto& c = cell(DeviceOverride::ShadowQuality);
    return c ? (ShadowTier)std::min((int)tier, (int)*c) : tier;
}

float QualityResolver::shadowDistance(float metres){
    const auto& c = cell(DeviceOverride::ShadowDistance);
    return (c && metres > *c) ? *c : metres;
}

float QualityResolver::lodBias(){
    const auto& c = cell(DeviceOverride::LodBias);
    return state().settings.lodBias * (c ? *c : 1.0f);
}

float QualityResolver::shadowLodBias(){
    const auto& c = cell(DeviceOverride::ShadowLodBias);
    return state().settings.shadowLodBias * (c ? *c : 1.0f);
}

LitQuality QualityResolver::litQuality(LitQuality quality){
    const auto& c = cell(DeviceOverride::LitQuality);
    return c ? (LitQuality)std::min((int)quality, (int)*c) : quality;
}

int QualityResolver::textureQuality(int projectQuality){
    if (!state().active)
        return projectQuality;
    int quality = projectQuality;
    if (const auto& c = cell(DeviceOverride::TextureQuality))
        quality = std::max(quality, (int)*c);
    return std::clamp(quality + state().memoryDrop, 0, 3);
}

bool QualityResolver::bloom(bool enabled){
    return enabled && !cell(DeviceOverride::Bloom).has_value();
}

bool QualityResolver::ssao(bool enabled){
    return enabled && !cell(DeviceOverride::SSAO).has_value();
}

AtmosphereQuality QualityResolver::volumetrics(AtmosphereQuality quality){
    return cell(DeviceOverride::Volumetrics).has_value() ? AtmosphereQuality::OFF : quality;
}

int QualityResolver::targetFrameRate(int fps){
    if (!state().active)
        return fps;
    int rate = fps <= 0 ? -1 : fps;
    if (const auto& c = cell(DeviceOverride::TargetFrameRate))
        rate = rate <= 0 ? (int)*c : std::min(rate, (int)*c);
    if (serious() && (rate <= 0 || rate > THERMAL_FRAME_CAP))
        rate = THERMAL_FRAME_CAP;
    return rate;
}

int QualityResolver::maxPixelLights(int lights){
    const auto& c = cell(DeviceOverride::MaxPixelLights);
    return c ? std::min(lights, (int)*c) : lights;
}

uint32_t QualityResolver::particleBudget(uint32_t budget){
    const auto& c = cell(DeviceOverride::ParticleBudget);
    return c ? std::min(budget, (uint32_t)*c) : budget;
}

uint64_t QualityResolver::uploadBudgetBytes(){
    const ResolverState& s = state();
    return (uint64_t)std::max(s.settings.uploadBudgetMB[(size_t)s.group], 1) * 1024ull * 1024ull;
}

std::vector<AppliedDeviceOverride> QualityResolver::getAppliedOverrides(){
    const ResolverState& s = state();
    std::vector<AppliedDeviceOverride> applied;
    for (int f = 0; f < DEVICE_OVERRIDE_COUNT; f++){
        const auto& value = s.effective.cells[f];
        if (value)
            applied.push_back({(DeviceOverride)f, *value, s.fromThermal[f] ? "thermal" : "tier"});
    }
    if (s.response >= ThermalState::Serious){
        applied.push_back({DeviceOverride::RenderScale, THERMAL_SCALE, "thermal"});
        applied.push_back({DeviceOverride::TargetFrameRate, (float)THERMAL_FRAME_CAP, "thermal"});
    }
    if (s.memoryDrop > 0)
        applied.push_back({DeviceOverride::TextureQuality, (float)s.memoryDrop, "memory"});
    return applied;
}
