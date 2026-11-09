// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Internal to ActionSystem's Animation Player sources (ActionSystemPlayers.cpp and the layered
// evaluator in ActionSystemLayers.cpp): helpers both use. Main thread.

#ifndef ACTION_SYSTEM_PLAYERS_DETAIL_H
#define ACTION_SYSTEM_PLAYERS_DETAIL_H

#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipTypes.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::playerdetail{

    // the scene value of the channels of `entity` (R2) that have none yet
    void completeRest(Scene* scene, Entity entity, anim::ChannelRest& rest, ClipChannel channel);
    // the rest of a bound property (its value in key space the first time, the binding it was
    // bound through every time: what the flush and a preview restore write through); null when
    // the binding is not Bound
    anim::PropertyRest* completePropertyRest(Scene* scene, Entity entity, anim::ChannelRest& rest, const anim::PropertyBinding& binding);
    // The root's humanoid rig resolved again (anim-retarget): `current` replaced only when the
    // rig (the cached PoseRig), its validity or its slot entities changed, so a pool change that
    // left the rig alone (any clip insert) keeps the bindings made on it. True when replaced.
    bool refreshHumanoidSlots(Scene* scene, Entity root, std::shared_ptr<anim::HumanoidSlots>& current);
    // the time a state is sampled at this update, in its clip
    float sampleTimeOf(const AnimationPlayerComponent& player, const anim::PlayerStateRuntime& state);
    // the weight a state blends with this update: its weight, or with a Pose Frame Rate the one
    // taken on the last step of the player's clock (S48 R23)
    float blendWeightOf(const AnimationPlayerComponent& player, anim::PlayerStateRuntime& state);

    // one set of bindings and the per-track slot indices to fill for it
    struct PoseSlotSource{
        anim::ClipBindings* bindings = nullptr;
        std::vector<uint32_t>* slots = nullptr;
        // a humanoid clip: the slot of each HumanoidBinding output (anim-retarget)
        std::vector<uint32_t>* humanoidSlots = nullptr;
    };
    // The scratch's slot table for these bindings (one slot per bound (target, channel), rest
    // pointers into `rest`); fills every source's per-track slot indices and per humanoid output
    // slot indices (ActionSystemLayers.cpp)
    void buildPoseSlots(Scene* scene, anim::PlayerPoseScratch& scratch, const std::vector<PoseSlotSource>& sources, anim::RestMap& rest);

}

#endif // ACTION_SYSTEM_PLAYERS_DETAIL_H
