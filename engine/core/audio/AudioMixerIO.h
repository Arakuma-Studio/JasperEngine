// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef AudioMixerIO_h
#define AudioMixerIO_h

#include "Export.h"
#include "audio/AudioEffects.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine {

    // An Audio Mixer asset (.mixer, JSON {"format": "jasperengine.mixer"}; audio-mixer spec 7.4):
    // a tree of groups under Master, their effects, the exposed parameters and the snapshots.
    // A parameter is addressed by a target: "<groupId>/volume" or "<effectId>/<param>" (a
    // parameter of the effect's table, AudioEffects, or "wet").

    struct AudioMixerEffectDef {
        std::string id;                       // 8 hex characters, unique in the mixer
        AudioEffectType type = AudioEffectType::Unknown;
        std::string typeName;                 // as written in the file (an Unknown type keeps it)
        bool bypass = false;
        float wet = 1.0f;
        std::map<std::string, float> params;  // the ones the file sets; a missing one is its default
        std::string sidechainGroup;           // duck: the name of the group it listens to
        std::string unknownJson;              // an Unknown type: the effect object as read (written back)
    };

    struct AudioMixerGroupDef {
        std::string id;
        std::string name;
        std::string parentId;                 // "" only for Master
        float volumeDb = 0.0f;                // -80..+20; -80 is silence
        bool mute = false;
        bool solo = false;
        bool bypassEffects = false;
        std::vector<AudioMixerEffectDef> effects;
    };

    struct AudioMixerExposedDef {
        std::string name;                     // what AudioMixer.setFloat takes, unique in the mixer
        std::string target;
        bool remember = false;                // values set by scripts are kept in UserSettings
    };

    struct AudioMixerSnapshotDef {
        std::string name;
        std::map<std::string, float> values;  // target -> value; a missing target keeps the group / effect value
    };

    struct JASPERENGINE_API AudioMixerDef {
        int version = 1;
        bool unscaled = false;                // updateMode "unscaled": transitions and timed setFloat ignore Engine.timeScale
        std::vector<AudioMixerGroupDef> groups;
        std::vector<AudioMixerExposedDef> exposed;
        std::vector<AudioMixerSnapshotDef> snapshots;
        std::string startSnapshot;

        // lookups (null when missing)
        const AudioMixerGroupDef* findGroup(const std::string& idOrName) const;
        AudioMixerGroupDef* findGroup(const std::string& idOrName);
        const AudioMixerGroupDef* findGroupById(const std::string& id) const;
        const AudioMixerGroupDef* findGroupByName(const std::string& name) const;
        const AudioMixerGroupDef* master() const;
        // the group holding an effect (and its index there)
        const AudioMixerGroupDef* findEffect(const std::string& effectId, size_t* index = nullptr) const;
        const AudioMixerExposedDef* findExposed(const std::string& name) const;
        const AudioMixerSnapshotDef* findSnapshot(const std::string& name) const;
        // the children of a group, in file order
        std::vector<const AudioMixerGroupDef*> children(const std::string& groupId) const;
        // true when `groupId` is `ancestorId` or below it
        bool isUnder(const std::string& groupId, const std::string& ancestorId) const;
        // groups ordered so that a parent comes before its children (Master first)
        std::vector<const AudioMixerGroupDef*> treeOrder() const;
    };

    // What a target addresses
    struct AudioMixerTarget {
        const AudioMixerGroupDef* group = nullptr;
        const AudioMixerEffectDef* effect = nullptr;   // null for a group volume
        size_t effectIndex = 0;
        const AudioEffectParamInfo* param = nullptr;   // null for a group volume
        bool volume = false;
    };

    namespace AudioMixerIO {
        constexpr const char* FORMAT = "jasperengine.mixer";
        constexpr const char* EXTENSION = ".mixer";

        // The file -> def. False + `error` ("groups[2].parent: ..."): not JSON, not a mixer, no
        // Master or two, duplicate ids or names, a target that does not resolve. `warnings` gets
        // the unknown effect types (kept, inactive) and the values clamped into range.
        JASPERENGINE_API bool parse(const std::string& json, AudioMixerDef& def, std::string* error,
                                    std::vector<std::string>* warnings = nullptr);
        // The def -> the file: keys in the order of spec 7.4, 2-space indent, defaults left out
        JASPERENGINE_API std::string write(const AudioMixerDef& def);
        // The rules parse checks, on a def built in code (the editor, MCP): "" when valid
        JASPERENGINE_API std::string validate(const AudioMixerDef& def);

        // A new mixer: Master only
        JASPERENGINE_API AudioMixerDef createDefault();
        // 8 hex characters no group or effect of `def` uses
        JASPERENGINE_API std::string newId(const AudioMixerDef& def);

        JASPERENGINE_API std::string volumeTarget(const std::string& groupId);
        JASPERENGINE_API std::string paramTarget(const std::string& effectId, const std::string& param);
        // false when the target does not resolve in `def`
        JASPERENGINE_API bool resolveTarget(const AudioMixerDef& def, const std::string& target, AudioMixerTarget& out);
        // the value the def gives a target (the group volume, the effect's parameter or its default)
        JASPERENGINE_API bool authoredValue(const AudioMixerDef& def, const std::string& target, float& value);
        // sets it in the def (clamped); false when the target does not resolve
        JASPERENGINE_API bool setAuthoredValue(AudioMixerDef& def, const std::string& target, float value);
        JASPERENGINE_API AudioParamCurve curveOf(const AudioMixerDef& def, const std::string& target);
        // clamp into the target's range (volume -80..+20)
        JASPERENGINE_API float clampValue(const AudioMixerDef& def, const std::string& target, float value);
        // every target of the mixer (group volumes, then every parameter of every known effect, wet included)
        JASPERENGINE_API std::vector<std::string> allTargets(const AudioMixerDef& def);
        // "Music volume", "Music > Low Pass cutoff": a target for people
        JASPERENGINE_API std::string describeTarget(const AudioMixerDef& def, const std::string& target);
        // the effect's parameter value in the def (its default when the file leaves it out)
        JASPERENGINE_API float effectParam(const AudioMixerEffectDef& effect, const AudioEffectParamInfo& param);
    }

    namespace AudioMath {
        constexpr float minDb = -80.0f;
        constexpr float maxDb = 20.0f;
        // <= -80 dB is silence (0); 0 dB = 1
        JASPERENGINE_API float dbToLinear(float db);
        // 0 (and below) = -80 dB
        JASPERENGINE_API float linearToDb(float linear);
        // `t` (0..1) of the way from `a` to `b` along the curve (dB and plain values linearly,
        // frequencies along log2)
        JASPERENGINE_API float interpolate(AudioParamCurve curve, float a, float b, float t);
        // the weighted mean along the curve; `weights` summing to 0 gives the first value
        JASPERENGINE_API float blend(AudioParamCurve curve, const std::vector<float>& values, const std::vector<float>& weights);
    }

}

#endif /* AudioMixerIO_h */
