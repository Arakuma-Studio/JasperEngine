// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FogResolve.h"

#include "Engine.h"
#include "Scene.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "component/Transform.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

// ---- FogRuntimeSlot (FogComponent.h) ----

FogRuntimeSlot::FogRuntimeSlot() = default;
FogRuntimeSlot::~FogRuntimeSlot() = default;

FogRuntimeSlot::FogRuntimeSlot(const FogRuntimeSlot&){
}

FogRuntimeSlot& FogRuntimeSlot::operator=(const FogRuntimeSlot&){
    return *this;
}

FogRuntimeSlot::FogRuntimeSlot(FogRuntimeSlot&& other) noexcept: state(std::move(other.state)){
}

FogRuntimeSlot& FogRuntimeSlot::operator=(FogRuntimeSlot&& other) noexcept{
    if (this != &other)
        state = std::move(other.state);
    return *this;
}

FogRuntime& FogRuntimeSlot::ensure(){
    if (!state)
        state = std::make_unique<FogRuntime>();
    return *state;
}

void FogRuntimeSlot::reset(){
    state.reset();
}

namespace jasperengine::fogresolve {
namespace {

    constexpr size_t kFogInlineZones = 64;

    bool fogZoneBefore(const FogZoneSample& a, const FogZoneSample& b){
        if (a.priority != b.priority)
            return a.priority < b.priority;
        return a.entity < b.entity;
    }

    bool fogZoneStateBefore(const FogZoneState& a, const FogZoneState& b){
        if (a.priority != b.priority)
            return a.priority < b.priority;
        return a.entity < b.entity;
    }

    void applyFogZone(const FogZoneSample& zone, FogResolve& out){
        if (!(zone.weight > 0.0f) || !zone.profile)
            return;
        const FogProfileData& profile = *zone.profile;
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            if (!profile.set.test(i))
                continue;
            const FogField field = static_cast<FogField>(i);
            const FogFieldKind kind = FogFields::info(field).kind;
            // an enum or a bool takes the zone's value only from half weight on (R8)
            if ((kind == FogFieldKind::Bool || kind == FogFieldKind::Enum) && zone.weight < 0.5f)
                continue;
            FogFields::blend(out.resolved, profile.values, field, std::min(zone.weight, 1.0f));
            out.source[i] = {FogLayer::Zone, zone.entity};
        }
    }

    // a zone's profile, looked up each resolve (an edit shows on the next frame, R5)
    std::shared_ptr<const FogProfileData> fogZoneProfileOf(const FogZoneComponent& zone){
        if (!zone.profile.isSet())
            return nullptr;
        return FogProfileLibrary::getRef(zone.profile);
    }

    std::string fogRefKey(const AssetRef& ref){
        if (!ref.path.empty())
            return ref.path;
        if (!ref.guid.isNull())
            return ref.guid.str();
        return std::string();
    }

    // the base look the blend layer gives now (own fields, profile, blend), for a new blendTo's start
    void currentFogBase(const FogComponent& fog, FogComponent& base){
        FogResolve resolve;
        const std::shared_ptr<const FogProfileData> profile = FogScene::profileOf(fog);
        const FogRuntime* runtime = fog.runtime.get();
        std::shared_ptr<const FogProfileData> target;
        FogBlendSample blend;
        if (runtime && runtime->blending){
            target = FogProfileLibrary::get(runtime->targetKey);
            blend.from = &runtime->from;
            blend.target = target.get();
            blend.t = runtime->duration > 0.0 ? (float)std::clamp(runtime->elapsed / runtime->duration, 0.0, 1.0) : 1.0f;
        }
        resolveFog(fog, profile.get(), runtime && runtime->blending ? &blend : nullptr, {}, nullptr, resolve);
        base = resolve.resolved;
    }

    void finishFogBlend(FogRuntime& runtime){
        runtime.blending = false;
        runtime.profileOverridden = true;
        runtime.profileKey = runtime.targetKey;
        runtime.targetKey.clear();
        runtime.elapsed = runtime.duration = 0.0;
    }

    // reused by every resolve of the frame: no allocation once warm (R20, R21)
    std::vector<FogZoneSample>& fogZoneScratch(){
        static std::vector<FogZoneSample>* scratch = new std::vector<FogZoneSample>();
        return *scratch;
    }

    std::vector<std::shared_ptr<const FogProfileData>>& fogProfileScratch(){
        static std::vector<std::shared_ptr<const FogProfileData>>* scratch = new std::vector<std::shared_ptr<const FogProfileData>>();
        return *scratch;
    }

}
}


std::string jasperengine::fogLayerSourceName(const FogLayerSource& source){
    switch (source.layer){
        case FogLayer::Fog: return "fog";
        case FogLayer::Profile: return "profile";
        case FogLayer::Blend: return "blend";
        case FogLayer::Zone: return "zone:" + std::to_string(source.zone);
        case FogLayer::Runtime: return "runtime";
    }
    return "fog";
}

void jasperengine::resolveFog(const FogComponent& fog, const FogProfileData* profile, const FogBlendSample* blend,
                              std::span<const FogZoneSample> zones, const FogRuntimeWrites* runtime, FogResolve& out){
    out.resolved = fog;
    out.source.fill(FogLayerSource());

    if (blend && blend->from){
        // the base layer cross-fades from the look blendTo started from to the own fields under
        // the target profile; the linked profile is in `from` (R10)
        const float t = std::clamp(blend->t, 0.0f, 1.0f);
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            const FogField field = static_cast<FogField>(i);
            const bool targetSets = blend->target && blend->target->set.test(i);
            const FogComponent& to = targetSets ? blend->target->values : fog;
            FogFields::copy(out.resolved, *blend->from, field);
            FogFields::blend(out.resolved, to, field, t);
            if (targetSets || !FogFields::equal(*blend->from, to, field))
                out.source[i] = {FogLayer::Blend, NULL_ENTITY};
        }
    }else if (profile){
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            if (!profile->set.test(i))
                continue;
            FogFields::copy(out.resolved, profile->values, static_cast<FogField>(i));
            out.source[i] = {FogLayer::Profile, NULL_ENTITY};
        }
    }

    if (!zones.empty()){
        if (zones.size() <= fogresolve::kFogInlineZones){
            std::array<FogZoneSample, fogresolve::kFogInlineZones> sorted;
            std::copy(zones.begin(), zones.end(), sorted.begin());
            std::sort(sorted.begin(), sorted.begin() + zones.size(), &fogresolve::fogZoneBefore);
            for (size_t z = 0; z < zones.size(); z++)
                fogresolve::applyFogZone(sorted[z], out);
        }else{
            std::vector<FogZoneSample> sorted(zones.begin(), zones.end());
            std::sort(sorted.begin(), sorted.end(), &fogresolve::fogZoneBefore);
            for (const FogZoneSample& zone : sorted)
                fogresolve::applyFogZone(zone, out);
        }
    }

    if (runtime){
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            if (!runtime->set.test(i))
                continue;
            FogFields::copy(out.resolved, runtime->values, static_cast<FogField>(i));
            out.source[i] = {FogLayer::Runtime, NULL_ENTITY};
        }
    }
}

float jasperengine::fogZoneDistance(const FogZoneComponent& zone, const Transform& transform, const Vector3& camera){
    const Vector3 half(std::abs(zone.size.x * transform.worldScale.x) * 0.5f, std::abs(zone.size.y * transform.worldScale.y) * 0.5f,
                       std::abs(zone.size.z * transform.worldScale.z) * 0.5f);
    const Vector3 local = transform.worldRotation.inverse() * (camera - transform.worldPosition);
    if (zone.shape == FogZoneShape::SPHERE){
        // an ellipsoid of those semi-axes; Local Volumetric Fog's measure (volumetric_fog.frag),
        // exact for a sphere
        const float smallest = std::min(half.x, std::min(half.y, half.z));
        if (smallest <= 1e-6f)
            return local.length();
        const Vector3 q(local.x / half.x, local.y / half.y, local.z / half.z);
        return std::max(q.length() - 1.0f, 0.0f) * smallest;
    }
    const Vector3 outside(std::max(std::abs(local.x) - half.x, 0.0f), std::max(std::abs(local.y) - half.y, 0.0f),
                          std::max(std::abs(local.z) - half.z, 0.0f));
    return outside.length();
}

float jasperengine::fogZoneWeight(const FogZoneComponent& zone, float distance){
    const float weight = std::clamp(zone.weight, 0.0f, 1.0f);
    if (distance <= 0.0f)
        return weight;
    // 0: a hard switch at the surface
    if (!(zone.blendDistance > 0.0f))
        return 0.0f;
    return weight * std::clamp(1.0f - distance / zone.blendDistance, 0.0f, 1.0f);
}

// ---- FogScene ----

Entity FogScene::findFog(Scene* scene){
    if (!scene)
        return NULL_ENTITY;
    auto fogs = scene->getComponentArray<FogComponent>();
    return fogs->size() > 0 ? fogs->getEntity(0) : NULL_ENTITY;
}

bool FogScene::needsResolve(Scene* scene, const FogComponent& fog){
    if (fog.profile.isSet())
        return true;
    if (const FogRuntime* runtime = fog.runtime.get(); runtime && !runtime->empty())
        return true;
    return scene && scene->isComponentRegistered<FogZoneComponent>() && scene->getComponentArray<FogZoneComponent>()->size() > 0;
}

std::vector<FogZoneState> FogScene::zoneStates(Scene* scene, const Vector3& camera){
    std::vector<FogZoneState> states;
    if (!scene || !scene->isComponentRegistered<FogZoneComponent>())
        return states;
    auto zones = scene->getComponentArray<FogZoneComponent>();
    for (size_t i = 0; i < zones->size(); i++){
        const Entity entity = zones->getEntity(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !scene->isEntityActiveInHierarchy(entity))
            continue;
        const FogZoneComponent& zone = zones->getComponentFromIndex(i);
        FogZoneState state;
        state.entity = entity;
        state.priority = zone.priority;
        state.distance = fogZoneDistance(zone, *transform, camera);
        state.profileKey = fogresolve::fogRefKey(zone.profile);
        const std::shared_ptr<const FogProfileData> profile = fogresolve::fogZoneProfileOf(zone);
        state.profileMissing = zone.profile.isSet() && !profile;
        state.weight = profile ? fogZoneWeight(zone, state.distance) : 0.0f;
        states.push_back(std::move(state));
    }
    std::sort(states.begin(), states.end(), &fogresolve::fogZoneStateBefore);
    return states;
}

void FogScene::resolve(Scene* scene, const FogComponent& fog, const Vector3& camera, FogResolve& out, std::vector<FogZoneState>* zoneOut){
    const std::shared_ptr<const FogProfileData> profile = profileOf(fog);
    const FogRuntime* runtime = fog.runtime.get();

    std::shared_ptr<const FogProfileData> target;
    FogBlendSample blend;
    const bool blending = runtime && runtime->blending;
    if (blending){
        target = FogProfileLibrary::get(runtime->targetKey);
        blend.from = &runtime->from;
        blend.target = target.get();
        blend.t = runtime->duration > 0.0 ? (float)std::clamp(runtime->elapsed / runtime->duration, 0.0, 1.0) : 1.0f;
    }

    std::vector<FogZoneSample>& samples = fogresolve::fogZoneScratch();
    std::vector<std::shared_ptr<const FogProfileData>>& held = fogresolve::fogProfileScratch();
    samples.clear();
    held.clear();
    if (scene && scene->isComponentRegistered<FogZoneComponent>()){
        auto zones = scene->getComponentArray<FogZoneComponent>();
        for (size_t i = 0; i < zones->size(); i++){
            const Entity entity = zones->getEntity(i);
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform || !scene->isEntityActiveInHierarchy(entity))
                continue;
            const FogZoneComponent& zone = zones->getComponentFromIndex(i);
            std::shared_ptr<const FogProfileData> zoneData = fogresolve::fogZoneProfileOf(zone);
            if (!zoneData)
                continue;
            const float weight = fogZoneWeight(zone, fogZoneDistance(zone, *transform, camera));
            if (!(weight > 0.0f))
                continue;
            samples.push_back({entity, zone.priority, weight, zoneData.get()});
            held.push_back(std::move(zoneData));
        }
    }

    resolveFog(fog, profile.get(), blending ? &blend : nullptr, samples, runtime && runtime->writes.set.any() ? &runtime->writes : nullptr, out);
    held.clear();

    if (zoneOut)
        *zoneOut = zoneStates(scene, camera);
}

Vector3 FogScene::mainCameraPosition(Scene* scene){
    if (!scene)
        return Vector3(0.0f, 0.0f, 0.0f);
    const Entity camera = scene->getCamera();
    const Transform* transform = camera != NULL_ENTITY ? scene->findComponent<Transform>(camera) : nullptr;
    return transform ? transform->worldPosition : Vector3(0.0f, 0.0f, 0.0f);
}

Vector3 FogScene::cameraPosition(Scene* scene, Entity camera){
    const Transform* transform = scene && camera != NULL_ENTITY ? scene->findComponent<Transform>(camera) : nullptr;
    return transform ? transform->worldPosition : mainCameraPosition(scene);
}

void FogScene::resolveAtMainCamera(Scene* scene, const FogComponent& fog, FogResolve& out){
    resolve(scene, fog, mainCameraPosition(scene), out);
}

std::string FogScene::profileKey(const FogComponent& fog){
    if (const FogRuntime* runtime = fog.runtime.get(); runtime && runtime->profileOverridden)
        return runtime->profileKey;
    return fogresolve::fogRefKey(fog.profile);
}

std::shared_ptr<const FogProfileData> FogScene::profileOf(const FogComponent& fog){
    if (const FogRuntime* runtime = fog.runtime.get(); runtime && runtime->profileOverridden)
        return FogProfileLibrary::get(runtime->profileKey);
    if (!fog.profile.isSet())
        return nullptr;
    return FogProfileLibrary::getRef(fog.profile);
}

bool FogScene::isFieldSetByProfile(const FogComponent& fog, FogField field){
    const std::shared_ptr<const FogProfileData> profile = profileOf(fog);
    return profile && profile->isSet(field);
}

bool FogScene::isRunning(Scene* scene){
    return scene && Engine::isSceneRunning(scene);
}

void FogScene::blendTo(FogComponent& fog, const std::string& key, double seconds){
    FogComponent from;
    fogresolve::currentFogBase(fog, from);
    FogRuntime& runtime = fog.runtime.ensure();
    runtime.from = from;
    runtime.targetKey = key;
    runtime.duration = std::max(seconds, 0.0);
    runtime.elapsed = 0.0;
    runtime.blending = true;
    if (!(seconds > 0.0))
        fogresolve::finishFogBlend(runtime);
}

void FogScene::setRuntimeProfile(FogComponent& fog, const std::string& key){
    FogRuntime& runtime = fog.runtime.ensure();
    runtime.blending = false;
    runtime.targetKey.clear();
    runtime.elapsed = runtime.duration = 0.0;
    runtime.profileOverridden = true;
    runtime.profileKey = key;
}

FogBlendState FogScene::blendState(const FogComponent& fog){
    FogBlendState state;
    const FogRuntime* runtime = fog.runtime.get();
    if (!runtime || !runtime->blending)
        return state;
    state.active = true;
    state.targetKey = runtime->targetKey;
    state.seconds = runtime->duration;
    state.elapsed = runtime->elapsed;
    state.progress = runtime->duration > 0.0 ? (float)std::clamp(runtime->elapsed / runtime->duration, 0.0, 1.0) : 1.0f;
    return state;
}

void FogScene::advance(Scene* scene, double dt){
    if (!scene || !(dt > 0.0))
        return;
    auto fogs = scene->getComponentArray<FogComponent>();
    for (size_t i = 0; i < fogs->size(); i++){
        FogRuntime* runtime = fogs->getComponentFromIndex(i).runtime.get();
        if (!runtime || !runtime->blending)
            continue;
        runtime->elapsed += dt;
        if (runtime->elapsed >= runtime->duration)
            fogresolve::finishFogBlend(*runtime);
    }
}

FogComponent& FogScene::writeTarget(Scene* scene, FogComponent& fog, FogField field){
    if (!isRunning(scene))
        return fog;
    FogRuntime& runtime = fog.runtime.ensure();
    if (!runtime.writes.set.test(static_cast<size_t>(field))){
        // a partial write (one channel of a colour) starts from the value it shows now
        FogFields::copy(runtime.writes.values, fog, field);
        runtime.writes.set.set(static_cast<size_t>(field));
    }
    return runtime.writes.values;
}

const FogComponent& FogScene::readSource(const FogComponent& fog, FogField field){
    const FogRuntime* runtime = fog.runtime.get();
    if (runtime && runtime->writes.set.test(static_cast<size_t>(field)))
        return runtime->writes.values;
    return fog;
}

bool FogScene::hasOverride(const FogComponent& fog, FogField field){
    const FogRuntime* runtime = fog.runtime.get();
    return runtime && runtime->writes.set.test(static_cast<size_t>(field));
}

void FogScene::clearOverride(FogComponent& fog, FogField field){
    if (FogRuntime* runtime = fog.runtime.get())
        runtime->writes.set.reset(static_cast<size_t>(field));
}

void FogScene::clearOverrides(FogComponent& fog){
    if (FogRuntime* runtime = fog.runtime.get())
        runtime->writes.set.reset();
}

void FogScene::resetRuntime(Scene* scene){
    if (!scene)
        return;
    auto fogs = scene->getComponentArray<FogComponent>();
    for (size_t i = 0; i < fogs->size(); i++)
        fogs->getComponentFromIndex(i).runtime.reset();
}
