// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.4, 9.R17/R18/R42/R43)
// - Purpose: the AnimationPlayer object (Lua `AnimationPlayer`, `object:getAnimationPlayer()`),
//   Unity's legacy Animation API over an AnimationPlayerComponent, and AnimationState, the
//   handle getState() returns. The C++ side of the editor (MCP, Inspector live rows, Model
//   integration) uses the same calls.
// - The constructor ADDS the component when the entity has none (PoseAnimator precedent). The
//   Lua `object:getAnimationPlayer()` does not construct one blindly: it returns nil when
//   the GameObject has no Animation Player (Unity), and `hasAnimationPlayer()` tells.
// - Thread-safety: main thread (ECS).
// - Semantics (tests cover them):
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
//   * layers (anim layers spec 8): play / crossFade stop the states of the target's layer
//     (PlayMode::StopSameLayer, Unity's default) or of every layer (StopAll); queue waits for the
//     target's layer. A layer is a 0-based index or a name: an unknown one warns once and does
//     nothing (getters return 0, "" or -1). Layer weight, mask and blend setters are runtime
//     (not saved); addLayer / removeLayer are authored (the next update reloads).
//   * humanoid clips (anim-retarget 8): addClip(path, name, options) with options.retarget plays
//     a generic clip retargeted from its source model; AnimationState mirror / footIK override
//     the entry's flags at runtime (not saved); isHumanoid tells a humanoid or retargeted state.
//   * upstream's Animation Player with an Animator Controller (docs/specs/S21-animation.md
//     "Upstream absorption"): the controller plays on the GameObject's Animator (S21), which
//     the scene migration adds for upstream's controller players, leaving the player disabled.
//     A player that is not enabled on a GameObject with an Animator hands its playback calls to
//     the Animator: play(name) jumps to "<Layer>.<State>" or "<State>" instantly (playInLayer:
//     a layer and normalized time), play / crossFade with a fade is a transition in seconds
//     (crossFadeInFixedTime; crossFadeInLayer on a layer), blend and queue warn once and do
//     nothing, stop() disables the Animator until the next play (setEnabled(true) restarts it
//     from its default states), isPlaying names the Animator's current or next states,
//     isEnabled / setEnabled, isLoaded and the layer calls (count, names, weights with fades)
//     are the Animator's. setController sets the Animator's controller (adding the Animator)
//     and hands over; setController("") gives the clip list back.
//     The parameter and state calls (setFloat... getCurrentStateInfo, isInTransition) always
//     go to the GameObject's Animator (without one they warn once and return defaults);
//     setFloat(name, v, dampTime) damps toward v once per update until another set. The
//     onStateEnter / onStateExit callbacks get the Animator's states (layer index, state name),
//     after the update's events; a handing-over player also gets its events (onEvent) and
//     calls its scripts' onStateEnter / onStateExit methods.

#ifndef ANIMATIONPLAYER_H
#define ANIMATIONPLAYER_H

#include "EntityHandle.h"
#include "component/AnimationPlayerComponent.h"

#include <string>
#include <vector>

namespace jasperengine{

    // What a state looks like from outside (Inspector live rows, MCP, Lua getState)
    struct JASPERENGINE_API AnimationStateInfo{
        std::string name;
        std::string clip;               // the entry's clip reference
        bool loaded = false;            // the clip is loaded (false while loading or after a failure)
        std::string error;              // the load error, when it failed
        float time = 0.0f;              // seconds in the clip (wrapped for loop / ping_pong)
        float normalizedTime = 0.0f;    // time / length
        float length = 0.0f;
        float speed = 1.0f;             // the state's own (the component speed multiplies it)
        float weight = 0.0f;
        float targetWeight = 0.0f;
        ClipWrapMode wrap = ClipWrapMode::Once;     // effective (state override, entry, clip)
        bool enabled = false;           // Unity AnimationState.enabled: advancing and sampled
        bool playing = false;
        bool queued = false;
        size_t boundTracks = 0;         // tracks whose path resolved
        size_t trackCount = 0;
        // humanoid playback (anim-retarget): a humanoid clip, or a generic one retargeted; its
        // effective Mirror / Foot IK; the bones it drives on the root's rig (0: no humanoid rig)
        bool humanoid = false;
        bool retarget = false;
        bool mirror = false;
        bool footIK = false;
        size_t humanoidBones = 0;
    };

    // AnimationPlayer:addClip options (anim-retarget 8; layer: anim layers spec 8)
    struct JASPERENGINE_API AnimationClipOptions{
        bool retarget = false;          // play a generic clip as a humanoid clip, retargeted from its source model
        bool mirror = false;
        bool footIK = false;
        int layer = 0;
    };

    // upstream's AnimatorStateInfo of the Animation Player's controller (named ControllerStateInfo
    // here: AnimatorStateInfo is the Animator's, object/Animator.h): a copy of the GameObject's
    // Animator layer's current (or next) state
    struct JASPERENGINE_API ControllerStateInfo{
        bool isValid = false;
        std::string name;               // the state name (without the layer)
        std::string fullName;           // "<Layer>.<State>" (sub-state machines in between: "<Layer>.<Machine>.<State>")
        float normalizedTime = 0.0f;    // whole loops counted
        float length = 0.0f;            // seconds
        float speed = 1.0f;
        bool loop = false;
    };

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

        // layers (anim layers spec 8): a runtime move to another layer (keeps time and weight)
        void setLayer(int layer);
        int getLayer() const;
        // Unity AddMixingTransform: with any, the state animates only these paths (and their
        // descendants when recursive), multiplied with the layer's mask
        void addMixingTransform(const std::string& path);
        void addMixingTransform(const std::string& path, bool recursive);
        bool removeMixingTransform(const std::string& path);
        void clearMixingTransforms();

        void setMirror(bool mirror);
        bool getMirror() const;
        void setFootIK(bool footIK);
        bool getFootIK() const;
        void resetMirror();
        void resetFootIK();
        bool isHumanoid() const;
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
        void play(const std::string& name, float fade, PlayMode mode);
        void crossFade(const std::string& name);
        void crossFade(const std::string& name, float fade);
        void crossFade(const std::string& name, float fade, PlayMode mode);
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
        AnimationStateInfo getStateInfo(const std::string& name) const;
        // the property tracks a state's clip binds now, as "path:component.property" (debugging;
        // empty while its clip loads or for an unknown state)
        std::vector<std::string> getAnimatedProperties(const std::string& name) const;

        // clip list (an authored change: the next update reloads)
        // path: asset-relative .anim or "<model>#<clip>"; name empty = default name. False when
        // the state name is taken.
        bool addClip(const std::string& path);
        bool addClip(const std::string& path, const std::string& name);
        bool addClip(const std::string& path, const std::string& name, const AnimationClipOptions& options);
        bool removeClip(const std::string& name);
        int getClipCount() const;
        // 0-based state name ("" out of range)
        std::string getClipName(int index) const;

        void setSpeed(float speed);
        float getSpeed() const;
        // S48 Pose Frame Rate (fps, 0 = smooth): poses held on whole steps from each key
        void setPoseFrameRate(float fps);
        float getPoseFrameRate() const;
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

        // ---- layers (anim layers spec 8) ----
        // >= 1 (the base layer is index 0)
        int getLayerCount() const;
        // "" / -1 when unknown
        std::string getLayerName(int index) const;
        int getLayerIndex(const std::string& name) const;
        // authored: appends a layer; its index, or -1 when the name is taken. blend "override"
        // (default) or "additive"; mask an asset-relative .mask ("" = none)
        int addLayer(const std::string& name);
        int addLayer(const std::string& name, float weight);
        int addLayer(const std::string& name, float weight, const std::string& blend);
        int addLayer(const std::string& name, float weight, const std::string& blend, const std::string& mask);
        // authored: not the base layer; its clip entries move to layer 0
        bool removeLayer(int layer);
        bool removeLayer(const std::string& layer);
        // runtime weight in [0, 1] (fades linearly over `fade` seconds); the getter returns the
        // current (possibly fading) value; layer 0 is always 1
        void setLayerWeight(int layer, float weight);
        void setLayerWeight(int layer, float weight, float fade);
        void setLayerWeight(const std::string& layer, float weight);
        void setLayerWeight(const std::string& layer, float weight, float fade);
        float getLayerWeight(int layer) const;
        float getLayerWeight(const std::string& layer) const;
        // runtime mask (an asset-relative .mask; "" clears it)
        void setLayerMask(int layer, const std::string& mask);
        void setLayerMask(const std::string& layer, const std::string& mask);
        std::string getLayerMask(int layer) const;
        std::string getLayerMask(const std::string& layer) const;
        // runtime blend: "override" / "additive"
        void setLayerBlendMode(int layer, const std::string& mode);
        void setLayerBlendMode(const std::string& layer, const std::string& mode);
        std::string getLayerBlendMode(int layer) const;
        std::string getLayerBlendMode(const std::string& layer) const;
        // every state of the layer fades out (instantly without a fade); its queue clears
        void stopLayer(int layer);
        void stopLayer(int layer, float fade);
        void stopLayer(const std::string& layer);
        void stopLayer(const std::string& layer, float fade);

        // ---- Animator Controller (upstream's player API on the GameObject's Animator) ----
        // the Animator's controller while the player hands over to it ("" otherwise); setting
        // one adds the Animator when missing and hands over, "" gives the clip list back
        void setController(const std::string& controller);
        std::string getController() const;
        bool hasController() const;
        // Unity's Animator.Play(name, layer, normalizedTime) and CrossFadeInFixedTime(name,
        // seconds, layer): a controller state ("Layer.State" or "State") on `layer` (-1: any)
        // from `normalizedTime`. Without a controller: the clip state (its own layer) from that
        // normalized time / with that fade. play / crossFade(name, fade, PlayMode) keep their
        // meaning with a controller (a fade crossfades; the mode does not apply)
        void playInLayer(const std::string& name, int layer);
        void playInLayer(const std::string& name, int layer, float normalizedTime);
        void crossFadeInLayer(const std::string& name, float seconds, int layer);
        void setFloat(const std::string& name, float value);
        void setFloat(const std::string& name, float value, float dampTime);
        float getFloat(const std::string& name) const;
        void setInteger(const std::string& name, int value);
        int getInteger(const std::string& name) const;
        void setBool(const std::string& name, bool value);
        bool getBool(const std::string& name) const;
        void setTrigger(const std::string& name);
        void resetTrigger(const std::string& name);
        bool hasParameter(const std::string& name) const;
        // "float", "int", "bool", "trigger" ("" when unknown)
        std::string getParameterType(const std::string& name) const;
        // the current state of a layer (default 0), without the layer prefix ("" when none)
        std::string getCurrentStateName() const;
        std::string getCurrentStateName(int layer) const;
        ControllerStateInfo getCurrentStateInfo() const;
        ControllerStateInfo getCurrentStateInfo(int layer) const;
        // the destination of the layer's transition (isValid only during one)
        ControllerStateInfo getNextStateInfo() const;
        ControllerStateInfo getNextStateInfo(int layer) const;
        bool isInTransition() const;
        bool isInTransition(int layer) const;

        FunctionSubscribe<void(const AnimationEvent&)>& getOnEvent();
        FunctionSubscribe<void(const std::string&)>& getOnFinish();
        // Animator Controller states entered / exited (layer index, state name)
        FunctionSubscribe<void(int, const std::string&)>& getOnStateEnter();
        FunctionSubscribe<void(int, const std::string&)>& getOnStateExit();
    };

}

#endif //ANIMATIONPLAYER_H
