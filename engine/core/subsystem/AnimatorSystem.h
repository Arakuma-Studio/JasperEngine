// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md D7, R13-R21)
// - Purpose: plays the scene's Animators. Registered after ActionSystem (the Animation Players'
//   blend is flushed: the Animator overwrites the bones it binds) and before PoseAnimatorSystem
//   (a Pose Animator's weight blends over it), so before MeshSystem/RenderSystem compute world
//   matrices and skins. Each update:
//   1. Prepare (main thread): loads and binds (controllers, masks, montages through
//      AnimatorAssetPool, clips through AnimationClipPool; rebinding on a pool or hierarchy
//      revision), culling from last frame's visibility, the IK inputs (target entities, FootIK
//      raycasts from last frame's feet).
//   2. Wave (jobs::parallelFor, 4 Animators per job; serial with 0 workers): each Animator's
//      AnimatorInstance::update and the writes of its own entities (bone local TRS, morph
//      weights, lattice offsets, skin matrices of its animator-driven ModelSkinBindings).
//   3. Finalize (main thread, entity order): root motion (Transform, S22 setRootMotion, or
//      onAnimatorMove), state enter/exit, montage ends, events (C++/Lua subscribers, then the
//      scripts' methods named like the events).
//   AnimatePhysics Animators run in fixedUpdate, the others in update (UnscaledTime: the frame
//   delta before Engine's time scale).
// - The editor pauses it in edit mode (Project::pauseEngineScene); the Animator window's edit
//   preview drives one Animator through preview()/restorePreview().
// - Thread-safety: main thread, except the wave's per-Animator work (it touches only that
//   Animator's runtime and entities; two Animators binding one entity: the later reports a bind
//   error and drives nothing).

#ifndef ANIMATORSYSTEM_H
#define ANIMATORSYSTEM_H

#include "SubSystem.h"

#include "animation/AnimationEvent.h"
#include "animation/AnimatorInstance.h"
#include "component/AnimatorComponent.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    class JASPERENGINE_API AnimatorSystem : public SubSystem {
    public:
        struct Stats{
            double prepareMs = 0.0;
            double waveMs = 0.0;
            double finalizeMs = 0.0;
            size_t animators = 0;       // updated this frame
            size_t sampled = 0;         // evaluated and written
            size_t culled = 0;
            size_t footRaycasts = 0;    // Foot IK's ground rays

            Stats& operator+=(const Stats& other){
                prepareMs += other.prepareMs;
                waveMs += other.waveMs;
                finalizeMs += other.finalizeMs;
                animators += other.animators;
                sampled += other.sampled;
                culled += other.culled;
                footRaycasts += other.footRaycasts;
                return *this;
            }
        };

        AnimatorSystem(Scene* scene);

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;
        void fixedUpdate(double dt) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // the Animator's runtime, created (and its controller loaded and bound when possible) now;
        // null without an AnimatorComponent
        anim::AnimatorInstance* instance(Entity entity);
        // loads and plays a montage (a .montage path) on the Animator; false with `error`
        bool playMontage(Entity entity, const std::string& path, float rate, const std::string& section, std::string* error);

        // Edit-mode preview of one Animator (the system is paused): updates it by `dt` and writes
        // the pose (no callbacks run); false with `error` when it cannot play
        bool preview(Entity entity, float dt, std::string* error);
        // puts back every value the Animator wrote (its rest values) and forgets its runtime
        void restorePreview(Entity entity);

        // back to the starting state (Play starts and stops: the scene is restored)
        void resetAnimators();

        // 0 workers or not: the wave's jobs (ParallelEqualsSerial, benchmarks)
        void setParallel(bool parallel);
        // the last run (the update or a fixed step)
        const Stats& getStats() const { return stats; }
        // what ran since the last call, both phases: time adds up, and the counts are one frame's,
        // however many fixed steps it took (the profiler's monitors take them once per frame)
        Stats takeFrameStats();

    private:
        struct Job{
            Entity entity = NULL_ENTITY;
            AnimatorComponent* component = nullptr;
            anim::AnimatorInstance* instance = nullptr;
            anim::AnimatorUpdateContext context;
        };

        std::vector<Job> jobs;
        Stats stats;
        Stats updateStats;
        Stats fixedStats;
        bool parallel = true;
        uint64_t clipRevision = 0;
        uint64_t assetRevision = 0;
        bool revisionChanged = false;
        // entities an Animator binds (the first one owns them)
        std::unordered_map<Entity, Entity> owners;
        // reused for every dispatch (no allocation once the strings have grown)
        AnimationEvent event;
        std::vector<anim::AnimatorPendingEvent> sortedEvents;
        std::vector<anim::AnimatorStateNote> stateNotes;
        std::vector<anim::AnimatorMontageNote> montageNotes;

        void run(double dt, bool fixedPhase);
        // the Prepare step of one Animator; false when it does not update this frame
        bool prepare(Entity entity, AnimatorComponent& component, float dt, Job& job);
        bool loadController(Entity entity, AnimatorComponent& component, anim::AnimatorInstance& instance);
        // (re)binds when needed; false while waiting (models loading, clips loading)
        bool ensureBound(Entity entity, AnimatorComponent& component, anim::AnimatorInstance& instance, bool animInCall = false);
        void pollClips(Entity entity, AnimatorComponent& component, anim::AnimatorInstance& instance, bool animInCall, bool& rebind, bool& loading);
        bool visible(const anim::AnimatorInstance& instance) const;
        void ikInputs(AnimatorComponent& component, anim::AnimatorInstance& instance, const anim::AnimatorUpdateContext& context);
        void writePose(const AnimatorComponent& component, anim::AnimatorInstance& instance);
        void finalize(Job& job, float dt);
        // the update's audio cues, then the state_exit stops of the states and montages that ended (S40 R4-R7)
        void fireCues(Entity entity, anim::AnimatorInstance& instance);
        bool warnedNoSoundLayer = false;
        void release(Entity entity, anim::AnimatorInstance& instance);
        void setSkinsDriven(anim::AnimatorInstance& instance, bool driven);
        std::string modelPathOf(Entity entity) const;
        bool modelsLoading(Entity entity) const;
    };

}

#endif //ANIMATORSYSTEM_H
