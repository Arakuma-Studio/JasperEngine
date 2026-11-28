// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef AudioSourceEffect_h
#define AudioSourceEffect_h

#include "Export.h"
#include "ecs/Entity.h"

#include <cstddef>
#include <string>

namespace jasperengine {

    class Scene;
    struct AudioSourceComponent;

    // An effect of an Audio Source's own list for scripts (AudioSource:getEffect,
    // docs/specs/S97-audio-gaps.md REQ-1, REQ-3), as AudioMixerEffect is a group's: valid while
    // the source's list holds an effect of its type at its place. Values are the component's, the
    // value heard now; a timed setParameter moves them.
    class JASPERENGINE_API AudioSourceEffect {
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        size_t index = 0;
        std::string type;

        AudioSourceComponent* component() const;
    public:
        AudioSourceEffect() = default;
        AudioSourceEffect(Scene* scene, Entity entity, size_t index, std::string type);

        bool isValid() const;
        std::string getType() const;        // "lowpass", ...
        // 1-based, as getEffect takes it
        int getIndex() const;
        bool getBypass() const;
        void setBypass(bool bypass);
        float getWet() const;
        void setWet(float wet);
        // false when the effect has no such parameter ("wet" is one); seconds > 0 moves there on
        // scaled time (unscaled: unscaled time), with the mixer's curves
        bool setParameter(const std::string& name, float value, float seconds = 0.0f, bool unscaled = false);
        bool getParameter(const std::string& name, float& value) const;
        bool hasParameter(const std::string& name) const;
    };

}

#endif /* AudioSourceEffect_h */
