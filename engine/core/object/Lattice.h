// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_H
#define LATTICE_H

#include "GameObject.h"
#include "action/Ease.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/LatticeFollowerComponent.h"

namespace jasperengine{

    // Box-shaped cage of control points deforming the meshes bound to it (spec §8). Its local
    // space is the unit cube [-0.5, 0.5]^3, placed and sized by the Transform (scale = size).
    // Point (i, j, k) counts from 0 on each axis; its flat index is i + rx * (j + ry * k).
    // Offsets are local-space vectors from the rest positions, zero at rest. Every change
    // takes effect in the same frame, in the editor too; tweens and jiggle run while playing.
    // Writing offsets (setOffset..., setPointPosition..., resetOffsets, applyPose, applyPreset)
    // stops a running tween. Out of range points are logged and ignored (getters return zero).
    class JASPERENGINE_API Lattice: public GameObject{
    private:
        // the flat index of (i, j, k), -1 (logged) when outside the resolution
        int checkedIndex(int i, int j, int k, const char* function) const;
        int checkedIndex(int index, const char* function) const;

    public:
        Lattice(Scene* scene);
        Lattice(Scene* scene, Entity entity);
        virtual ~Lattice();

        Lattice(const Lattice& rhs);
        Lattice& operator=(const Lattice& rhs);
        Lattice(Lattice&& rhs) noexcept;
        Lattice& operator=(Lattice&& rhs) noexcept;

        LatticeComponent& getLatticeComponent() const;

        // component checkbox (Unity Behaviour.enabled): a disabled lattice deforms nothing
        void setEnabled(bool enabled);
        bool isEnabled() const;

        // points per axis, each 2..16 (default 3, 3, 3); changing it resamples the offsets,
        // the poses and a running tween so the deformation keeps its shape
        void setResolution(int x, int y, int z);
        // resolution as a Vector3 of whole numbers
        Vector3 getResolution() const;
        int getResolutionX() const;
        int getResolutionY() const;
        int getResolutionZ() const;
        int getPointCount() const;
        // flat index of point (i, j, k)
        int getPointIndex(int i, int j, int k) const;

        // offset of a point from its rest position, lattice local units
        Vector3 getOffset(int i, int j, int k) const;
        void setOffset(int i, int j, int k, Vector3 offset);
        Vector3 getOffsetAt(int index) const;
        void setOffsetAt(int index, Vector3 offset);
        // rest position of a point, lattice local space
        Vector3 getRestPosition(int i, int j, int k) const;
        // current position (rest + offset), lattice local space
        Vector3 getPointPosition(int i, int j, int k) const;
        void setPointPosition(int i, int j, int k, Vector3 position);
        // current position in world space, with the Transform as of the last frame's update
        // (call updateTransform() first after moving the lattice in the same frame)
        Vector3 getPointWorldPosition(int i, int j, int k) const;
        void setPointWorldPosition(int i, int j, int k, Vector3 position);
        // every offset back to zero
        void resetOffsets();

        void setInterpolation(LatticeInterpolation interpolation);
        LatticeInterpolation getInterpolation() const;
        void setOutside(LatticeOutside outside);
        LatticeOutside getOutside() const;
        // how far outside the cage the deformation fades out with LatticeOutside::FADE, local units
        void setFadeDistance(float fadeDistance);
        float getFadeDistance() const;

        // adds a preset to every offset: name "squash", "stretch", "taper", "twist", "bend",
        // "bulge", "shear" or "noise"; axis and toward "x", "y" or "z" (toward: bend and shear,
        // empty = the axis after `axis`); amount as in the presets (degrees for twist and bend).
        // False (logged) on an unknown name or axis.
        bool applyPreset(const std::string& name, const std::string& axis, float amount, const std::string& toward = "");

        // stores the current offsets as a pose, replacing one of the same name
        void savePose(const std::string& name);
        bool hasPose(const std::string& name) const;
        bool removePose(const std::string& name);
        // offsets = pose * weight; false (logged) when there is no such pose
        bool applyPose(const std::string& name, float weight = 1.0f);
        // animates the offsets from their current values to the pose over `duration` seconds
        // (while playing); false (logged) when there is no such pose
        bool tweenToPose(const std::string& name, float duration, EaseType ease = EaseType::LINEAR);
        // animates every offset back to zero
        void tweenToRest(float duration, EaseType ease = EaseType::LINEAR);
        bool isTweening() const;
        // stops a running tween where it is
        void stopTween();

        // spring-driven wobble when the lattice accelerates or is poked (while playing): each point
        // springs back to its offset (stiffness, damping), pushed by the lattice's acceleration
        // times inertia, at most maxOffset (local units) away; anchored face stays still
        void setJiggle(bool jiggle);
        bool isJiggle() const;
        void setJiggleStiffness(float stiffness);
        float getJiggleStiffness() const;
        void setJiggleDamping(float damping);
        float getJiggleDamping() const;
        void setJiggleInertia(float inertia);
        float getJiggleInertia() const;
        void setJiggleAnchor(LatticeJiggleAnchor anchor);
        LatticeJiggleAnchor getJiggleAnchor() const;
        void setJiggleMaxOffset(float maxOffset);
        float getJiggleMaxOffset() const;
        // true while the jiggle is at rest (nothing moves, nothing to draw)
        bool isJiggleSleeping() const;
        // jiggle displacement of a point on top of its offset, local units
        Vector3 getJiggleOffset(int i, int j, int k) const;
        // clears the jiggle motion, e.g. after teleporting the lattice
        void resetJiggle();
        // adds velocity to one point (local units per second) and wakes the jiggle
        void poke(int i, int j, int k, Vector3 impulse);
        // adds velocity (world units per second) to the points within `radius` (world units) of
        // `worldPoint`, fading smoothly to the edge; radius <= 0 pokes the nearest point
        void pokeAt(Vector3 worldPoint, Vector3 impulse, float radius);

        // where a world point ends up through this lattice (world in, world out)
        Vector3 deformPoint(Vector3 worldPoint) const;

        // makes the target's mesh (and its descendants' meshes with includeChildren) follow this
        // lattice: adds a binding to its LatticeModifierComponent, adding the component when
        // missing; an existing binding to this lattice gets the weight (0..2). False (logged)
        // when the target is not in this scene.
        bool bind(Entity target, float weight = 1.0f);
        bool bind(EntityHandle* target, float weight = 1.0f);
        // removes the target's bindings to this lattice (the component stays); false when none
        bool unbind(Entity target);
        bool unbind(EntityHandle* target);

        // places `follower` where `rest`'s transform ends up through this lattice each frame
        // (LatticeFollowerComponent, added or retargeted); false (logged) when follower is the
        // rest, an ancestor of it, or of this lattice
        bool addFollower(Entity follower, Entity rest, bool applyPosition = true, bool applyRotation = true,
                         bool applyScale = false, float weight = 1.0f);
        bool addFollower(EntityHandle* follower, EntityHandle* rest, bool applyPosition = true, bool applyRotation = true,
                         bool applyScale = false, float weight = 1.0f);
        // removes the follower's LatticeFollowerComponent when it follows this lattice
        bool removeFollower(Entity follower);
        bool removeFollower(EntityHandle* follower);

        // the target's LatticeModifierComponent (bindings to any lattice), null when it has none;
        // use it right away: adding that component to another entity can move it
        LatticeModifierComponent* getModifierComponent(Entity target) const;
        // the follower's LatticeFollowerComponent when it follows this lattice, else null; same
        // lifetime as getModifierComponent
        LatticeFollowerComponent* getFollowerComponent(Entity follower) const;
    };

}

#endif //LATTICE_H
