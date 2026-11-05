// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioMixer.h"
#include "AudioCapture.h"
#include "AudioFilters.h"

#include "Log.h"
#include "io/Data.h"
#include "io/UserSettings.h"
#include "subsystem/AudioSystem.h"

#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_biquadresonantfilter.h"
#include "soloud_echofilter.h"
#include "soloud_freeverbfilter.h"
#include "soloud_eqfilter.h"
#include "soloud_waveshaperfilter.h"
#include "soloud_flangerfilter.h"
#include "soloud_lofifilter.h"
#include "soloud_bassboostfilter.h"
#include "soloud_duckfilter.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

using namespace jasperengine;

namespace {

    // the slot of a group's level meter (soloud.h FILTERS_PER_STREAM is 9: 8 effects + it)
    constexpr unsigned int METER_SLOT = 8;
    constexpr unsigned int EFFECT_SLOTS = 8;

    struct ParamState {
        float authored = 0.0f;           // the asset's value
        float base = 0.0f;               // moved by snapshots
        AudioParamCurve curve = AudioParamCurve::Linear;
        bool moving = false;
        float from = 0.0f;
        float to = 0.0f;
        double elapsed = 0.0;
        double duration = 0.0;
        bool unscaled = false;

        // a script's value (setFloat, a group's volume, setParameter): wins until cleared
        bool overridden = false;
        float overrideValue = 0.0f;
        bool overrideMoving = false;
        float overrideFrom = 0.0f;
        float overrideTo = 0.0f;
        double overrideElapsed = 0.0;
        double overrideDuration = 0.0;
        bool overrideUnscaled = false;

        float value() const { return overridden ? overrideValue : base; }
        bool isMoving() const { return overridden ? overrideMoving : moving; }
        // where a moving value is going (what is remembered)
        float finalValue() const { return overridden ? (overrideMoving ? overrideTo : overrideValue) : (moving ? to : base); }
    };

    struct EffectState {
        std::string id;
        AudioEffectType type = AudioEffectType::Unknown;
        bool bypass = false;
        std::unique_ptr<SoLoud::Filter> filter;
        std::string sidechain;            // duck: the group name
        unsigned int sidechainHandle = 0;
        float echoDelay = 0.0f;           // echo: the delay its buffer was made for
        std::vector<float> applied;       // per SoLoud attribute, the last value written (NAN = never)
    };

    struct GroupState {
        std::string id;
        std::string name;
        std::string parentId;
        bool mute = false;
        bool solo = false;
        bool bypassEffects = false;
        std::vector<EffectState> effects;
        // the meter goes after the bus (its instance reads it)
        std::unique_ptr<MeterFilter> meter;
        std::unique_ptr<SoLoud::Bus> bus;
        unsigned int handle = 0;
        SoLoud::Filter* slots[EFFECT_SLOTS] = {};
        float appliedVolume = -1.0f;
    };

    struct State {
        AudioMixerDef def = AudioMixerIO::createDefault();
        std::string path;
        bool hasAsset = false;
        std::vector<std::unique_ptr<GroupState>> groups;
        std::map<std::string, ParamState> params;
        std::string currentSnapshot;
        bool built = false;
        // made once, on the first setLoudnessTap, and kept: the audio thread may still be in it
        std::unique_ptr<LoudnessTapRing> loudnessTap;
        bool loudnessTapOn = false;
        // the game's (AudioSystem.pauseAll) and the platform's (an app in the background): the
        // Master bus holds while either does, and neither lifts the other's
        bool masterPaused = false;
        bool platformPaused = false;
        double frameUnscaled = 0.0;
        // the unscaled time of every frame so far: the clock clocked voices start on
        double clock = 0.0;
        unsigned int voiceLimitChanges = 0;
        unsigned int filterBuilds = 0;
        float sampleRate = 44100.0f;
        std::set<std::string> warnedNames;
        std::set<std::string> rememberDirty;
        bool initialized = false;
    };

    State& state() {
        static State* instance = new State();
        return *instance;
    }

    void ensureInitialized();

    GroupState* findState(const std::string& idOrName) {
        State& s = state();
        for (auto& group : s.groups) {
            if (group->name == idOrName) return group.get();
        }
        for (auto& group : s.groups) {
            if (group->id == idOrName) return group.get();
        }
        return nullptr;
    }

    GroupState* masterState() {
        for (auto& group : state().groups) {
            if (group->parentId.empty()) return group.get();
        }
        return nullptr;
    }

    EffectState* findEffectState(const std::string& effectId, GroupState** owner = nullptr) {
        for (auto& group : state().groups) {
            for (EffectState& effect : group->effects) {
                if (effect.id == effectId) {
                    if (owner) *owner = group.get();
                    return &effect;
                }
            }
        }
        return nullptr;
    }

    bool resolveUnscaled(bool unscaled) {
        return unscaled || state().def.unscaled;
    }

    void warnOnce(const std::string& key, const std::string& message) {
        if (state().warnedNames.insert(key).second) {
            Log::warn("%s", message.c_str());
        }
    }

    std::string joinNames(const std::vector<std::string>& names) {
        std::string out;
        for (const std::string& name : names) out += (out.empty() ? "" : ", ") + name;
        return out.empty() ? "none" : out;
    }

    // ---- values

    void syncParams(const AudioMixerDef& oldDef, const AudioMixerDef& newDef, bool fresh) {
        State& s = state();
        std::map<std::string, ParamState> next;
        for (const std::string& target : AudioMixerIO::allTargets(newDef)) {
            float authored = 0.0f;
            AudioMixerIO::authoredValue(newDef, target, authored);
            auto it = s.params.find(target);
            ParamState param;
            if (it != s.params.end() && !fresh) {
                param = it->second;
                float old = 0.0f;
                const bool existed = AudioMixerIO::authoredValue(oldDef, target, old);
                if (!existed || old != authored) {
                    // a changed fader is heard at once (a script override still wins)
                    param.base = authored;
                    param.moving = false;
                }
            } else {
                param.base = authored;
            }
            param.authored = authored;
            param.curve = AudioMixerIO::curveOf(newDef, target);
            next[target] = param;
        }
        s.params = std::move(next);
    }

    void advance(double scaledDt, double unscaledDt) {
        for (auto& [target, param] : state().params) {
            if (param.moving) {
                param.elapsed += param.unscaled ? unscaledDt : scaledDt;
                const double t = param.duration > 0.0 ? param.elapsed / param.duration : 1.0;
                param.base = AudioMath::interpolate(param.curve, param.from, param.to, (float)std::min(1.0, t));
                if (t >= 1.0) {
                    param.base = param.to;
                    param.moving = false;
                }
            }
            if (param.overridden && param.overrideMoving) {
                param.overrideElapsed += param.overrideUnscaled ? unscaledDt : scaledDt;
                const double t = param.overrideDuration > 0.0 ? param.overrideElapsed / param.overrideDuration : 1.0;
                param.overrideValue = AudioMath::interpolate(param.curve, param.overrideFrom, param.overrideTo, (float)std::min(1.0, t));
                if (t >= 1.0) {
                    param.overrideValue = param.overrideTo;
                    param.overrideMoving = false;
                }
            }
        }
    }

    float paramValue(const std::string& target, float fallback) {
        auto it = state().params.find(target);
        return it != state().params.end() ? it->second.value() : fallback;
    }

    bool paramMoving(const std::string& target) {
        auto it = state().params.find(target);
        return it != state().params.end() && it->second.isMoving();
    }

    // ---- solo and mute (spec A4)

    bool isAudible(const GroupState& group) {
        if (group.mute) return false;
        State& s = state();
        bool anySolo = false;
        for (auto& other : s.groups) anySolo = anySolo || other->solo;
        if (!anySolo || group.solo) return true;
        for (auto& other : s.groups) {
            if (!other->solo) continue;
            // a soloed ancestor or descendant keeps it
            if (s.def.isUnder(group.id, other->id) || s.def.isUnder(other->id, group.id)) return true;
        }
        return false;
    }

    // ---- SoLoud filters

    std::unique_ptr<SoLoud::Filter> makeFilter(const AudioMixerEffectDef& def, EffectState& effect) {
        state().filterBuilds++;
        // the values heard now (a filter made again after a timed change starts where it is)
        auto param = [&def](const char* name) {
            const AudioEffectParamInfo* info = AudioEffects::findParam(def.type, name);
            if (!info) return 0.0f;
            return paramValue(AudioMixerIO::paramTarget(def.id, info->name), AudioMixerIO::effectParam(def, *info));
        };
        switch (def.type) {
            case AudioEffectType::LowPass:
            case AudioEffectType::HighPass:
            case AudioEffectType::BandPass: {
                auto filter = std::make_unique<SoLoud::BiquadResonantFilter>();
                const int type = def.type == AudioEffectType::LowPass ? SoLoud::BiquadResonantFilter::LOWPASS :
                                 def.type == AudioEffectType::HighPass ? SoLoud::BiquadResonantFilter::HIGHPASS :
                                 SoLoud::BiquadResonantFilter::BANDPASS;
                filter->setParams(type, std::clamp(param("cutoff"), 10.0f, 0.45f * state().sampleRate), std::max(0.1f, param("resonance")));
                return filter;
            }
            case AudioEffectType::Echo: {
                auto filter = std::make_unique<SoLoud::EchoFilter>();
                effect.echoDelay = std::max(0.01f, param("delay"));
                filter->setParams(effect.echoDelay, std::clamp(param("decay"), 0.001f, 1.0f), std::clamp(param("filter"), 0.0f, 0.99f));
                return filter;
            }
            case AudioEffectType::Reverb: {
                auto filter = std::make_unique<SoLoud::FreeverbFilter>();
                filter->setParams(param("freeze"), param("roomSize"), param("damp"), param("width"));
                return filter;
            }
            case AudioEffectType::Equalizer:
                return std::make_unique<SoLoud::EqFilter>();
            case AudioEffectType::Distortion: {
                auto filter = std::make_unique<SoLoud::WaveShaperFilter>();
                filter->setParams(std::clamp(param("amount"), -1.0f, 1.0f));
                return filter;
            }
            case AudioEffectType::Flanger: {
                auto filter = std::make_unique<SoLoud::FlangerFilter>();
                filter->setParams(std::max(0.001f, param("delay")), std::max(0.01f, param("frequency")));
                return filter;
            }
            case AudioEffectType::Lofi: {
                auto filter = std::make_unique<SoLoud::LofiFilter>();
                filter->setParams(std::max(100.0f, param("sampleRate")), std::max(0.5f, param("bitDepth")));
                return filter;
            }
            case AudioEffectType::BassBoost: {
                auto filter = std::make_unique<SoLoud::BassboostFilter>();
                filter->setParams(param("boost"));
                return filter;
            }
            case AudioEffectType::Duck: {
                // listens to the side-chain group's bus: made once that bus plays
                if (!state().built) return nullptr;
                GroupState* sidechain = def.sidechainGroup.empty() ? nullptr : findState(def.sidechainGroup);
                if (!sidechain || sidechain->handle == 0) return nullptr;
                auto filter = std::make_unique<SoLoud::DuckFilter>();
                if (filter->setParams(&AudioSystem::getSoloud(), sidechain->handle, param("attack"), param("release"), param("level")) != SoLoud::SO_NO_ERROR) {
                    return nullptr;
                }
                effect.sidechainHandle = sidechain->handle;
                return filter;
            }
            case AudioEffectType::Limiter: {
                auto filter = std::make_unique<LimiterFilter>();
                filter->threshold = param("threshold");
                filter->release = param("release");
                filter->lookahead = param("lookahead");
                filter->truePeak = param("truePeak") >= 0.5f;
                filter->sampleRate = state().sampleRate;
                filter->bypass = effect.bypass;
                return filter;
            }
            case AudioEffectType::Unknown:
                break;
        }
        return nullptr;
    }

    // what each SoLoud attribute of an effect gets now: its values with the A5 bypass rule
    std::vector<float> effectAttributes(const AudioMixerEffectDef& def) {
        const AudioEffectInfo* info = AudioEffects::find(def.type);
        if (!info) return {};
        int count = 1;
        for (const AudioEffectParamInfo& param : info->params) count = std::max(count, param.attribute + 1);
        std::vector<float> attrs(count, 0.0f);
        float wet = paramValue(AudioMixerIO::paramTarget(def.id, "wet"), def.wet);
        for (const AudioEffectParamInfo& param : info->params) {
            float value = paramValue(AudioMixerIO::paramTarget(def.id, param.name), AudioMixerIO::effectParam(def, param));
            value = std::clamp(value, param.minValue, param.maxValue);
            if (std::string(param.name) == "cutoff") {
                const float bypassAt = AudioEffects::lowPassBypassRatio * state().sampleRate;
                if (def.type == AudioEffectType::LowPass && value >= bypassAt) {
                    // Unity's 22000 Hz "off": no biquad near Nyquist; the filter keeps running
                    // close to it, so going under the threshold does not click
                    value = bypassAt;
                    wet = 0.0f;
                } else if (def.type == AudioEffectType::HighPass && value <= AudioEffects::highPassBypassHz) {
                    value = AudioEffects::highPassBypassHz;
                    wet = 0.0f;
                }
                value = std::clamp(value, 10.0f, 0.49f * state().sampleRate);
            }
            attrs[param.attribute] = value;
        }
        attrs[0] = std::clamp(wet, 0.0f, 1.0f);
        if (def.type == AudioEffectType::Limiter) attrs.resize(LimiterFilter::ATTRIBUTE_COUNT, 0.0f);
        if (def.type == AudioEffectType::Reverb) {
            // freeze is a switch
            attrs[1] = attrs[1] >= 0.5f ? 1.0f : 0.0f;
        }
        return attrs;
    }

    const AudioMixerEffectDef* effectDef(const std::string& effectId) {
        size_t index = 0;
        const AudioMixerGroupDef* group = state().def.findEffect(effectId, &index);
        return group ? &group->effects[index] : nullptr;
    }

    // A limiter with a delay stays in its slot while bypassed: an empty slot would drop the delay's
    // audio at the switch. It then only passes the signal and keeps its last frames.
    bool limiterKeepsItsSlot(const EffectState& effect) {
        if (effect.type != AudioEffectType::Limiter) return false;
        const AudioMixerEffectDef* def = effectDef(effect.id);
        if (!def) return false;
        const std::vector<float> attrs = effectAttributes(*def);
        return attrs[LimiterFilter::LOOKAHEAD] > 0.0f || attrs[LimiterFilter::TRUE_PEAK] >= 0.5f;
    }

    // the slots of a bus as its effects say (a bypassed or inactive effect leaves its slot empty)
    void writeSlots(GroupState& group) {
        if (!group.bus) return;
        for (unsigned int slot = 0; slot < EFFECT_SLOTS; slot++) {
            SoLoud::Filter* desired = nullptr;
            if (slot < group.effects.size()) {
                EffectState& effect = group.effects[slot];
                if ((!effect.bypass && !group.bypassEffects) || limiterKeepsItsSlot(effect)) desired = effect.filter.get();
            }
            if (group.slots[slot] != desired) {
                group.bus->setFilter(slot, desired);
                group.slots[slot] = desired;
                if (slot < group.effects.size()) group.effects[slot].applied.clear();
            }
        }
    }

    void rebuildFilter(GroupState& group, size_t index) {
        EffectState& effect = group.effects[index];
        const AudioMixerEffectDef* def = effectDef(effect.id);
        // the bus lets go of the old filter first
        if (group.bus && index < EFFECT_SLOTS && group.slots[index]) {
            group.bus->setFilter((unsigned int)index, nullptr);
            group.slots[index] = nullptr;
        }
        effect.filter.reset();
        effect.sidechainHandle = 0;
        effect.applied.clear();
        if (def) effect.filter = makeFilter(*def, effect);
    }

    void applyGroup(GroupState& group, double frameDt) {
        if (!group.bus || group.handle == 0) return;
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        const std::string volumeTarget = AudioMixerIO::volumeTarget(group.id);
        const float db = paramValue(volumeTarget, 0.0f);
        const float linear = AudioMath::dbToLinear(db) * (isAudible(group) ? 1.0f : 0.0f);
        if (linear != group.appliedVolume) {
            if (paramMoving(volumeTarget) && frameDt > 0.0) soloud.fadeVolume(group.handle, linear, frameDt);
            else soloud.setVolume(group.handle, linear);
            group.appliedVolume = linear;
        }

        for (size_t index = 0; index < group.effects.size() && index < EFFECT_SLOTS; index++) {
            EffectState& effect = group.effects[index];
            const AudioMixerEffectDef* def = effectDef(effect.id);
            if (!def || !effect.filter) continue;
            std::vector<float> attrs = effectAttributes(*def);
            if (def->type == AudioEffectType::Limiter) {
                attrs[LimiterFilter::BYPASS] = (effect.bypass || group.bypassEffects) ? 1.0f : 0.0f;
                // a parameter can take a limiter into or out of keeping its slot
                writeSlots(group);
            }
            // (not while a timed change moves it: once, where it ends; meanwhile SoLoud keeps the delay
            // within the buffer it has)
            if (def->type == AudioEffectType::Echo && attrs.size() > 1 && attrs[1] > effect.echoDelay + 1e-4f &&
                !paramMoving(AudioMixerIO::paramTarget(effect.id, "delay"))) {
                // the echo's buffer is as long as its delay was: a longer one needs a new filter
                rebuildFilter(group, index);
                writeSlots(group);
                if (!effect.filter) continue;
            }
            if (group.slots[index] != effect.filter.get()) continue;
            if (effect.applied.size() != attrs.size()) effect.applied.assign(attrs.size(), NAN);
            bool moving = paramMoving(AudioMixerIO::paramTarget(effect.id, "wet"));
            if (const AudioEffectInfo* info = AudioEffects::find(def->type)) {
                for (const AudioEffectParamInfo& param : info->params) {
                    moving = moving || paramMoving(AudioMixerIO::paramTarget(effect.id, param.name));
                }
            }
            for (size_t attr = 0; attr < attrs.size(); attr++) {
                // a biquad's type (attribute 1) is fixed by setParams
                if ((def->type == AudioEffectType::LowPass || def->type == AudioEffectType::HighPass ||
                     def->type == AudioEffectType::BandPass) && attr == 1) {
                    continue;
                }
                if (effect.applied[attr] == attrs[attr]) continue;
                if (moving && frameDt > 0.0 && !std::isnan(effect.applied[attr])) {
                    soloud.fadeFilterParameter(group.handle, (unsigned int)index, (unsigned int)attr, attrs[attr], frameDt);
                } else {
                    soloud.setFilterParameter(group.handle, (unsigned int)index, (unsigned int)attr, attrs[attr]);
                }
                effect.applied[attr] = attrs[attr];
            }
        }
    }

    void applyAll(double frameDt) {
        if (!state().built) return;
        for (auto& group : state().groups) applyGroup(*group, frameDt);
    }

    // ---- the bus graph

    void playBus(GroupState& group) {
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        group.bus = std::make_unique<SoLoud::Bus>();
        if (!group.meter) group.meter = std::make_unique<MeterFilter>();
        group.meter->reset();
        group.bus->setVisualizationEnable(true);
        // a bus never ends by itself; and the vendored mixer reads a bus without autostop only for
        // the frames of the mix call (no read-ahead: a voice started into it is heard in the next
        // call, not up to 511 frames later; SoLoud's local patch in mixBus_internal)
        group.bus->setAutoStop(false);
        group.bus->mBaseSamplerate = state().sampleRate;
        group.bus->setFilter(METER_SLOT, group.meter.get());
        for (SoLoud::Filter*& slot : group.slots) slot = nullptr;
        group.appliedVolume = -1.0f;
        for (EffectState& effect : group.effects) effect.applied.clear();

        GroupState* parent = group.parentId.empty() ? nullptr : findState(group.parentId);
        // started paused: the pan below is its pan from its first sample. Set on a running voice it would
        // ramp from the centre's 0.707 over the next mix call, so a game's first sounds would depend on
        // how long that call is (a setVolume while paused takes the channel volumes at once).
        if (parent && parent->bus && parent->handle) {
            group.handle = parent->bus->play(*group.bus, 1.0f, 0.0f, true);
        } else {
            group.handle = soloud.play(*group.bus, 1.0f, 0.0f, true);
        }
        if (group.handle) {
            // SoLoud learns a bus's own handle only when something plays through it; a bus stopped
            // before that would stop every voice of the core (its instance stops the voices whose
            // bus handle is its own, 0 = the core's), Master and all
            group.bus->mChannelHandle = group.handle;
            soloud.setProtectVoice(group.handle, true);
            // a bus passes its stereo through at unity: SoLoud's centre pan (constant power, 0.707
            // per side) would take 3 dB off at every level of the tree
            soloud.setPanAbsolute(group.handle, 1.0f, 1.0f);
            soloud.setVolume(group.handle, 1.0f);
            soloud.setPause(group.handle, false);
        }
    }

    void stopBus(GroupState& group) {
        if (group.bus && group.handle && AudioSystem::isInited()) {
            AudioSystem::getSoloud().stop(group.handle);
        }
        group.bus.reset();
        group.handle = 0;
        for (SoLoud::Filter*& slot : group.slots) slot = nullptr;
    }

    void updateVoiceLimit() {
        if (!AudioSystem::isInited()) return;
        // SoLoud mixes at most 255 voices at once (its active-voice map); the buses count
        const unsigned int count = std::min(AudioMixer::maxActiveVoices,
            (unsigned int)AudioSystem::getConfig().maxRealVoices + (unsigned int)state().groups.size());
        // only when it changes: SoLoud reallocates every voice's resample buffers (a dropout)
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        if (soloud.getMaxActiveVoiceCount() != count) {
            soloud.setMaxActiveVoiceCount(count);
            state().voiceLimitChanges++;
        }
    }

    // groups, effects and their buses as the def says, keeping what stays (spec 7.1 apply)
    void syncGraph(const AudioMixerDef& oldDef, bool fresh) {
        State& s = state();
        const AudioMixerDef& def = s.def;

        std::vector<std::unique_ptr<GroupState>> previous = std::move(s.groups);
        s.groups.clear();
        // another mixer (or a file whose Master has another id): the Master bus stays, under the new
        // id, so what plays through it (sources, video, the Inspector preview) goes on
        if (const AudioMixerGroupDef* newMaster = def.master()) {
            bool kept = false;
            for (auto& old : previous) kept = kept || (old && old->id == newMaster->id);
            if (!kept) {
                for (auto& old : previous) {
                    if (old && old->parentId.empty()) {
                        AudioSystem::moveGroupVoices(old->id, newMaster->id);
                        old->id = newMaster->id;
                        break;
                    }
                }
            }
        }
        std::vector<std::unique_ptr<GroupState>> removed;
        std::vector<EffectState> droppedEffects;

        for (const AudioMixerGroupDef* groupDef : def.treeOrder()) {
            std::unique_ptr<GroupState> group;
            for (auto& old : previous) {
                if (old && old->id == groupDef->id) {
                    group = std::move(old);
                    break;
                }
            }
            const AudioMixerGroupDef* oldGroup = fresh ? nullptr : oldDef.findGroupById(groupDef->id);
            if (!group) {
                group = std::make_unique<GroupState>();
                group->id = groupDef->id;
                group->meter = std::make_unique<MeterFilter>();
            }
            group->name = groupDef->name;
            group->parentId = groupDef->parentId;
            // runtime switches follow the asset where it changed
            if (!oldGroup || oldGroup->mute != groupDef->mute) group->mute = groupDef->mute;
            if (!oldGroup || oldGroup->solo != groupDef->solo) group->solo = groupDef->solo;
            if (!oldGroup || oldGroup->bypassEffects != groupDef->bypassEffects) group->bypassEffects = groupDef->bypassEffects;

            std::vector<EffectState> effects;
            for (const AudioMixerEffectDef& effectDef : groupDef->effects) {
                EffectState effect;
                bool found = false;
                for (EffectState& old : group->effects) {
                    if (old.id == effectDef.id && old.type == effectDef.type) {
                        effect = std::move(old);
                        old.id.clear();
                        found = true;
                        break;
                    }
                }
                const AudioMixerEffectDef* oldEffect = nullptr;
                if (!fresh) {
                    size_t index = 0;
                    if (const AudioMixerGroupDef* owner = oldDef.findEffect(effectDef.id, &index)) oldEffect = &owner->effects[index];
                }
                effect.id = effectDef.id;
                effect.type = effectDef.type;
                if (!oldEffect || oldEffect->bypass != effectDef.bypass) effect.bypass = effectDef.bypass;
                const bool sidechainChanged = effect.sidechain != effectDef.sidechainGroup;
                effect.sidechain = effectDef.sidechainGroup;
                if (!found || sidechainChanged) {
                    effect.filter.reset();
                    effect.applied.clear();
                }
                effects.push_back(std::move(effect));
            }
            for (EffectState& old : group->effects) {
                if (!old.id.empty()) droppedEffects.push_back(std::move(old));
            }
            group->effects = std::move(effects);
            s.groups.push_back(std::move(group));
        }
        for (auto& old : previous) {
            if (old) removed.push_back(std::move(old));
        }

        if (!s.built || !AudioSystem::isInited()) {
            // no buses yet: the filters are made with them
            for (auto& group : removed) stopBus(*group);
            return;
        }

        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        // new buses (parents first), moved ones into their new parent
        for (auto& group : s.groups) {
            if (!group->bus || group->handle == 0 || !soloud.isValidVoiceHandle(group->handle)) {
                playBus(*group);
                continue;
            }
            const AudioMixerGroupDef* oldGroup = oldDef.findGroupById(group->id);
            if (oldGroup && oldGroup->parentId != group->parentId) {
                // (Master never moves: validate keeps exactly one group without a parent)
                GroupState* parent = findState(group->parentId);
                if (parent && parent->bus) {
                    parent->bus->annexSound(group->handle);
                }
            }
        }
        // the voices of a removed group go on in Master (spec 7.1), then its bus stops
        // (all of them first: stopping a bus stops what still plays in it, sub-buses included)
        GroupState* master = masterState();
        for (auto& group : removed) {
            if (master && master->bus) {
                for (unsigned int handle : AudioSystem::getGroupVoices(group->id)) master->bus->annexSound(handle);
                AudioSystem::moveGroupVoices(group->id, "");
            }
        }
        // children first; its sub-buses that stay were moved above
        for (auto it = removed.rbegin(); it != removed.rend(); ++it) stopBus(**it);

        // filters: made where missing (a duck needs its side-chain bus), then the slots
        for (auto& group : s.groups) {
            for (size_t index = 0; index < group->effects.size(); index++) {
                EffectState& effect = group->effects[index];
                if (!effect.filter && effect.type != AudioEffectType::Unknown) {
                    if (const AudioMixerEffectDef* def = effectDef(effect.id)) effect.filter = makeFilter(*def, effect);
                }
                // a side-chain bus that was made again: the duck listens to the new one
                if (effect.type == AudioEffectType::Duck && effect.filter) {
                    GroupState* sidechain = findState(effect.sidechain);
                    if (!sidechain || sidechain->handle != effect.sidechainHandle) rebuildFilter(*group, index);
                }
            }
            writeSlots(*group);
        }
        // dropped effects go once no slot holds them
        droppedEffects.clear();

        updateVoiceLimit();
        if (GroupState* root = masterState()) {
            if (root->handle) soloud.setPause(root->handle, s.masterPaused || s.platformPaused);
        }
        applyAll(0.0);
    }

    void setDef(const AudioMixerDef& def, const std::string& path, bool hasAsset, bool fresh) {
        State& s = state();
        const AudioMixerDef oldDef = s.def;
        s.def = def;
        if (!s.def.master()) s.def = AudioMixerIO::createDefault();
        s.path = path;
        s.hasAsset = hasAsset;
        syncParams(oldDef, s.def, fresh);
        syncGraph(oldDef, fresh);
        if (!s.currentSnapshot.empty() && !s.def.findSnapshot(s.currentSnapshot)) s.currentSnapshot.clear();
    }

    void ensureInitialized() {
        State& s = state();
        if (s.initialized) return;
        s.initialized = true;
        syncParams(s.def, s.def, true);
        syncGraph(s.def, true);
    }

    void startTransition(ParamState& param, float to, float seconds, bool unscaled) {
        if (!(seconds > 0.0f)) {
            param.base = to;
            param.moving = false;
            return;
        }
        param.moving = true;
        param.from = param.base;
        param.to = to;
        param.elapsed = 0.0;
        param.duration = seconds;
        param.unscaled = unscaled;
    }

}

// ---------------------------------------------------------------- the asset

bool AudioMixer::loadFromText(const std::string& json, const std::string& path, std::string* error){
    ensureInitialized();
    AudioMixerDef def;
    std::vector<std::string> warnings;
    if (!AudioMixerIO::parse(json, def, error, &warnings)) {
        return false;
    }
    for (const std::string& warning : warnings) {
        Log::warn("Audio mixer '%s': %s", path.c_str(), warning.c_str());
    }
    setDef(def, path, true, true);
    resetRuntime(true);
    loadRemembered();
    Log::verbose("Audio mixer loaded: %s (%zu groups)", path.c_str(), state().def.groups.size());
    return true;
}

bool AudioMixer::load(const std::string& path, std::string* error){
    ensureInitialized();
    if (path.empty()) {
        unload();
        return true;
    }
    Data data;
    if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) {
        const std::string message = "cannot open '" + path + "'";
        if (error) *error = message;
        Log::error("Audio mixer: %s (the implicit Master plays)", message.c_str());
        return false;
    }
    const std::string text = data.readString();
    std::string parseError;
    if (!loadFromText(text, path, &parseError)) {
        if (error) *error = parseError;
        Log::error("Audio mixer '%s': %s (the implicit Master plays)", path.c_str(), parseError.c_str());
        return false;
    }
    return true;
}

void AudioMixer::apply(const AudioMixerDef& def){
    ensureInitialized();
    const std::string invalid = AudioMixerIO::validate(def);
    if (!invalid.empty()) {
        Log::error("Audio mixer: %s (edit not applied)", invalid.c_str());
        return;
    }
    State& s = state();
    setDef(def, s.path, true, false);
}

void AudioMixer::unload(){
    ensureInitialized();
    State& s = state();
    // the implicit Master keeps the id of the old Master, so its voices stay on the same bus
    AudioMixerDef def = AudioMixerIO::createDefault();
    if (const AudioMixerGroupDef* master = s.def.master()) def.groups[0].id = master->id;
    setDef(def, "", false, false);
    resetRuntime(false);
}

const AudioMixerDef& AudioMixer::getDef(){
    ensureInitialized();
    return state().def;
}

const std::string& AudioMixer::getPath(){
    return state().path;
}

void AudioMixer::setAssetPath(const std::string& path){
    state().path = path;
    state().hasAsset = true;
}

bool AudioMixer::hasAsset(){
    return state().hasAsset;
}

void AudioMixer::resetRuntime(bool startSnapshot){
    ensureInitialized();
    State& s = state();
    const AudioMixerSnapshotDef* snapshot = startSnapshot ? s.def.findSnapshot(s.def.startSnapshot) : nullptr;
    for (auto& [target, param] : s.params) {
        param = ParamState{param.authored, param.authored, param.curve};
        if (snapshot) {
            auto it = snapshot->values.find(target);
            if (it != snapshot->values.end()) param.base = it->second;
        }
    }
    s.currentSnapshot = snapshot ? snapshot->name : "";
    for (auto& group : s.groups) {
        if (const AudioMixerGroupDef* def = s.def.findGroupById(group->id)) {
            group->mute = def->mute;
            group->solo = def->solo;
            group->bypassEffects = def->bypassEffects;
            for (EffectState& effect : group->effects) {
                if (const AudioMixerEffectDef* effectDef = ::effectDef(effect.id)) effect.bypass = effectDef->bypass;
            }
        }
        writeSlots(*group);
    }
    s.rememberDirty.clear();
    s.warnedNames.clear();
    applyAll(0.0);
}

// ---------------------------------------------------------------- targets

bool AudioMixer::setTarget(const std::string& target, float value, float seconds, bool unscaled){
    ensureInitialized();
    State& s = state();
    auto it = s.params.find(target);
    if (it == s.params.end()) return false;
    ParamState& param = it->second;
    value = AudioMixerIO::clampValue(s.def, target, value);
    const float current = param.value();
    param.overridden = true;
    if (!(seconds > 0.0f)) {
        param.overrideValue = value;
        param.overrideMoving = false;
    } else {
        param.overrideValue = current;
        param.overrideFrom = current;
        param.overrideTo = value;
        param.overrideElapsed = 0.0;
        param.overrideDuration = seconds;
        param.overrideUnscaled = resolveUnscaled(unscaled);
        param.overrideMoving = true;
    }
    if (!(seconds > 0.0f)) applyAll(0.0);
    return true;
}

bool AudioMixer::clearTarget(const std::string& target){
    ensureInitialized();
    auto it = state().params.find(target);
    if (it == state().params.end()) return false;
    it->second.overridden = false;
    it->second.overrideMoving = false;
    applyAll(0.0);
    return true;
}

bool AudioMixer::getTarget(const std::string& target, float& value){
    ensureInitialized();
    auto it = state().params.find(target);
    if (it == state().params.end()) return false;
    value = it->second.value();
    return true;
}

bool AudioMixer::isTargetOverridden(const std::string& target){
    ensureInitialized();
    auto it = state().params.find(target);
    return it != state().params.end() && it->second.overridden;
}

// ---------------------------------------------------------------- exposed parameters

bool AudioMixer::setFloat(const std::string& name, float value, float seconds, bool unscaled){
    ensureInitialized();
    const AudioMixerExposedDef* exposed = state().def.findExposed(name);
    if (!exposed) {
        warnOnce("setFloat:" + name, "AudioMixer.setFloat: no exposed parameter '" + name + "' (exposed: " + joinNames(getParameterNames()) + ")");
        return false;
    }
    if (!setTarget(exposed->target, value, seconds, unscaled)) return false;
    if (exposed->remember) state().rememberDirty.insert(name);
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioCapture::isActive()) {
        AudioCapture::Line line("mixer");
        line.text("action", "set_float").text("name", name).number("value", value, 6).number("seconds", seconds, 6);
    }
#endif
    return true;
}

bool AudioMixer::getFloat(const std::string& name, float& value){
    ensureInitialized();
    const AudioMixerExposedDef* exposed = state().def.findExposed(name);
    if (!exposed) {
        warnOnce("getFloat:" + name, "AudioMixer.getFloat: no exposed parameter '" + name + "' (exposed: " + joinNames(getParameterNames()) + ")");
        return false;
    }
    return getTarget(exposed->target, value);
}

bool AudioMixer::clearFloat(const std::string& name){
    ensureInitialized();
    const AudioMixerExposedDef* exposed = state().def.findExposed(name);
    if (!exposed) {
        warnOnce("clearFloat:" + name, "AudioMixer.clearFloat: no exposed parameter '" + name + "' (exposed: " + joinNames(getParameterNames()) + ")");
        return false;
    }
    if (exposed->remember) {
        const std::string key = rememberedKey(name);
        UserSettings::removeKey(key.c_str());
        state().rememberDirty.erase(name);
    }
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioCapture::isActive()) {
        AudioCapture::Line line("mixer");
        line.text("action", "clear_float").text("name", name);
    }
#endif
    return clearTarget(exposed->target);
}

bool AudioMixer::hasParameter(const std::string& name){
    ensureInitialized();
    return state().def.findExposed(name) != nullptr;
}

std::vector<std::string> AudioMixer::getParameterNames(){
    ensureInitialized();
    std::vector<std::string> names;
    for (const AudioMixerExposedDef& exposed : state().def.exposed) names.push_back(exposed.name);
    return names;
}

bool AudioMixer::isOverridden(const std::string& name){
    ensureInitialized();
    const AudioMixerExposedDef* exposed = state().def.findExposed(name);
    return exposed && isTargetOverridden(exposed->target);
}

// ---------------------------------------------------------------- snapshots

namespace {
    const char* mixerCaptureCause = nullptr;
}

AudioMixer::CaptureCause::CaptureCause(const char* cause): previous(mixerCaptureCause){
    mixerCaptureCause = cause;
}

AudioMixer::CaptureCause::~CaptureCause(){
    mixerCaptureCause = previous;
}

bool AudioMixer::transitionTo(const std::string& snapshot, float seconds, bool unscaled){
    return transitionToSnapshots({snapshot}, {1.0f}, seconds, unscaled);
}

bool AudioMixer::transitionToSnapshots(const std::vector<std::string>& snapshots, const std::vector<float>& weights,
                                       float seconds, bool unscaled){
    ensureInitialized();
    State& s = state();
    std::vector<const AudioMixerSnapshotDef*> defs;
    for (const std::string& name : snapshots) {
        const AudioMixerSnapshotDef* snapshot = s.def.findSnapshot(name);
        if (!snapshot) {
            warnOnce("snapshot:" + name, "AudioMixer.transitionTo: no snapshot '" + name + "' (snapshots: " + joinNames(getSnapshotNames()) + ")");
            return false;
        }
        defs.push_back(snapshot);
    }
    if (defs.empty()) return false;
    const bool timeUnscaled = resolveUnscaled(unscaled);
    for (auto& [target, param] : s.params) {
        std::vector<float> values;
        for (const AudioMixerSnapshotDef* snapshot : defs) {
            auto it = snapshot->values.find(target);
            values.push_back(it != snapshot->values.end() ? it->second : param.authored);
        }
        startTransition(param, AudioMath::blend(param.curve, values, weights), seconds, timeUnscaled);
    }
    s.currentSnapshot = defs.size() == 1 ? defs[0]->name : "";
    if (!(seconds > 0.0f)) applyAll(0.0);
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioCapture::isActive()) {
        AudioCapture::Line line("mixer");
        line.text("action", "transition");
        if (snapshots.size() == 1) line.text("snapshot", snapshots[0]);
        else line.texts("snapshots", snapshots).numbers("weights", weights, 6);
        line.number("seconds", seconds, 6).flag("unscaled", timeUnscaled);
        if (mixerCaptureCause) line.text("cause", mixerCaptureCause);
    }
#endif
    return true;
}

std::vector<std::string> AudioMixer::getSnapshotNames(){
    ensureInitialized();
    std::vector<std::string> names;
    for (const AudioMixerSnapshotDef& snapshot : state().def.snapshots) names.push_back(snapshot.name);
    return names;
}

std::string AudioMixer::getCurrentSnapshot(){
    return state().currentSnapshot;
}

bool AudioMixer::isTransitioning(){
    for (const auto& [target, param] : state().params) {
        if (param.isMoving()) return true;
    }
    return false;
}

// ---------------------------------------------------------------- groups

std::vector<std::string> AudioMixer::getGroupNames(){
    ensureInitialized();
    std::vector<std::string> names;
    for (const AudioMixerGroupDef* group : state().def.treeOrder()) names.push_back(group->name);
    return names;
}

bool AudioMixer::hasGroup(const std::string& nameOrId){
    ensureInitialized();
    return state().def.findGroup(nameOrId) != nullptr;
}

std::string AudioMixer::findGroupId(const std::string& nameOrId){
    ensureInitialized();
    if (nameOrId.empty()) return "";
    const AudioMixerGroupDef* group = state().def.findGroup(nameOrId);
    return group ? group->id : "";
}

std::string AudioMixer::getGroupName(const std::string& id){
    ensureInitialized();
    const AudioMixerGroupDef* group = state().def.findGroupById(id);
    return group ? group->name : "";
}

bool AudioMixer::getGroupMute(const std::string& group){
    GroupState* found = findState(group);
    return found && found->mute;
}

bool AudioMixer::getGroupSolo(const std::string& group){
    GroupState* found = findState(group);
    return found && found->solo;
}

bool AudioMixer::getGroupBypassEffects(const std::string& group){
    GroupState* found = findState(group);
    return found && found->bypassEffects;
}

bool AudioMixer::setGroupMute(const std::string& group, bool mute){
    ensureInitialized();
    GroupState* found = findState(group);
    if (!found) return false;
    found->mute = mute;
    applyAll(0.0);
    return true;
}

bool AudioMixer::setGroupSolo(const std::string& group, bool solo){
    ensureInitialized();
    GroupState* found = findState(group);
    if (!found) return false;
    found->solo = solo;
    applyAll(0.0);
    return true;
}

bool AudioMixer::setGroupBypassEffects(const std::string& group, bool bypass){
    ensureInitialized();
    GroupState* found = findState(group);
    if (!found) return false;
    found->bypassEffects = bypass;
    writeSlots(*found);
    applyAll(0.0);
    return true;
}

bool AudioMixer::isGroupAudible(const std::string& group){
    ensureInitialized();
    GroupState* found = findState(group);
    return found && isAudible(*found);
}

std::string AudioMixer::findEffectId(const std::string& group, int index){
    ensureInitialized();
    const AudioMixerGroupDef* def = state().def.findGroup(group);
    if (!def || index < 0 || index >= (int)def->effects.size()) return "";
    return def->effects[index].id;
}

std::string AudioMixer::findEffectId(const std::string& group, const std::string& type){
    ensureInitialized();
    const AudioMixerGroupDef* def = state().def.findGroup(group);
    if (!def) return "";
    const AudioEffectType wanted = AudioEffects::typeFromName(type);
    for (const AudioMixerEffectDef& effect : def->effects) {
        if (effect.type == wanted && wanted != AudioEffectType::Unknown) return effect.id;
        if (effect.id == type) return effect.id;
    }
    return "";
}

bool AudioMixer::getEffectBypass(const std::string& effectId){
    EffectState* effect = findEffectState(effectId);
    return effect && effect->bypass;
}

bool AudioMixer::setEffectBypass(const std::string& effectId, bool bypass){
    ensureInitialized();
    GroupState* owner = nullptr;
    EffectState* effect = findEffectState(effectId, &owner);
    if (!effect) return false;
    effect->bypass = bypass;
    writeSlots(*owner);
    applyAll(0.0);
    return true;
}

std::vector<AudioMixer::GroupMeter> AudioMixer::getGroupMeters(){
    ensureInitialized();
    std::vector<GroupMeter> out;
    for (const auto& group : state().groups) {
        if (!group->meter || !group->bus || !group->handle) continue;
        const GroupState* parent = group->parentId.empty() ? nullptr : findState(group->parentId);
        out.push_back({group->id, group->name, parent ? parent->name : std::string(), group->meter.get()});
    }
    return out;
}

float AudioMixer::getGroupFader(const std::string& id){
    GroupState* group = findState(id);
    return group && group->appliedVolume >= 0.0f ? group->appliedVolume : 1.0f;
}

int AudioMixer::getGroupDepth(const std::string& id){
    int depth = 1;
    for (const GroupState* group = id.empty() ? nullptr : findState(id); group && !group->parentId.empty() && depth < 64;
         group = findState(group->parentId)) {
        depth++;
    }
    return depth;
}

unsigned int AudioMixer::getOutputLatencyFrames(const std::string& id){
    unsigned int frames = 0;
    const GroupState* group = id.empty() ? masterState() : findState(id);
    for (int depth = 0; group && depth < 64; depth++) {
        frames += getGroupLatencyFrames(group->id);
        group = group->parentId.empty() ? nullptr : findState(group->parentId);
    }
    return frames;
}

float AudioMixer::getEffectGainReductionDb(const std::string& effectId){
    ensureInitialized();
    GroupState* owner = nullptr;
    EffectState* effect = findEffectState(effectId, &owner);
    if (!effect || effect->type != AudioEffectType::Limiter || !effect->filter || !owner->handle ||
        !state().built || !AudioSystem::isInited()) {
        return 0.0f;
    }
    size_t index = 0;
    while (index < owner->effects.size() && &owner->effects[index] != effect) index++;
    if (index >= EFFECT_SLOTS || owner->slots[index] != effect->filter.get()) return 0.0f;
    return static_cast<LimiterFilter*>(effect->filter.get())->getGainReductionDb();
}

unsigned int AudioMixer::getGroupLatencyFrames(const std::string& group){
    ensureInitialized();
    GroupState* found = group.empty() ? masterState() : findState(group);
    if (!found || found->bypassEffects) return 0;
    unsigned int frames = 0;
    for (const EffectState& effect : found->effects) {
        if (effect.type != AudioEffectType::Limiter || effect.bypass) continue;
        const AudioMixerEffectDef* def = effectDef(effect.id);
        if (!def) continue;
        frames += LimiterFilter::lookaheadFrames(effectAttributes(*def)[LimiterFilter::LOOKAHEAD], state().sampleRate);
    }
    return frames;
}

void AudioMixer::setLoudnessTap(bool on){
    ensureInitialized();
    State& s = state();
    if (on && !s.loudnessTap) {
        s.loudnessTap = std::make_unique<LoudnessTapRing>((size_t)(2.0f * std::max(s.sampleRate, 192000.0f)));
    }
    if (on && !s.loudnessTapOn) s.loudnessTap->clear();
    s.loudnessTapOn = on;
    GroupState* master = masterState();
    if (master && master->meter) master->meter->tap.store(on ? s.loudnessTap.get() : nullptr, std::memory_order_release);
}

bool AudioMixer::isLoudnessTapOn(){
    return state().loudnessTapOn;
}

size_t AudioMixer::drainLoudnessTap(float* interleaved, size_t maxFrames, unsigned int& channels, unsigned int& sampleRate){
    State& s = state();
    channels = LoudnessTapRing::CHANNELS;
    sampleRate = (unsigned int)s.sampleRate;
    if (!s.loudnessTapOn || !s.loudnessTap) return 0;
    // a mixer rebuilt under the tap (a new .mixer, a new bus) has a new meter
    GroupState* master = masterState();
    if (master && master->meter && master->meter->tap.load(std::memory_order_relaxed) != s.loudnessTap.get()) {
        master->meter->tap.store(s.loudnessTap.get(), std::memory_order_release);
    }
    const size_t frames = s.loudnessTap->pop(interleaved, maxFrames);
    if (frames > 0 && master && master->handle && AudioSystem::isInited()) {
        const float volume = AudioSystem::getSoloud().getVolume(master->handle);
        if (volume != 1.0f) {
            for (size_t i = 0; i < frames * channels; i++) interleaved[i] *= volume;
        }
    }
    return frames;
}

float AudioMixer::getMeterDb(const std::string& group){
    ensureInitialized();
    GroupState* found = group.empty() ? masterState() : findState(group);
    if (!found || !state().built || found->handle == 0 || !found->meter || !AudioSystem::isInited()) return AudioMath::minDb;
    const float volume = AudioSystem::getSoloud().getVolume(found->handle);
    return AudioMath::linearToDb(found->meter->getPeak() * volume);
}

// ---------------------------------------------------------------- remembered

std::string AudioMixer::rememberedKey(const std::string& name){
    return "audio.mixer." + name;
}

void AudioMixer::saveRemembered(){
    ensureInitialized();
    State& s = state();
    for (const AudioMixerExposedDef& exposed : s.def.exposed) {
        if (!exposed.remember) continue;
        auto it = s.params.find(exposed.target);
        if (it == s.params.end()) continue;
        const std::string key = rememberedKey(exposed.name);
        // what a script set (as the end-of-frame writes); the others follow their snapshots, so
        // nothing of them is kept
        if (it->second.overridden) UserSettings::setFloatForKey(key.c_str(), it->second.finalValue());
        else UserSettings::removeKey(key.c_str());
    }
    s.rememberDirty.clear();
    UserSettings::save();
}

void AudioMixer::loadRemembered(){
    ensureInitialized();
    State& s = state();
    for (const AudioMixerExposedDef& exposed : s.def.exposed) {
        if (!exposed.remember) continue;
        const std::string key = rememberedKey(exposed.name);
        if (!UserSettings::hasKey(key.c_str())) continue;
        const float value = UserSettings::getFloatForKey(key.c_str(), 0.0f);
        if (std::isfinite(value)) setTarget(exposed.target, value, 0.0f, false);
    }
    s.rememberDirty.clear();
}

// ---------------------------------------------------------------- frame

void AudioMixer::update(double scaledDt, double unscaledDt){
    ensureInitialized();
    State& s = state();
    s.frameUnscaled = unscaledDt;
    if (unscaledDt > 0.0) s.clock += unscaledDt;
    advance(scaledDt, unscaledDt);
    applyAll(unscaledDt);

    // what scripts set on remembered parameters, at the end of their frame
    if (!s.rememberDirty.empty()) {
        for (const std::string& name : s.rememberDirty) {
            const AudioMixerExposedDef* exposed = s.def.findExposed(name);
            if (!exposed) continue;
            auto it = s.params.find(exposed->target);
            if (it == s.params.end() || !it->second.overridden) continue;
            const std::string key = rememberedKey(name);
            UserSettings::setFloatForKey(key.c_str(), it->second.finalValue());
        }
        s.rememberDirty.clear();
    }

    AudioSystem::pruneVoices();
    AudioSystem::mixManually(unscaledDt);
}

double AudioMixer::getFrameUnscaledDelta(){
    return state().frameUnscaled;
}

double AudioMixer::getClockTime(){
    return state().clock;
}

float AudioMixer::dbToLinear(float db){
    return AudioMath::dbToLinear(db);
}

float AudioMixer::linearToDb(float linear){
    return AudioMath::linearToDb(linear);
}

// ---------------------------------------------------------------- engine-internal

void AudioMixer::onAudioInit(){
    ensureInitialized();
    State& s = state();
    if (!AudioSystem::isInited()) return;
    s.sampleRate = (float)std::max(8000u, AudioSystem::getSoloud().getBackendSamplerate());
    if (!s.built) {
        s.built = true;
        // filters made before (no buses yet) are made again with the device's sample rate
        for (auto& group : s.groups) {
            for (EffectState& effect : group->effects) {
                effect.filter.reset();
                effect.applied.clear();
            }
        }
        // the runtime switches (a script's mute before the first sound) stay as they are
        syncGraph(s.def, false);
    } else {
        updateVoiceLimit();
    }
}

void AudioMixer::onAudioShutdown(){
    State& s = state();
    // children first, so no bus outlives its parent
    for (auto it = s.groups.rbegin(); it != s.groups.rend(); ++it) {
        GroupState& group = **it;
        group.bus.reset();
        group.handle = 0;
        for (SoLoud::Filter*& slot : group.slots) slot = nullptr;
        for (EffectState& effect : group.effects) {
            effect.filter.reset();
            effect.applied.clear();
            effect.sidechainHandle = 0;
        }
    }
    s.built = false;
}

SoLoud::Bus* AudioMixer::getBus(const std::string& groupIdOrName){
    ensureInitialized();
    if (!state().built) return nullptr;
    GroupState* group = groupIdOrName.empty() ? nullptr : findState(groupIdOrName);
    if (!group || !group->bus || group->handle == 0) group = masterState();
    return group && group->handle ? group->bus.get() : nullptr;
}

unsigned int AudioMixer::getBusHandle(const std::string& groupIdOrName){
    ensureInitialized();
    if (!state().built) return 0;
    GroupState* group = groupIdOrName.empty() ? nullptr : findState(groupIdOrName);
    if (!group || group->handle == 0) group = masterState();
    return group ? group->handle : 0;
}

unsigned int AudioMixer::play(const std::string& groupId, SoLoud::AudioSource& sample, bool is3D,
                              const Vector3& position, float volume, bool clocked, double clockTime){
    if (!AudioSystem::isInited()) return 0;
    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    SoLoud::Bus* bus = getBus(groupId);
    if (clocked) {
        if (bus) {
            return is3D ? bus->play3dClocked(clockTime, sample, position.x, position.y, position.z, 0, 0, 0, volume)
                        : bus->playClocked(clockTime, sample, volume, 0.0f);
        }
        return is3D ? soloud.play3dClocked(clockTime, sample, position.x, position.y, position.z, 0, 0, 0, volume)
                    : soloud.playClocked(clockTime, sample, volume, 0.0f);
    }
    if (bus) {
        return is3D ? bus->play3d(sample, position.x, position.y, position.z, 0, 0, 0, volume, true)
                    : bus->play(sample, volume, 0.0f, true);
    }
    return is3D ? soloud.play3d(sample, position.x, position.y, position.z, 0, 0, 0, volume, true)
                : soloud.play(sample, volume, 0.0f, true);
}

unsigned int AudioMixer::getBusVoiceCount(){
    unsigned int count = 0;
    for (auto& group : state().groups) {
        if (group->handle) count++;
    }
    return count;
}

bool AudioMixer::isBusHandle(unsigned int handle){
    if (handle == 0) return false;
    for (auto& group : state().groups) {
        if (group->handle == handle) return true;
    }
    return false;
}

namespace {

    void applyMasterPause() {
        State& s = state();
        if (!AudioSystem::isInited()) return;
        if (GroupState* master = masterState()) {
            if (master->handle) AudioSystem::getSoloud().setPause(master->handle, s.masterPaused || s.platformPaused);
        }
    }

}

void AudioMixer::setMasterPaused(bool paused){
    ensureInitialized();
    state().masterPaused = paused;
    applyMasterPause();
}

void AudioMixer::setPlatformPaused(bool paused){
    ensureInitialized();
    state().platformPaused = paused;
    applyMasterPause();
}

bool AudioMixer::isPlatformPaused(){
    return state().platformPaused;
}

void AudioMixer::getDebugCounters(unsigned int& voiceLimitChanges, unsigned int& filterBuilds){
    voiceLimitChanges = state().voiceLimitChanges;
    filterBuilds = state().filterBuilds;
}

bool AudioMixer::isMasterPaused(){
    return state().masterPaused;
}

// ---------------------------------------------------------------- AudioMixerGroup / AudioMixerEffect

AudioMixerGroup::AudioMixerGroup(std::string id): id(std::move(id)) {
}

bool AudioMixerGroup::isValid() const{
    return !id.empty() && AudioMixer::getDef().findGroupById(id) != nullptr;
}

std::string AudioMixerGroup::getName() const{
    return AudioMixer::getGroupName(id);
}

float AudioMixerGroup::getVolume() const{
    float value = 0.0f;
    AudioMixer::getTarget(AudioMixerIO::volumeTarget(id), value);
    return value;
}

void AudioMixerGroup::setVolume(float db){
    AudioMixer::setTarget(AudioMixerIO::volumeTarget(id), db);
}

void AudioMixerGroup::setVolumeOver(float db, float seconds, bool unscaled){
    AudioMixer::setTarget(AudioMixerIO::volumeTarget(id), db, seconds, unscaled);
}

bool AudioMixerGroup::getMute() const{
    return AudioMixer::getGroupMute(id);
}

void AudioMixerGroup::setMute(bool mute){
    AudioMixer::setGroupMute(id, mute);
}

bool AudioMixerGroup::getSolo() const{
    return AudioMixer::getGroupSolo(id);
}

void AudioMixerGroup::setSolo(bool solo){
    AudioMixer::setGroupSolo(id, solo);
}

bool AudioMixerGroup::getBypassEffects() const{
    return AudioMixer::getGroupBypassEffects(id);
}

void AudioMixerGroup::setBypassEffects(bool bypass){
    AudioMixer::setGroupBypassEffects(id, bypass);
}

int AudioMixerGroup::getEffectCount() const{
    const AudioMixerGroupDef* group = AudioMixer::getDef().findGroupById(id);
    return group ? (int)group->effects.size() : 0;
}

float AudioMixerGroup::getMeter() const{
    return AudioMixer::getMeterDb(id);
}

AudioMixerEffect::AudioMixerEffect(std::string id): id(std::move(id)) {
}

bool AudioMixerEffect::isValid() const{
    return !id.empty() && AudioMixer::getDef().findEffect(id) != nullptr;
}

std::string AudioMixerEffect::getType() const{
    size_t index = 0;
    const AudioMixerGroupDef* group = AudioMixer::getDef().findEffect(id, &index);
    if (!group) return "";
    const AudioMixerEffectDef& effect = group->effects[index];
    return effect.type == AudioEffectType::Unknown ? effect.typeName : AudioEffects::typeName(effect.type);
}

bool AudioMixerEffect::getBypass() const{
    return AudioMixer::getEffectBypass(id);
}

void AudioMixerEffect::setBypass(bool bypass){
    AudioMixer::setEffectBypass(id, bypass);
}

float AudioMixerEffect::getWet() const{
    float value = 1.0f;
    getParameter("wet", value);
    return value;
}

void AudioMixerEffect::setWet(float wet){
    setParameter("wet", wet);
}

bool AudioMixerEffect::hasParameter(const std::string& name) const{
    AudioMixerTarget target;
    return AudioMixerIO::resolveTarget(AudioMixer::getDef(), AudioMixerIO::paramTarget(id, name), target);
}

bool AudioMixerEffect::setParameter(const std::string& name, float value, float seconds, bool unscaled){
    size_t index = 0;
    const AudioMixerGroupDef* group = AudioMixer::getDef().findEffect(id, &index);
    if (!group) return false;
    const AudioEffectParamInfo* param = AudioEffects::findParam(group->effects[index].type, name);
    if (!param) {
        warnOnce("param:" + id + "/" + name, "AudioMixerEffect.setParameter: " + getType() + " has no parameter '" + name +
                 "' (" + AudioEffects::paramNames(group->effects[index].type) + ")");
        return false;
    }
    return AudioMixer::setTarget(AudioMixerIO::paramTarget(id, param->name), value, seconds, unscaled);
}

bool AudioMixerEffect::getParameter(const std::string& name, float& value) const{
    size_t index = 0;
    const AudioMixerGroupDef* group = AudioMixer::getDef().findEffect(id, &index);
    if (!group) return false;
    const AudioEffectParamInfo* param = AudioEffects::findParam(group->effects[index].type, name);
    if (!param) return false;
    return AudioMixer::getTarget(AudioMixerIO::paramTarget(id, param->name), value);
}
