// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// An Audio Source's own effects (docs/specs/S97-audio-gaps.md REQ-1 to REQ-4, D1, D2).
// Engine-internal: it includes SoLoud.
#ifndef SourceEffects_h
#define SourceEffects_h

#include "Export.h"
#include "audio/AudioEffects.h"
#include "component/AudioSourceComponent.h"

#include "soloud.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    struct SourceEffectChain;

    // What a source's voices play when its effects are on: a SoLoud source that wraps the shared
    // sample of the pool. Its createInstance is the sample's, and its mFilter[] holds the source's
    // filters, so Soloud::play gives each voice its own filter instances. Filters are never set on
    // the pool's sample: every source of that file shares it (D2).
    class SourceEffectSound : public SoLoud::AudioSource {
    public:
        SourceEffectSound(std::shared_ptr<SoLoud::AudioSource> sample, SourceEffectChain& chain);
        ~SourceEffectSound() override;

        SoLoud::AudioSourceInstance* createInstance() override;
        const std::shared_ptr<SoLoud::AudioSource>& getSample() const { return sample; }

        // the voices it started that may still play
        std::vector<unsigned int> handles;
        // per filter slot, the attribute values the voices were last given
        std::vector<std::vector<float>> applied;

    private:
        std::shared_ptr<SoLoud::AudioSource> sample;
        // what Soloud::play reads of its source: the sample's, now
        void copySampleState();
    };

    // The filters of one structure of a source's list (its known per-source types in order): the
    // filters voices make their instances from, and a wrapper per sample its voices play. A voice
    // keeps the chain it started with when the structure changes (REQ-2).
    struct SourceEffectChain {
        std::vector<AudioEffectType> types;
        std::vector<std::unique_ptr<SoLoud::Filter>> filters;
        // after the filters: destroyed first, stopping their voices before the filters go
        std::vector<std::unique_ptr<SourceEffectSound>> sounds;

        bool hasVoices() const;
    };

    // A timed change of an effect's value (REQ-3)
    struct SourceEffectMove {
        size_t index = 0;
        std::string type;
        std::string param;          // a parameter's name, or "wet"
        AudioParamCurve curve = AudioParamCurve::Linear;
        float from = 0.0f;
        float to = 0.0f;
        float written = 0.0f;       // the value it last stored (another write cancels it)
        double elapsed = 0.0;
        double duration = 0.0;
        bool unscaled = false;
    };

    // The runtime of a source's effects (AudioSourceComponent::effectRuntime): made on its first
    // filtered voice or timed change, dropped once nothing of it is left
    struct SourceEffectRuntime {
        std::shared_ptr<SourceEffectChain> chain;
        // chains of an earlier structure whose voices still play
        std::vector<std::shared_ptr<SourceEffectChain>> retired;
        std::vector<SourceEffectMove> moves;
    };

    class JASPERENGINE_API SourceEffects {
    public:
        // Whether a voice of a sample at `sampleRate` plays through the effects: one known
        // per-source effect is on (not bypassed, not at its A5 off value). REQ-4: otherwise the
        // voice plays the sample directly.
        static bool isActive(const AudioSourceComponent& audio, float sampleRate);
        // The sound a new voice of the source plays for `sample`: the source's wrapper of it, or
        // null to play the sample itself
        static SoLoud::AudioSource* wrap(AudioSourceComponent& audio, const std::shared_ptr<SoLoud::AudioSource>& sample);
        // a voice started from what wrap returned: it gets the effects' values before its first mix
        static void started(AudioSourceComponent& audio, SoLoud::AudioSource& sound, unsigned int handle);
        // Once a frame, for a source with effects or their runtime: timed changes move, value
        // changes reach the live voices of the current structure, ended voices are let go
        static void update(AudioSourceComponent& audio, double scaledDt, double unscaledDt);

        // An effect's value (a parameter or "wet") moves to `value` over `seconds` of scaled time
        // (unscaled: unscaled time); 0 sets it at once. False when there is no such effect (index
        // 0-based) or parameter. The component holds the value heard now.
        static bool setParameter(AudioSourceComponent& audio, size_t index, const std::string& name, float value,
                                 float seconds = 0.0f, bool unscaled = false);
        // the value of an effect's parameter ("wet" too): what it sets, else its default
        static bool getParameter(const AudioSourceComponent& audio, size_t index, const std::string& name, float& value);
        static float paramValue(const AudioSourceEffectDef& effect, AudioEffectType type, const AudioEffectParamInfo& param);

        // tests: the sample a wrapper plays (null when `sound` is not one)
        static SoLoud::AudioSource* sampleOf(SoLoud::AudioSource* sound);
    };

}

#endif /* SourceEffects_h */
