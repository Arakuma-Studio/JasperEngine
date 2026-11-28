// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioSourceEffect.h"
#include "AudioSource.h"
#include "audio/AudioEffects.h"
#include "audio/SourceEffects.h"
#include "component/AudioSourceComponent.h"
#include "Log.h"
#include "Scene.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // the 0-based place of an effect by its 1-based index or its type name ("lowpass": the first
    // of it); effects.size() when there is none
    size_t sourceEffectPlace(const AudioSourceComponent& audio, int index, const std::string& type) {
        if (type.empty()) {
            return index >= 1 && (size_t)index <= audio.effects.size() ? (size_t)index - 1 : audio.effects.size();
        }
        const AudioEffectInfo* wanted = AudioEffects::find(type);
        for (size_t i = 0; i < audio.effects.size(); i++) {
            const AudioEffectInfo* info = AudioEffects::find(audio.effects[i].type);
            if (wanted ? info == wanted : audio.effects[i].type == type) return i;
        }
        return audio.effects.size();
    }

}

// ---------------------------------------------------------------- AudioSourceEffect

AudioSourceEffect::AudioSourceEffect(Scene* scene, Entity entity, size_t index, std::string type):
    scene(scene), entity(entity), index(index), type(std::move(type)){
}

AudioSourceComponent* AudioSourceEffect::component() const{
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return nullptr;
    AudioSourceComponent* audio = scene->findComponent<AudioSourceComponent>(entity);
    if (!audio || index >= audio->effects.size() || audio->effects[index].type != type) return nullptr;
    return audio;
}

bool AudioSourceEffect::isValid() const{
    return component() != nullptr;
}

std::string AudioSourceEffect::getType() const{
    return type;
}

int AudioSourceEffect::getIndex() const{
    return (int)index + 1;
}

bool AudioSourceEffect::getBypass() const{
    AudioSourceComponent* audio = component();
    return audio && audio->effects[index].bypass;
}

void AudioSourceEffect::setBypass(bool bypass){
    if (AudioSourceComponent* audio = component()) {
        audio->effects[index].bypass = bypass;
        scene->markChanged<AudioSourceComponent>(entity);
    }
}

float AudioSourceEffect::getWet() const{
    AudioSourceComponent* audio = component();
    return audio ? audio->effects[index].wet : 0.0f;
}

void AudioSourceEffect::setWet(float wet){
    setParameter("wet", wet);
}

bool AudioSourceEffect::setParameter(const std::string& name, float value, float seconds, bool unscaled){
    AudioSourceComponent* audio = component();
    if (!audio) return false;
    if (!SourceEffects::setParameter(*audio, index, name, value, seconds, unscaled)) {
        Log::warn("AudioSourceEffect.setParameter: %s has no parameter '%s' (%s)", type.c_str(), name.c_str(),
                  AudioEffects::paramNames(AudioEffects::typeFromName(type)).c_str());
        return false;
    }
    scene->markChanged<AudioSourceComponent>(entity);
    return true;
}

bool AudioSourceEffect::getParameter(const std::string& name, float& value) const{
    AudioSourceComponent* audio = component();
    return audio && SourceEffects::getParameter(*audio, index, name, value);
}

bool AudioSourceEffect::hasParameter(const std::string& name) const{
    float value = 0.0f;
    return getParameter(name, value);
}

// ---------------------------------------------------------------- AudioSource's effect list

AudioSourceEffect AudioSource::addEffect(const std::string& type){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();
    const AudioEffectInfo* info = AudioEffects::findPerSource(type);
    if (!info) {
        Log::warn("AudioSource.addEffect: '%s' is not an effect a source can hold (%s)", type.c_str(), AudioEffects::perSourceNames().c_str());
        return AudioSourceEffect();
    }
    if ((int)audio.effects.size() >= AudioSourceComponent::maxEffects) {
        Log::warn("AudioSource.addEffect: a source holds up to %d effects (remove one first, or use a mixer group)",
                  AudioSourceComponent::maxEffects);
        return AudioSourceEffect();
    }
    AudioSourceEffectDef effect;
    effect.type = info->name;
    audio.effects.push_back(effect);
    markChanged<AudioSourceComponent>();
    return AudioSourceEffect(scene, entity, audio.effects.size() - 1, info->name);
}

AudioSourceEffect AudioSource::getEffect(int index) const{
    const AudioSourceComponent& audio = getComponent<AudioSourceComponent>();
    const size_t place = sourceEffectPlace(audio, index, "");
    if (place >= audio.effects.size()) return AudioSourceEffect();
    return AudioSourceEffect(scene, entity, place, audio.effects[place].type);
}

AudioSourceEffect AudioSource::getEffect(const std::string& type) const{
    const AudioSourceComponent& audio = getComponent<AudioSourceComponent>();
    const size_t place = sourceEffectPlace(audio, 0, type);
    if (place >= audio.effects.size()) return AudioSourceEffect();
    return AudioSourceEffect(scene, entity, place, audio.effects[place].type);
}

bool AudioSource::removeEffect(int index){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();
    const size_t place = sourceEffectPlace(audio, index, "");
    if (place >= audio.effects.size()) return false;
    audio.effects.erase(audio.effects.begin() + (std::ptrdiff_t)place);
    markChanged<AudioSourceComponent>();
    return true;
}

bool AudioSource::removeEffect(const std::string& type){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();
    const size_t place = sourceEffectPlace(audio, 0, type);
    if (place >= audio.effects.size()) return false;
    audio.effects.erase(audio.effects.begin() + (std::ptrdiff_t)place);
    markChanged<AudioSourceComponent>();
    return true;
}

int AudioSource::getEffectCount() const{
    return (int)getComponent<AudioSourceComponent>().effects.size();
}
