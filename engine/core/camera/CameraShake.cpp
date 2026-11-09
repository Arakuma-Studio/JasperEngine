// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CameraShake.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "camera/ShakePresets.h"
#include "camera/ShakerRuntime.h"
#include "pool/ShakePresetPool.h"
#include "subsystem/CameraControlSystem.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::camera;

namespace{

    // one warning per unknown name (or unreadable file) in the process
    void warnOnce(const std::string& key, const std::string& message){
        static std::set<std::string>* warned = new std::set<std::string>();
        if (warned->insert(key).second){
            Log::warn("%s", message.c_str());
        }
    }

    bool hasShakeExtension(const std::string& name){
        const size_t length = std::char_traits<char>::length(SHAKE_EXTENSION);
        return name.size() > length && name.compare(name.size() - length, length, SHAKE_EXTENSION) == 0;
    }

    const char* stateName(const ShakeInstance& instance){
        if (instance.isPaused() && instance.getState() != ShakeState::Stopped) return "paused";
        switch (instance.getState()){
            case ShakeState::FadingIn: return "fading_in";
            case ShakeState::Sustained: return "sustained";
            case ShakeState::FadingOut: return "fading_out";
            default: return "stopped";
        }
    }

    ShakeInstanceHandle shakeEveryGlobal(const ShakeSource& source, const ShakeOptions& options, bool fromPoint,
                                         const Vector3& point, float radius){
        ShakePreset preset;
        if (!CameraShake::resolveSource(source, preset)) return ShakeInstanceHandle();
        ShakeInstanceHandle handle;
        bool any = false;
        int paused = 0;
        CameraShake::forEachGlobalShaker([&](Scene* scene, Entity entity){
            any = true;
            CameraShakerComponent& shaker = scene->getComponent<CameraShakerComponent>(entity);
            auto system = scene->getSystem<CameraControlSystem>();
            const float scale = fromPoint ? system->distanceScale(entity, shaker, point, radius) : 1.0f;
            const uint64_t id = system->play(entity, shaker, preset, source.name, options, scale);
            if (id != 0) handle.add(shaker.runtime, id);
        }, &paused);
        if (!any && paused > 0){
            // the editor's Play paused (or edit mode): nothing steps, the shake is dropped
            warnOnce("#paused-shakers", "CameraShake: the game is paused, a shake asked now is dropped (it does not wait "
                     "for the resume)");
        }else if (!any){
            warnOnce("#no-global-shaker", "CameraShake: no global Camera Shaker to shake (add a Camera Shaker to the camera, "
                     "with Global on)");
        }
        return handle;
    }

}

// ---------------------------------------------------------------- ShakeInstanceHandle

void ShakeInstanceHandle::add(const std::shared_ptr<ShakerRuntime>& runtime, uint64_t id){
    if (!runtime || id == 0) return;
    members.push_back({runtime, id});
}

void ShakeInstanceHandle::merge(const ShakeInstanceHandle& other){
    members.insert(members.end(), other.members.begin(), other.members.end());
}

template<typename F>
void ShakeInstanceHandle::forEachLive(F&& f) const{
    for (const Member& member : members){
        if (std::shared_ptr<ShakerRuntime> runtime = member.runtime.lock()){
            if (ShakerInstanceEntry* entry = runtime->findInstance(member.id)){
                f(*entry);
            }
        }
    }
}

ShakerInstanceEntry* ShakeInstanceHandle::firstLive() const{
    for (const Member& member : members){
        if (std::shared_ptr<ShakerRuntime> runtime = member.runtime.lock()){
            // the component owns the runtime: the entry stays valid while the caller reads it
            if (ShakerInstanceEntry* entry = runtime->findInstance(member.id)) return entry;
        }
    }
    return nullptr;
}

void ShakeInstanceHandle::stop(){
    stop(-1.0f);
}

void ShakeInstanceHandle::stop(float fade){
    forEachLive([&](ShakerInstanceEntry& entry){ entry.instance.stop(fade); });
}

void ShakeInstanceHandle::pause(){
    pause(0.0f);
}

void ShakeInstanceHandle::pause(float fade){
    forEachLive([&](ShakerInstanceEntry& entry){ entry.instance.pause(fade); });
}

void ShakeInstanceHandle::resume(){
    resume(0.0f);
}

void ShakeInstanceHandle::resume(float fade){
    forEachLive([&](ShakerInstanceEntry& entry){ entry.instance.resume(fade); });
}

bool ShakeInstanceHandle::isPaused() const{
    const ShakerInstanceEntry* entry = firstLive();
    return entry && entry->instance.isPaused();
}

bool ShakeInstanceHandle::isFinished() const{
    return firstLive() == nullptr;
}

bool ShakeInstanceHandle::isValid() const{
    return firstLive() != nullptr;
}

float ShakeInstanceHandle::getStrengthScale() const{
    const ShakerInstanceEntry* entry = firstLive();
    return entry ? entry->instance.strengthScale : 0.0f;
}

void ShakeInstanceHandle::setStrengthScale(float scale){
    if (!std::isfinite(scale)) return;
    forEachLive([&](ShakerInstanceEntry& entry){ entry.instance.strengthScale = std::max(0.0f, scale); });
}

float ShakeInstanceHandle::getRoughnessScale() const{
    const ShakerInstanceEntry* entry = firstLive();
    return entry ? entry->instance.roughnessScale : 0.0f;
}

void ShakeInstanceHandle::setRoughnessScale(float scale){
    if (!std::isfinite(scale)) return;
    forEachLive([&](ShakerInstanceEntry& entry){ entry.instance.roughnessScale = std::max(0.0f, scale); });
}

std::string ShakeInstanceHandle::getState() const{
    const ShakerInstanceEntry* entry = firstLive();
    return entry ? stateName(entry->instance) : "stopped";
}

int ShakeInstanceHandle::getCount() const{
    int count = 0;
    forEachLive([&](ShakerInstanceEntry&){ count++; });
    return count;
}

std::string ShakeInstanceHandle::toString() const{
    const ShakerInstanceEntry* entry = firstLive();
    if (!entry) return "ShakeInstance(stopped)";
    return "ShakeInstance(" + entry->presetName + ", " + stateName(entry->instance) + ", " + std::to_string(getCount()) +
           (getCount() == 1 ? " shaker)" : " shakers)");
}

// ---------------------------------------------------------------- CameraShake

bool CameraShake::resolvePreset(const std::string& nameOrPath, ShakePreset& out, std::string* error){
    if (nameOrPath.empty()){
        if (error) *error = "no preset name";
        return false;
    }
    if (!hasShakeExtension(nameOrPath)){
        if (const ShakePreset* builtin = findBuiltinShakePreset(nameOrPath)){
            out = *builtin;
            return true;
        }
        if (error){
            std::string names;
            for (const std::string& name : builtinShakePresetNames()) names += (names.empty() ? "" : ", ") + name;
            *error = "unknown shake preset '" + nameOrPath + "': a built-in name (" + names + ") or a .shake path";
        }
        return false;
    }
    std::string why;
    std::shared_ptr<const ShakePresetFile> file = ShakePresetPool::get(nameOrPath, &why);
    if (!file){
        if (error) *error = "shake preset " + why;
        return false;
    }
    out = file->preset;
    return true;
}

bool CameraShake::resolveSource(const ShakeSource& source, ShakePreset& out){
    if (source.inlinePreset){
        out = source.preset;
        sanitizeShakePreset(out);
        return true;
    }
    std::string error;
    if (resolvePreset(source.name, out, &error)) return true;
    warnOnce(source.name, "CameraShake: " + error);
    return false;
}

void CameraShake::forEachGlobalShaker(const std::function<void(Scene*, Entity)>& callback, int* pausedSkipped){
    for (Scene* scene : Engine::getSceneLayers()){
        // an editor scene in edit mode (its system paused) takes no shake: it would play on Play
        std::shared_ptr<CameraControlSystem> system = scene ? scene->findSystem<CameraControlSystem>() : nullptr;
        if (!system) continue;
        auto shakers = scene->getComponentArray<CameraShakerComponent>();
        if (system->isPaused()){
            if (pausedSkipped) *pausedSkipped += (int)shakers->size();
            continue;
        }
        // the entities first: a callback may grow a runtime, never the array
        std::vector<Entity> entities;
        for (size_t i = 0; i < shakers->size(); i++){
            const CameraShakerComponent& shaker = shakers->getComponentFromIndex(i);
            const Entity entity = shakers->getEntity(i);
            if (shaker.global && shaker.enabled && scene->isEntityActiveInHierarchy(entity)){
                entities.push_back(entity);
            }
        }
        for (Entity entity : entities){
            if (scene->findComponent<CameraShakerComponent>(entity)) callback(scene, entity);
        }
    }
}

ShakeInstanceHandle CameraShake::shake(const ShakeSource& preset){
    return shake(preset, ShakeOptions());
}

ShakeInstanceHandle CameraShake::shake(const ShakeSource& preset, const ShakeOptions& options){
    return shakeEveryGlobal(preset, options, false, Vector3(0, 0, 0), 0.0f);
}

ShakeInstanceHandle CameraShake::shakeAt(const Vector3& position, float radius, const ShakeSource& preset){
    return shakeAt(position, radius, preset, ShakeOptions());
}

ShakeInstanceHandle CameraShake::shakeAt(const Vector3& position, float radius, const ShakeSource& preset, const ShakeOptions& options){
    return shakeEveryGlobal(preset, options, true, position, radius);
}

ShakeInstanceHandle CameraShake::kick(float amplitudePx, float seconds){
    return shake(ShakeSource(camera::makeKickPreset(amplitudePx, seconds)));
}

void CameraShake::addTrauma(float amount){
    forEachGlobalShaker([&](Scene* scene, Entity entity){
        CameraControlSystem::addTrauma(scene->getComponent<CameraShakerComponent>(entity), amount);
    });
}

void CameraShake::stopAll(){
    stopAll(-1.0f);
}

void CameraShake::stopAll(float fade){
    forEachGlobalShaker([&](Scene* scene, Entity entity){
        CameraControlSystem::stopAll(scene->getComponent<CameraShakerComponent>(entity), fade);
    });
}

std::vector<std::string> CameraShake::getPresetNames(){
    return builtinShakePresetNames();
}
