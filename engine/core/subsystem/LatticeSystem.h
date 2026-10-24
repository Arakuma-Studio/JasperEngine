// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICESYSTEM_H
#define LATTICESYSTEM_H

// Lattice runtime (spec §6): pose tweens, jiggle springs, followers, the sprite
// tessellation request. Mesh deformation itself happens on the GPU (LatticeRender).

#include "SubSystem.h"
#include "math/Vector3.h"
#include "math/Matrix4.h"
#include "action/Ease.h"
#include "component/LatticeComponent.h"
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{

    struct Transform;

    class JASPERENGINE_API LatticeSystem : public SubSystem {
    private:
        // play time since the last lateUpdate: the jiggle steps there, once transforms are current
        double jiggleTime = 0.0;
        // frames passed without a jiggle step (paused): the acceleration history restarts
        bool jiggleHistoryStale = false;

        // per-frame scratch, kept so a steady frame allocates nothing
        std::vector<float> jiggleWeights;
        std::vector<std::pair<size_t, Entity>> followerOrder;
        std::vector<std::pair<Entity, std::vector<Vector3>>> followerGrids; // padded grid per lattice
        size_t followerGridCount = 0;                                       // entries used this frame

        void updateTweens(double dt);
        void updateJiggle(double dt);
        void stepJiggle(LatticeComponent& lattice, const Transform& transform, double dt);
        void updateFollowers();

    public:
        LatticeSystem(Scene* scene);
        virtual ~LatticeSystem();

        // called by RenderSystem::update once transforms are current (spec §5.6): jiggle while
        // playing, then followers (in the editor too)
        void lateUpdate();

        void load() override;
        void draw() override;
        void destroy() override;
        // pose tweens, only while playing (the editor pauses the system in edit mode)
        void update(double dt) override;

        // --- component level helpers shared by the Lattice object, the editor and MCP. They
        // change the component directly (no undo) and bump LatticeComponent::revision. ---

        // resolution of a component clamped to [LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION]
        static void getResolution(const LatticeComponent& lattice, int& rx, int& ry, int& rz);
        // changes the resolution keeping the deformation: offsets, every pose and a running
        // tween are resampled (lattice::resample), the jiggle restarts
        static void setResolution(LatticeComponent& lattice, int rx, int ry, int rz);

        // the pose named `name`, null when there is none
        static const LatticePose* findPose(const LatticeComponent& lattice, const std::string& name);
        // pose offsets for the lattice's current resolution: a pose of another size (saved at
        // another resolution and not resampled with it) is resampled from the resolution its
        // size factors into, the one closest to the current
        static std::vector<Vector3> getPoseOffsets(const LatticeComponent& lattice, const LatticePose& pose);
        // stores the current offsets as `name`, replacing a pose of the same name
        static void savePose(LatticeComponent& lattice, const std::string& name);
        // offsets = pose * weight; false when there is no such pose
        static bool applyPose(LatticeComponent& lattice, const std::string& name, float weight);

        // animates offsets from their current values to `target` (resolution sized); duration <= 0
        // sets them at once. Advanced by update(dt) while playing; ends on the exact target.
        static void startTween(LatticeComponent& lattice, const std::vector<Vector3>& target, float duration, EaseType ease);
        static void stopTween(LatticeComponent& lattice);

        // clears the jiggle state (dynamic offsets, velocities, the acceleration history): the
        // next frames start it again as after enabling it, use it after teleporting a lattice
        static void resetJiggle(LatticeComponent& lattice);
        // adds velocity (local units per second) to one point and wakes the jiggle (nothing
        // when the jiggle is off)
        static void poke(LatticeComponent& lattice, int index, const Vector3& localImpulse);
        // adds velocity (world units per second) to the points within worldRadius of worldPoint,
        // with a smooth radial falloff (the nearest point alone when worldRadius <= 0)
        static void pokeAt(LatticeComponent& lattice, const Matrix4& latticeModel, const Vector3& worldPoint,
                           const Vector3& worldImpulse, float worldRadius);

        // world point through one lattice entity (offsets and jiggle); the point itself when the
        // entity has no LatticeComponent, is disabled or inactive, or has a zero scale
        Vector3 deformWorldPoint(Entity lattice, const Vector3& worldPoint, float weight = 1.0f) const;

        // a follower setup the runtime accepts: the follower is not its own rest, not an
        // ancestor of the rest, and the lattice is not in the follower's subtree (each frame
        // would move it again)
        bool isValidFollower(Entity follower, Entity lattice, Entity rest) const;
    };

}

#endif //LATTICESYSTEM_H
