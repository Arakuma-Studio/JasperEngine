// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "QualityResolver.h"

#include "Engine.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

std::atomic<uint32_t> QualityResolver::revision{1};

namespace {

    struct QualityResolverState {
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

    QualityResolverState& qualityState(){
        static QualityResolverState* s = new QualityResolverState();
        return *s;
    }

    // the stricter of two cells of a field
    std::optional<float> stricterDeviceCell(DeviceOverride field, const std::optional<float>& a, const std::optional<float>& b){
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

    std::vector<QualityResolverState>& stateStack(){
        static std::vector<QualityResolverState>* stack = new std::vector<QualityResolverState>();
        return *stack;
    }

    const std::optional<float>& resolvedCell(DeviceOverride field){
        return qualityState().effective.get(field);
    }

    bool thermalSerious(){
        return qualityState().response >= ThermalState::Serious;
    }

}

void QualityResolver::changed(){
    rebuild();
    revision.fetch_add(1, std::memory_order_acq_rel);
}

void QualityResolver::rebuild(){
    QualityResolverState& s = qualityState();
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
            const std::optional<float> merged = stricterDeviceCell(field, s.effective.get(field), lower.get(field));
            s.fromThermal[f] = merged != s.effective.get(field);
            s.effective.set(field, merged);
        }
    }
    s.active = !s.effective.empty() || s.response >= ThermalState::Serious || s.memoryDrop > 0 ||
               s.settings.lodBias != 1.0f || s.settings.shadowLodBias != 1.0f;
}

void QualityResolver::setSettings(const DeviceSettings& settings){
    if (settings == qualityState().settings)
        return;
    qualityState().settings = settings;
    changed();
}

const DeviceSettings& QualityResolver::getSettings(){
    return qualityState().settings;
}

void QualityResolver::setPlatformGroup(DevicePlatformGroup group){
    if (group == qualityState().group)
        return;
    qualityState().group = group;
    changed();
}

DevicePlatformGroup QualityResolver::getPlatformGroup(){
    return qualityState().group;
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
    if (tier == qualityState().tier)
        return;
    qualityState().tier = tier;
    changed();
}

std::optional<DeviceTier> QualityResolver::getTier(){
    return qualityState().tier;
}

void QualityResolver::setThermalState(ThermalState reported){
    QualityResolverState& s = qualityState();
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
    return qualityState().reported;
}

ThermalState QualityResolver::getThermalResponse(){
    return qualityState().response;
}

double QualityResolver::getRecoverySeconds(){
    const QualityResolverState& s = qualityState();
    return s.reported < s.applied ? std::max(RECOVERY_SECONDS - s.below, 0.0) : 0.0;
}

void QualityResolver::update(double seconds){
    QualityResolverState& s = qualityState();
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
    if (qualityState().memoryDrop >= MAX_MEMORY_DROP)
        return;
    qualityState().memoryDrop++;
    changed();
}

int QualityResolver::getMemoryDrop(){
    return qualityState().memoryDrop;
}

void QualityResolver::resetResponse(){
    QualityResolverState& s = qualityState();
    if (s.reported == ThermalState::Nominal && s.applied == ThermalState::Nominal && s.memoryDrop == 0)
        return;
    s.reported = ThermalState::Nominal;
    s.applied = ThermalState::Nominal;
    s.below = 0.0;
    s.memoryDrop = 0;
    changed();
}

void QualityResolver::pushState(){
    stateStack().push_back(qualityState());
}

void QualityResolver::popState(){
    if (stateStack().empty())
        return;
    qualityState() = stateStack().back();
    stateStack().pop_back();
    changed();
}

void QualityResolver::forceThermalResponse(ThermalState state){
    QualityResolverState& s = qualityState();
    s.reported = state;
    s.applied = state;
    s.below = 0.0;
    changed();
}

void QualityResolver::reset(){
    QualityResolverState& s = qualityState();
    s = QualityResolverState();
    changed();
}

// ---------------------------------------------------------------- the values

float QualityResolver::renderScale(float sceneScale){
    if (!qualityState().active)
        return sceneScale;
    float scale = sceneScale;
    if (const auto& c = resolvedCell(DeviceOverride::RenderScale))
        scale = std::min(scale, *c);
    if (thermalSerious())
        scale *= THERMAL_SCALE;
    return std::max(scale, 0.5f);
}

int QualityResolver::msaaSamples(int sceneSamples){
    const auto& c = resolvedCell(DeviceOverride::MsaaSamples);
    return c ? std::min(sceneSamples, (int)*c) : sceneSamples;
}

ShadowTier QualityResolver::shadowTier(ShadowTier tier){
    const auto& c = resolvedCell(DeviceOverride::ShadowQuality);
    return c ? (ShadowTier)std::min((int)tier, (int)*c) : tier;
}

float QualityResolver::shadowDistance(float metres){
    const auto& c = resolvedCell(DeviceOverride::ShadowDistance);
    return (c && metres > *c) ? *c : metres;
}

float QualityResolver::lodBias(){
    const auto& c = resolvedCell(DeviceOverride::LodBias);
    return qualityState().settings.lodBias * (c ? *c : 1.0f);
}

float QualityResolver::shadowLodBias(){
    const auto& c = resolvedCell(DeviceOverride::ShadowLodBias);
    return qualityState().settings.shadowLodBias * (c ? *c : 1.0f);
}

LitQuality QualityResolver::litQuality(LitQuality quality){
    const auto& c = resolvedCell(DeviceOverride::LitQuality);
    return c ? (LitQuality)std::min((int)quality, (int)*c) : quality;
}

int QualityResolver::textureQuality(int projectQuality){
    if (!qualityState().active)
        return projectQuality;
    int quality = projectQuality;
    if (const auto& c = resolvedCell(DeviceOverride::TextureQuality))
        quality = std::max(quality, (int)*c);
    return std::clamp(quality + qualityState().memoryDrop, 0, 3);
}

bool QualityResolver::bloom(bool enabled){
    return enabled && !resolvedCell(DeviceOverride::Bloom).has_value();
}

bool QualityResolver::ssao(bool enabled){
    return enabled && !resolvedCell(DeviceOverride::SSAO).has_value();
}

AtmosphereQuality QualityResolver::volumetrics(AtmosphereQuality quality){
    return resolvedCell(DeviceOverride::Volumetrics).has_value() ? AtmosphereQuality::OFF : quality;
}

int QualityResolver::targetFrameRate(int fps){
    if (!qualityState().active)
        return fps;
    int rate = fps <= 0 ? -1 : fps;
    if (const auto& c = resolvedCell(DeviceOverride::TargetFrameRate))
        rate = rate <= 0 ? (int)*c : std::min(rate, (int)*c);
    if (thermalSerious() && (rate <= 0 || rate > THERMAL_FRAME_CAP))
        rate = THERMAL_FRAME_CAP;
    return rate;
}

int QualityResolver::maxPixelLights(int lights){
    const auto& c = resolvedCell(DeviceOverride::MaxPixelLights);
    return c ? std::min(lights, (int)*c) : lights;
}

uint32_t QualityResolver::particleBudget(uint32_t budget){
    const auto& c = resolvedCell(DeviceOverride::ParticleBudget);
    return c ? std::min(budget, (uint32_t)*c) : budget;
}

uint64_t QualityResolver::uploadBudgetBytes(){
    const QualityResolverState& s = qualityState();
    return (uint64_t)std::max(s.settings.uploadBudgetMB[(size_t)s.group], 1) * 1024ull * 1024ull;
}

std::vector<AppliedDeviceOverride> QualityResolver::getAppliedOverrides(){
    const QualityResolverState& s = qualityState();
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
