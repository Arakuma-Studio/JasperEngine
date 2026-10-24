// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.4, 9.R17/R18/R42/R43)
// - Purpose: the AnimationPlayer object (Lua `AnimationPlayer`, `object:getAnimationPlayer()`),
//   Unity's legacy Animation API over an AnimationPlayerComponent, and AnimationState, the
//   handle getState() returns. The C++ side of the editor (MCP, Inspector live rows, Model
//   integration) uses the same calls.
// - Owner: T0 (API, the constructor); T1b implements it, binds it to Lua and adds
//   Model:playAnimation / GameObject getAnimationPlayer integration.
// - Thread-safety: main thread (ECS).
// - Semantics (T1b implements, tests cover):
//   * play(name): instant, stops the other states (rewinding them) and plays name from 0 at
//     weight 1 (from its length when the effective speed is negative); play(name, fade) with
//     fade > 0 = crossFade(name, fade); crossFade(name) uses defaultFadeTime.
//   * blend(name, weight, fade) moves one state's weight without touching the others.
//   * queue(name[, fade]) plays after the playing states finish (R17); play/crossFade/stop clear
//     the queue; queue with nothing playing = play.
//   * stop() / stop(name) rewind the stopped states and keep the pose; rewind() / rewind(name)
//     set time 0; sample() applies the current times once (e.g. while paused).
//   * calls made before the clips are loaded are remembered and applied on load; unknown state
//     names log a warning once and do nothing.
//   * calls from inside onEvent / onFinish take effect on the next frame.

#ifndef ANIMATIONPLAYER_H
#define ANIMATIONPLAYER_H

#include "EntityHandle.h"
#include "animation/AnimationPlayerRuntime.h"
#include "component/AnimationPlayerComponent.h"

#include <string>

namespace jasperengine{

    // One state of an Animation Player, by (entity, state name): re-resolved on every access,
    // so it stays usable across clip list changes (an invalid handle reads defaults, writes
    // nothing)
    class JASPERENGINE_API AnimationState{
    private:
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        std::string name;

    public:
        AnimationState();
        AnimationState(Scene* scene, Entity entity, const std::string& name);

        bool isValid() const;
        std::string getName() const;
        // the entry's clip reference (read-only)
        std::string getClip() const;

        void setTime(float time);
        float getTime() const;
        void setNormalizedTime(float normalizedTime);
        float getNormalizedTime() const;
        void setSpeed(float speed);
        float getSpeed() const;
        void setWeight(float weight);
        float getWeight() const;
        float getLength() const;
        // "once", "loop", "ping_pong", "clamp_forever" ("default" sets back the entry's)
        void setWrap(const std::string& wrap);
        std::string getWrap() const;
        void setEnabled(bool enabled);
        bool isEnabled() const;
    };

    class JASPERENGINE_API AnimationPlayer: public EntityHandle{
    public:
        // adds the Animation Player component to the entity when it has none
        AnimationPlayer(Scene* scene, Entity entity);
        virtual ~AnimationPlayer();

        AnimationPlayer(const AnimationPlayer& rhs);
        AnimationPlayer& operator=(const AnimationPlayer& rhs);
        AnimationPlayer(AnimationPlayer&& rhs) noexcept;
        AnimationPlayer& operator=(AnimationPlayer&& rhs) noexcept;

        void play(const std::string& name);
        void play(const std::string& name, float fade);
        void crossFade(const std::string& name);
        void crossFade(const std::string& name, float fade);
        void blend(const std::string& name, float weight);
        void blend(const std::string& name, float weight, float fade);
        void queue(const std::string& name);
        void queue(const std::string& name, float fade);
        void stop();
        void stop(const std::string& name);
        void rewind();
        void rewind(const std::string& name);
        void sample();
        bool isPlaying() const;
        bool isPlaying(const std::string& name) const;

        AnimationState getState(const std::string& name) const;
        // the runtime view of a state (default-constructed when unknown)
        anim::AnimationStateInfo getStateInfo(const std::string& name) const;

        // clip list (an authored change: the next update reloads)
        // path: asset-relative .anim or "<model>#<clip>"; name empty = default name. False when
        // the state name is taken.
        bool addClip(const std::string& path);
        bool addClip(const std::string& path, const std::string& name);
        bool removeClip(const std::string& name);
        int getClipCount() const;
        // 0-based state name ("" out of range)
        std::string getClipName(int index) const;

        void setSpeed(float speed);
        float getSpeed() const;
        void setDefaultClip(const std::string& name);
        std::string getDefaultClip() const;
        void setDefaultFadeTime(float fade);
        float getDefaultFadeTime() const;
        void setPlayAutomatically(bool playAutomatically);
        bool isPlayAutomatically() const;
        void setEnabled(bool enabled);
        bool isEnabled() const;

        // every clip of the list is loaded and bound
        bool isLoaded() const;

        FunctionSubscribe<void(const AnimationEvent&)>& getOnEvent();
        FunctionSubscribe<void(const std::string&)>& getOnFinish();
    };

}

#endif //ANIMATIONPLAYER_H
