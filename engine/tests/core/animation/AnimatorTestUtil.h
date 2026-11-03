// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers of the Animator tests (docs/specs/S21-animation.md AC1-AC12): clip and controller
// builders written as files under the animation tests' asset root (AnimTestEnvironment), and a
// fixture with a small generic rig (Hero -> Hips -> Spine -> Head, Hips -> Leg -> Foot).

#ifndef ANIMATOR_TEST_UTIL_H
#define ANIMATOR_TEST_UTIL_H

#include "AnimTestEnvironment.h"

#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorInstance.h"
#include "component/AnimatorComponent.h"
#include "component/Transform.h"
#include "object/Animator.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "subsystem/AnimatorSystem.h"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace animatortest {

    using namespace jasperengine;

    inline ClipKey key(float time, std::vector<float> value, EaseType ease = EaseType::LINEAR){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        k.ease = ease;
        return k;
    }

    inline ClipTrackData track(const std::string& path, ClipChannel channel, std::vector<ClipKey> keys, bool relative = false){
        ClipTrackData t;
        t.path = path;
        t.channel = channel;
        t.relative = relative;
        t.keys = std::move(keys);
        return t;
    }

    // position x of `path` from 0 to `x` over `length` seconds
    inline ClipData slide(const std::string& path, float x, ClipWrapMode wrap = ClipWrapMode::Loop, float length = 1.0f){
        ClipData data;
        data.wrap = wrap;
        data.tracks = {track(path, ClipChannel::Position, {key(0, {0, 0, 0}), key(length, {x, 0, 0})})};
        return data;
    }

    // a constant position of `path`
    inline ClipData hold(const std::string& path, Vector3 value, ClipWrapMode wrap = ClipWrapMode::Loop, float length = 1.0f){
        ClipData data;
        data.wrap = wrap;
        data.tracks = {track(path, ClipChannel::Position, {key(0, {value.x, value.y, value.z}), key(length, {value.x, value.y, value.z})})};
        return data;
    }

    inline ClipEventData event(float time, const std::string& function){
        ClipEventData e;
        e.time = time;
        e.function = function;
        return e;
    }

    inline Quaternion aboutY(float degrees){
        const float half = degrees * 3.14159265358979f / 360.0f;
        return Quaternion(std::cos(half), 0, std::sin(half), 0);
    }

    inline Quaternion aboutX(float degrees){
        const float half = degrees * 3.14159265358979f / 360.0f;
        return Quaternion(std::cos(half), std::sin(half), 0, 0);
    }

    inline anim::StateDef state(const std::string& name, const std::string& clip){
        anim::StateDef s;
        s.name = name;
        if (!clip.empty()) s.motion.clip.path = clip;
        return s;
    }

    inline anim::TransitionDef transition(const std::string& to, std::vector<anim::ConditionDef> conditions = {}, float duration = 0.0f){
        anim::TransitionDef t;
        t.to = to;
        t.conditions = std::move(conditions);
        t.duration = duration;
        t.fixedDuration = true;
        return t;
    }

    inline anim::ConditionDef condition(const std::string& parameter, AnimatorConditionMode mode, float threshold = 0.0f){
        anim::ConditionDef c;
        c.parameter = parameter;
        c.mode = mode;
        c.threshold = threshold;
        return c;
    }

    inline anim::ParameterDef parameter(const std::string& name, AnimatorParameterType type, float value = 0.0f){
        anim::ParameterDef p;
        p.name = name;
        p.type = type;
        p.defaultValue = value;
        return p;
    }

    // a one-layer controller with `states` (the first is the default)
    inline anim::ControllerDef controller(std::vector<anim::ParameterDef> parameters, std::vector<anim::StateDef> states){
        anim::ControllerDef def;
        def.parameters = std::move(parameters);
        anim::LayerDef layer;
        layer.name = "Base Layer";
        layer.machine.states = std::move(states);
        def.layers.push_back(std::move(layer));
        return def;
    }

    class AnimatorFixture{
    public:
        AnimatorFixture(){
            static int run = 0;
            prefix = "animator/r" + std::to_string(run++) + "/";
            AnimationClipPool::clear();
            AnimatorAssetPool::clear();
            root = node(NULL_ENTITY, "Hero", Vector3(0, 0, 0));
            hips = node(root, "Hips", Vector3(0, 1, 0));
            spine = node(hips, "Spine", Vector3(0, 0.3f, 0));
            head = node(spine, "Head", Vector3(0, 0.4f, 0));
            leg = node(hips, "Leg", Vector3(0.1f, -0.5f, 0));
            foot = node(leg, "Foot", Vector3(0, -0.45f, 0));
        }
        virtual ~AnimatorFixture(){
            AnimationClipPool::clear();
            AnimatorAssetPool::clear();
        }

        Entity node(Entity parent, const std::string& name, Vector3 position, Quaternion rotation = Quaternion()){
            Entity entity = scene.createEntity();
            scene.setEntityName(entity, name);
            Transform transform;
            transform.position = position;
            transform.rotation = rotation;
            scene.addComponent<Transform>(entity, transform);
            if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
            return entity;
        }

        std::string clip(const std::string& name, const ClipData& data){
            animtest::writeAsset(prefix + name + ".anim", anim::writeClip(data));
            return prefix + name + ".anim";
        }

        std::string controllerFile(const std::string& name, const anim::ControllerDef& def){
            animtest::writeAsset(prefix + name + ".controller", anim::writeController(def));
            return prefix + name + ".controller";
        }

        std::string maskFile(const std::string& name, const anim::AvatarMaskDef& def){
            animtest::writeAsset(prefix + name + ".mask", anim::writeMask(def));
            return prefix + name + ".mask";
        }

        std::string montageFile(const std::string& name, const anim::MontageDef& def){
            animtest::writeAsset(prefix + name + ".montage", anim::writeMontage(def));
            return prefix + name + ".montage";
        }

        AnimatorComponent& animator(const std::string& controllerPath, Entity on = NULL_ENTITY){
            const Entity entity = on == NULL_ENTITY ? root : on;
            if (!scene.findComponent<AnimatorComponent>(entity)) scene.addComponent<AnimatorComponent>(entity, {});
            AnimatorComponent& component = scene.getComponent<AnimatorComponent>(entity);
            component.controller = controllerPath;
            component.needsLoad = true;
            return component;
        }

        anim::AnimatorInstance& instance(Entity on = NULL_ENTITY){
            return *scene.getComponent<AnimatorComponent>(on == NULL_ENTITY ? root : on).runtime;
        }

        AnimatorSystem& system(){ return *scene.getSystem<AnimatorSystem>(); }

        void step(double dt, int count = 1){
            for (int i = 0; i < count; i++) system().update(dt);
        }

        Vector3 position(Entity entity){ return scene.getComponent<Transform>(entity).position; }
        Quaternion rotation(Entity entity){ return scene.getComponent<Transform>(entity).rotation; }

        Scene scene;
        std::string prefix;
        Entity root, hips, spine, head, leg, foot;
    };

}

#endif
