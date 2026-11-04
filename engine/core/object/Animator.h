// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMATOR_H
#define ANIMATOR_H

#include "EntityHandle.h"
#include "component/AnimatorComponent.h"

#include <string>

namespace jasperengine{

    // The current (or next) state of a layer, as getCurrentState / getNextState return it
    struct JASPERENGINE_API AnimatorStateInfo{
        bool valid = false;
        std::string name;
        std::string fullPath;           // "Base Layer.Locomotion"
        std::string tag;
        float normalizedTime = 0.0f;    // unbounded: 2.5 = the middle of the third loop
        float length = 0.0f;            // seconds of one cycle
        bool loop = false;
        float speed = 1.0f;             // the state's speed times its speed parameter
        bool mirror = false;            // the state's Mirror (humanoid playback)
        bool footIK = false;            // the state's Foot IK (humanoid playback)
    };

    // Unity's Animator (docs/animator.md, docs/specs/S21-animation.md D16): parameters, states,
    // layers, montages, IK targets and root motion of an AnimatorComponent. Calls work in play
    // mode and in games, and on an Animator the editor previews. A name the controller does not
    // have logs one warning per name and does nothing.
    class JASPERENGINE_API Animator: public EntityHandle{
    public:
        // adds the Animator component to the entity when it has none
        Animator(Scene* scene, Entity entity);
        virtual ~Animator();

        Animator(const Animator& rhs);
        Animator& operator=(const Animator& rhs);
        Animator(Animator&& rhs) noexcept;
        Animator& operator=(Animator&& rhs) noexcept;

        // the .controller (asset-relative); setting it loads it again
        void setController(const std::string& path);
        std::string getController() const;
        // the controller is loaded and the rig is bound
        bool isLoaded() const;

        void setFloat(const std::string& name, float value);
        // damped (Unity's SetFloat with dampTime and deltaTime)
        void setFloat(const std::string& name, float value, float dampTime, float deltaTime);
        float getFloat(const std::string& name) const;
        void setInteger(const std::string& name, int value);
        int getInteger(const std::string& name) const;
        void setBool(const std::string& name, bool value);
        bool getBool(const std::string& name) const;
        void setTrigger(const std::string& name);
        void resetTrigger(const std::string& name);
        // -1 when unknown
        int getParameterIndex(const std::string& name) const;
        int getParameterCount() const;
        std::string getParameterName(int index) const;

        // a state by name or "Layer.Machine.State"; layer -1: the state's own layer
        void play(const std::string& state);
        void play(const std::string& state, int layer);
        void play(const std::string& state, int layer, float normalizedTime);
        // normalizedDuration in source-state lengths
        void crossFade(const std::string& state, float normalizedDuration);
        void crossFade(const std::string& state, float normalizedDuration, int layer);
        void crossFade(const std::string& state, float normalizedDuration, int layer, float normalizedOffset);
        void crossFadeInFixedTime(const std::string& state, float seconds);
        void crossFadeInFixedTime(const std::string& state, float seconds, int layer);
        void crossFadeInFixedTime(const std::string& state, float seconds, int layer, float normalizedOffset);

        AnimatorStateInfo getCurrentState(int layer) const;
        AnimatorStateInfo getNextState(int layer) const;
        bool isInTransition(int layer) const;

        int getLayerCount() const;
        std::string getLayerName(int layer) const;
        int getLayerIndex(const std::string& name) const;
        void setLayerWeight(int layer, float weight);
        // reached linearly over `fade` seconds of the Animator's time
        void setLayerWeight(int layer, float weight, float fade);
        float getLayerWeight(int layer) const;

        // a .montage (asset-relative); false when it cannot play (no layer plays its slot)
        bool playMontage(const std::string& path);
        bool playMontage(const std::string& path, float rate);
        bool playMontage(const std::string& path, float rate, const std::string& section);
        void stopMontage();
        void stopMontage(float blendOut);
        bool isMontagePlaying() const;
        // the playing montage's path and section, "" when none
        std::string getMontage() const;
        std::string getMontageSection() const;
        bool montageJumpToSection(const std::string& section);
        bool montageSetNextSection(const std::string& from, const std::string& to);

        // an IK constraint by name: a world target (overrides its target entity) and a weight
        void setIKTarget(const std::string& constraint, const Vector3& position);
        void setIKTarget(const std::string& constraint, const Vector3& position, float weight);
        void clearIKTarget(const std::string& constraint);
        void setIKWeight(const std::string& constraint, float weight);
        // the end is on its target after the last solve (within 1 mm; a look-at: its aim within
        // 0.1 degree). False while it does not solve: weight 0, disabled, culled, unknown name.
        bool isIKReached(const std::string& constraint) const;
        // the constraint's shape (the same fields as the Inspector's): soften 0..1, the hinge axis
        // of the mid bone (zero: auto), the twist in degrees, the eyes from the last bone
        void setIKSoften(const std::string& constraint, float soften);
        void setIKHingeAxis(const std::string& constraint, const Vector3& axis);
        void setIKTwist(const std::string& constraint, float degrees);
        void setIKAimOffset(const std::string& constraint, const Vector3& offset);
        // the first LookAt constraint's target
        void setLookAt(const Vector3& position);
        void setLookAt(const Vector3& position, float weight);

        // the evaluated pose of a bone (a path or a humanoid slot name such as "left_hand"), world
        // space; zero / identity when the bone is unknown or nothing was evaluated yet
        Vector3 getBonePosition(const std::string& bone) const;
        Quaternion getBoneRotation(const std::string& bone) const;
        bool hasBone(const std::string& bone) const;

        void setSpeed(float speed);
        float getSpeed() const;
        void setApplyRootMotion(bool apply);
        bool isApplyRootMotion() const;
        // the last update's root motion, world space
        Vector3 getDeltaPosition() const;
        Quaternion getDeltaRotation() const;

        bool isHumanoid() const;

        FunctionSubscribe<void(const AnimationEvent&)>& getOnEvent();
        FunctionSubscribe<void(const std::string&, const std::string&)>& getOnStateEnter();
        FunctionSubscribe<void(const std::string&, const std::string&)>& getOnStateExit();
        FunctionSubscribe<void(const std::string&, bool)>& getOnMontageEnded();
        FunctionSubscribe<void(Vector3, Quaternion)>& getOnAnimatorMove();

    private:
        anim::AnimatorInstance* runtime() const;
        void unknown(const std::string& what, const std::string& name) const;
        // a parameter set as another type (a trigger set as a bool...): warned once per
        // (parameter, type), the value converted
        void mismatch(const std::string& name, AnimatorParameterType as) const;
    };

}

#endif //ANIMATOR_H
