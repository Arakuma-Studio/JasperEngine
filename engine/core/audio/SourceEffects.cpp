// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SourceEffects.h"
#include "AudioFilters.h"
#include "AudioMixerIO.h"

#include "subsystem/AudioSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // the effects a chain holds: the known per-source types of the list, in order (the extra ones
    // of a file with more than maxEffects are left out, as an unknown type is)
    struct SourceEffectSlot {
        AudioEffectType type;
        const AudioSourceEffectDef* def;
        size_t index;
    };

    std::vector<SourceEffectSlot> sourceEffectSlots(const AudioSourceComponent& audio) {
        std::vector<SourceEffectSlot> slots;
        const size_t count = std::min(audio.effects.size(), (size_t)AudioSourceComponent::maxEffects);
        for (size_t i = 0; i < count; i++) {
            if (const AudioEffectInfo* info = AudioEffects::findPerSource(audio.effects[i].type)) {
                slots.push_back({info->type, &audio.effects[i], i});
            }
        }
        return slots;
    }

    std::vector<AudioEffectType> sourceEffectTypes(const std::vector<SourceEffectSlot>& slots) {
        std::vector<AudioEffectType> types;
        for (const SourceEffectSlot& slot : slots) types.push_back(slot.type);
        return types;
    }

    EffectFilters::ParamValue sourceEffectValues(const SourceEffectSlot& slot) {
        return [slot](const AudioEffectParamInfo& param) { return SourceEffects::paramValue(*slot.def, slot.type, param); };
    }

    std::vector<float> sourceEffectAttributes(const SourceEffectSlot& slot, float sampleRate) {
        return EffectFilters::attributes(slot.type, sourceEffectValues(slot), slot.def->bypass ? 0.0f : slot.def->wet, sampleRate);
    }

    // a biquad's type (attribute 1) is fixed by its setParams
    bool sourceEffectWritable(AudioEffectType type, size_t attribute) {
        const bool biquad = type == AudioEffectType::LowPass || type == AudioEffectType::HighPass || type == AudioEffectType::BandPass;
        return !(biquad && attribute == 1);
    }

    float sourceSampleRate(const SoLoud::AudioSource& sample) {
        return sample.mBaseSamplerate > 0.0f ? sample.mBaseSamplerate : 44100.0f;
    }

    // where an effect stores a value: wet, or its parameter under the parameter's own name (a key
    // that names it another way, "roomsize", is folded into it)
    float& sourceEffectStored(AudioSourceEffectDef& def, AudioEffectType type, const AudioEffectParamInfo& param) {
        if (std::string(param.name) == "wet") return def.wet;
        const float current = SourceEffects::paramValue(def, type, param);
        for (auto it = def.params.begin(); it != def.params.end();) {
            if (it->first != param.name && AudioEffects::findParam(type, it->first) == &param) it = def.params.erase(it);
            else ++it;
        }
        auto [it, inserted] = def.params.emplace(param.name, current);
        return it->second;
    }

    // a chain's voices that ended are let go, and so are its wrappers without voices
    void pruneSourceEffectChain(SourceEffectChain& chain) {
        const bool inited = AudioSystem::isInited();
        for (auto& sound : chain.sounds) {
            auto& handles = sound->handles;
            handles.erase(std::remove_if(handles.begin(), handles.end(), [inited](unsigned int handle) {
                return !inited || !AudioSystem::getSoloud().isValidVoiceHandle(handle);
            }), handles.end());
        }
        chain.sounds.erase(std::remove_if(chain.sounds.begin(), chain.sounds.end(),
                                          [](const std::unique_ptr<SourceEffectSound>& sound) { return sound->handles.empty(); }),
                           chain.sounds.end());
    }

    // The list's effect each slot of a chain follows: the n-th of its type in the list now, so a
    // voice that kept an earlier structure still hears the values of its effects (REQ-2); -1: gone
    std::vector<int> sourceEffectMapping(const SourceEffectChain& chain, const std::vector<SourceEffectSlot>& slots) {
        std::vector<int> mapping(chain.types.size(), -1);
        for (size_t slot = 0; slot < chain.types.size(); slot++) {
            size_t nth = 0;
            for (size_t before = 0; before < slot; before++) nth += chain.types[before] == chain.types[slot] ? 1 : 0;
            for (size_t now = 0; now < slots.size(); now++) {
                if (slots[now].type != chain.types[slot]) continue;
                if (nth == 0) {
                    mapping[slot] = (int)now;
                    break;
                }
                nth--;
            }
        }
        return mapping;
    }

    // values that changed since its voices were last given them reach them; a moving value fades
    // over the frame so a timed change does not step
    void applySourceEffectValues(SourceEffectChain& chain, const std::vector<SourceEffectSlot>& slots, const std::vector<bool>& moving,
                                 double unscaledDt) {
        if (!AudioSystem::isInited()) return;
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        const std::vector<int> mapping = sourceEffectMapping(chain, slots);
        for (const auto& sound : chain.sounds) {
            const float rate = sourceSampleRate(*sound->getSample());
            sound->applied.resize(chain.types.size());
            for (size_t slot = 0; slot < chain.types.size(); slot++) {
                if (mapping[slot] < 0) continue;
                const SourceEffectSlot& effect = slots[(size_t)mapping[slot]];
                const std::vector<float> attrs = sourceEffectAttributes(effect, rate);
                std::vector<float>& applied = sound->applied[slot];
                if (applied.size() != attrs.size()) applied.assign(attrs.size(), NAN);
                const bool fade = moving[effect.index] && unscaledDt > 0.0;
                for (size_t attr = 0; attr < attrs.size(); attr++) {
                    if (applied[attr] == attrs[attr] || !sourceEffectWritable(effect.type, attr)) continue;
                    for (unsigned int handle : sound->handles) {
                        if (fade && !std::isnan(applied[attr])) {
                            soloud.fadeFilterParameter(handle, (unsigned int)slot, (unsigned int)attr, attrs[attr], unscaledDt);
                        } else {
                            soloud.setFilterParameter(handle, (unsigned int)slot, (unsigned int)attr, attrs[attr]);
                        }
                    }
                    applied[attr] = attrs[attr];
                }
            }
        }
    }

    // the list's structure changed: the chain's voices keep it until they end, the next start
    // makes a new one (REQ-2)
    void retireSourceEffectChain(SourceEffectRuntime& runtime, const std::vector<AudioEffectType>& types) {
        if (!runtime.chain || runtime.chain->types == types) return;
        pruneSourceEffectChain(*runtime.chain);
        if (runtime.chain->hasVoices()) runtime.retired.push_back(runtime.chain);
        runtime.chain.reset();
    }

}

// ---------------------------------------------------------------- SourceEffectSound

SourceEffectSound::SourceEffectSound(std::shared_ptr<SoLoud::AudioSource> sample, SourceEffectChain& chain): sample(std::move(sample)){
    copySampleState();
    for (size_t slot = 0; slot < chain.filters.size() && slot < FILTERS_PER_STREAM; slot++) {
        mFilter[slot] = chain.filters[slot].get();
    }
}

SourceEffectSound::~SourceEffectSound(){
    // its voices read the sample and the chain's filters: they stop before either can go
    stop();
    for (SoLoud::Filter*& filter : mFilter) filter = nullptr;
}

void SourceEffectSound::copySampleState(){
    mFlags = sample->mFlags;
    mBaseSamplerate = sample->mBaseSamplerate;
    mVolume = sample->mVolume;
    mChannels = sample->mChannels;
    mLoopPoint = sample->mLoopPoint;
    m3dMinDistance = sample->m3dMinDistance;
    m3dMaxDistance = sample->m3dMaxDistance;
    m3dAttenuationRolloff = sample->m3dAttenuationRolloff;
    m3dAttenuationModel = sample->m3dAttenuationModel;
    m3dDopplerFactor = sample->m3dDopplerFactor;
    mCollider = sample->mCollider;
    mAttenuator = sample->mAttenuator;
    mColliderData = sample->mColliderData;
}

SoLoud::AudioSourceInstance* SourceEffectSound::createInstance(){
    // Soloud::play reads this source's channels, rate and flags right after (AudioSourceInstance::init)
    copySampleState();
    sample->mSoloud = mSoloud;
    return sample->createInstance();
}

bool SourceEffectChain::hasVoices() const{
    for (const auto& sound : sounds) {
        if (!sound->handles.empty()) return true;
    }
    return false;
}

// ---------------------------------------------------------------- SourceEffects

float SourceEffects::paramValue(const AudioSourceEffectDef& effect, AudioEffectType type, const AudioEffectParamInfo& param){
    if (std::string(param.name) == "wet") return effect.wet;
    auto it = effect.params.find(param.name);
    if (it != effect.params.end()) return it->second;
    for (const auto& [name, value] : effect.params) {
        if (AudioEffects::findParam(type, name) == &param) return value;
    }
    return param.defaultValue;
}

bool SourceEffects::isActive(const AudioSourceComponent& audio, float sampleRate){
    for (const SourceEffectSlot& slot : sourceEffectSlots(audio)) {
        if (!slot.def->bypass && !EffectFilters::atOffValue(slot.type, sourceEffectValues(slot), sampleRate)) return true;
    }
    return false;
}

SoLoud::AudioSource* SourceEffects::wrap(AudioSourceComponent& audio, const std::shared_ptr<SoLoud::AudioSource>& sample){
    if (!sample || audio.effects.empty()) return nullptr;
    const float rate = sourceSampleRate(*sample);
    if (!isActive(audio, rate)) return nullptr;

    if (!audio.effectRuntime) audio.effectRuntime = std::make_shared<SourceEffectRuntime>();
    SourceEffectRuntime& runtime = *audio.effectRuntime;
    const std::vector<SourceEffectSlot> slots = sourceEffectSlots(audio);
    const std::vector<AudioEffectType> types = sourceEffectTypes(slots);
    retireSourceEffectChain(runtime, types);
    if (!runtime.chain) {
        auto chain = std::make_shared<SourceEffectChain>();
        chain->types = types;
        for (const SourceEffectSlot& slot : slots) {
            EffectFilters::countBuild();
            chain->filters.push_back(EffectFilters::make(slot.type, sourceEffectValues(slot), rate));
        }
        runtime.chain = std::move(chain);
    }
    SourceEffectChain& chain = *runtime.chain;
    for (const auto& sound : chain.sounds) {
        if (sound->getSample() == sample) return sound.get();
    }
    chain.sounds.push_back(std::make_unique<SourceEffectSound>(sample, chain));
    return chain.sounds.back().get();
}

void SourceEffects::started(AudioSourceComponent& audio, SoLoud::AudioSource& sound, unsigned int handle){
    if (!audio.effectRuntime || !audio.effectRuntime->chain || handle == 0) return;
    SourceEffectChain& chain = *audio.effectRuntime->chain;
    for (const auto& own : chain.sounds) {
        if (own.get() != &sound) continue;
        own->handles.push_back(handle);
        const float rate = sourceSampleRate(*own->getSample());
        const std::vector<SourceEffectSlot> slots = sourceEffectSlots(audio);
        std::vector<std::vector<float>> attrs;
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        for (size_t slot = 0; slot < slots.size(); slot++) {
            attrs.push_back(sourceEffectAttributes(slots[slot], rate));
            for (size_t attr = 0; attr < attrs.back().size(); attr++) {
                if (sourceEffectWritable(slots[slot].type, attr)) {
                    soloud.setFilterParameter(handle, (unsigned int)slot, (unsigned int)attr, attrs.back()[attr]);
                }
            }
        }
        if (own->applied.empty()) own->applied = std::move(attrs);
        return;
    }
}

void SourceEffects::update(AudioSourceComponent& audio, double scaledDt, double unscaledDt){
    if (!audio.effectRuntime) return;
    SourceEffectRuntime& runtime = *audio.effectRuntime;

    // timed changes: the component holds the value heard now (REQ-3)
    std::vector<bool> moving(audio.effects.size(), false);
    for (auto it = runtime.moves.begin(); it != runtime.moves.end();) {
        SourceEffectMove& move = *it;
        const AudioEffectInfo* info = move.index < audio.effects.size() && audio.effects[move.index].type == move.type ?
                                      AudioEffects::findPerSource(move.type) : nullptr;
        const AudioEffectParamInfo* param = info ? AudioEffects::findParam(info->type, move.param) : nullptr;
        float* stored = param ? &sourceEffectStored(audio.effects[move.index], info->type, *param) : nullptr;
        // the effect went, or something else set the value: the change is over
        if (!stored || *stored != move.written) {
            it = runtime.moves.erase(it);
            continue;
        }
        move.elapsed += move.unscaled ? unscaledDt : scaledDt;
        const double t = move.duration > 0.0 ? std::min(1.0, move.elapsed / move.duration) : 1.0;
        *stored = t >= 1.0 ? move.to : AudioMath::interpolate(move.curve, move.from, move.to, (float)t);
        move.written = *stored;
        moving[move.index] = true;
        if (t >= 1.0) it = runtime.moves.erase(it);
        else ++it;
    }

    const std::vector<SourceEffectSlot> slots = sourceEffectSlots(audio);
    retireSourceEffectChain(runtime, sourceEffectTypes(slots));
    for (auto it = runtime.retired.begin(); it != runtime.retired.end();) {
        pruneSourceEffectChain(**it);
        if ((*it)->sounds.empty()) it = runtime.retired.erase(it);
        else ++it;
    }

    // values reach every live voice in this frame, those of an earlier structure too (REQ-2)
    for (const auto& chain : runtime.retired) applySourceEffectValues(*chain, slots, moving, unscaledDt);
    if (runtime.chain) {
        pruneSourceEffectChain(*runtime.chain);
        applySourceEffectValues(*runtime.chain, slots, moving, unscaledDt);
        if (runtime.chain->sounds.empty()) runtime.chain.reset();
    }

    // nothing left to follow: the source costs nothing again
    if (!runtime.chain && runtime.retired.empty() && runtime.moves.empty()) audio.effectRuntime.reset();
}

bool SourceEffects::setParameter(AudioSourceComponent& audio, size_t index, const std::string& name, float value, float seconds, bool unscaled){
    if (index >= audio.effects.size()) return false;
    AudioSourceEffectDef& def = audio.effects[index];
    const AudioEffectInfo* info = AudioEffects::findPerSource(def.type);
    const AudioEffectParamInfo* param = info ? AudioEffects::findParam(info->type, name) : nullptr;
    if (!param || !std::isfinite(value)) return false;
    value = std::clamp(value, param->minValue, param->maxValue);
    float& stored = sourceEffectStored(def, info->type, *param);

    if (audio.effectRuntime) {
        auto& moves = audio.effectRuntime->moves;
        moves.erase(std::remove_if(moves.begin(), moves.end(), [&](const SourceEffectMove& move) {
            return move.index == index && move.param == param->name;
        }), moves.end());
    }
    if (!(seconds > 0.0f)) {
        // the frame's update gives it to the voices
        stored = value;
        return true;
    }
    if (!audio.effectRuntime) audio.effectRuntime = std::make_shared<SourceEffectRuntime>();
    SourceEffectMove move;
    move.index = index;
    move.type = def.type;
    move.param = param->name;
    move.curve = param->curve;
    move.from = stored;
    move.to = value;
    move.written = stored;
    move.duration = seconds;
    move.unscaled = unscaled;
    audio.effectRuntime->moves.push_back(move);
    return true;
}

bool SourceEffects::getParameter(const AudioSourceComponent& audio, size_t index, const std::string& name, float& value){
    if (index >= audio.effects.size()) return false;
    const AudioSourceEffectDef& def = audio.effects[index];
    const AudioEffectInfo* info = AudioEffects::findPerSource(def.type);
    const AudioEffectParamInfo* param = info ? AudioEffects::findParam(info->type, name) : nullptr;
    if (!param) return false;
    value = paramValue(def, info->type, *param);
    return true;
}

SoLoud::AudioSource* SourceEffects::sampleOf(SoLoud::AudioSource* sound){
    auto* wrapper = dynamic_cast<SourceEffectSound*>(sound);
    return wrapper ? wrapper->getSample().get() : nullptr;
}
