// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 5.2, 7.3)
// - Purpose: runs the scene's Camera Follow components, then its Camera Shakers, once per frame.
//   Registered after the Vertex Animator system and before UISystem and RenderSystem: it sees
//   this frame's animation, physics interpolation and Lua onUpdate writes, and RenderSystem builds
//   the matrices from what it leaves. Writes made in onPostUpdate reach the camera a frame late.
// - Follow (phase 2): the target's world pose of this frame (local TRS composed up its parents,
//   not the cached world matrices of the last render update), the offset in its space, the dead
//   zone (orthographic / UI cameras), the bounds, Unity's SmoothDamp per world axis, then look-at
//   (CameraComponent::target for useTarget cameras, else an exponential slerp of the rotation).
//   It owns the Transform while enabled: a foreign write is overwritten, with a warning once.
// - Shakers: each steps its instances (scaled or unscaled time per shaker or per shake) and its
//   trauma, sums them, applies the 2D rule, units and gains, then sets the camera's view offset
//   (never its Transform) or, without a camera, moves the Transform additively (transform mode).
//   startPreset plays on each enable edge; disabled or inactive, the offset is cleared once and
//   the instances hold. Idle shakers (no instance, no trauma, nothing applied) cost nothing.
// - Edit mode (paused by the editor): nothing runs but the Inspector's preview (previewShake...),
//   on a runtime that is not the component's, through the view offset only.
// - Main thread.

#ifndef CAMERACONTROLSYSTEM_H
#define CAMERACONTROLSYSTEM_H

#include "SubSystem.h"

#include "camera/CameraShake.h"
#include "camera/ShakeInstance.h"
#include "component/CameraFollowComponent.h"
#include "component/CameraShakerComponent.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace jasperengine{

    namespace camera{ struct ShakerRuntime; }
    struct CameraComponent;

    // A Camera Follow's placement: the local values it writes this frame
    struct JASPERENGINE_API FollowPlacement{
        bool valid = false;             // false: no target (nothing to place)
        Vector3 position = Vector3(0, 0, 0);
        bool hasRotation = false;
        Quaternion rotation;
        bool hasCameraTarget = false;   // a useTarget camera's CameraComponent::target
        Vector3 cameraTarget = Vector3(0, 0, 0);
        Vector3 desired = Vector3(0, 0, 0);     // world
    };

    // A GameObject's world pose now (local TRS composed up its parents)
    struct JASPERENGINE_API CameraWorldPose{
        Matrix4 matrix;
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
    };

    class JASPERENGINE_API CameraControlSystem : public SubSystem {

    private:
        // the Inspector's edit-mode preview
        Entity previewEntity = NULL_ENTITY;
        std::shared_ptr<camera::ShakerRuntime> previewRuntime;
        // the CameraShakerComponent settings the preview uses (copied when it starts)
        CameraShakerComponent previewSettings;

        void updateFollowers(float scaledDt, float unscaledDt);
        void followOne(Entity entity, CameraFollowComponent& follow, float dt);
        void updateShakers(float scaledDt, float unscaledDt);
        // steps the instances and the trauma, then applies the offset
        void stepShaker(Entity entity, const CameraShakerComponent& shaker, camera::ShakerRuntime& runtime,
                        float scaledDt, float unscaledDt);
        void applyOffset(Entity entity, const CameraShakerComponent& shaker, camera::ShakerRuntime& runtime,
                         const camera::ShakeOffset& raw);
        void applyTransformOffset(Entity entity, camera::ShakerRuntime& runtime, const Vector3& position, const Vector3& rotation);
        void restoreTransformBase(Entity entity, camera::ShakerRuntime& runtime);
        // the offset goes (view offset off, or the transform-mode base written back)
        void clearOffset(Entity entity, camera::ShakerRuntime& runtime);
        void updatePreview(float dt);
        bool beginPreview(Entity entity);

    public:
        // world units per art pixel for this shaker (S48 Pixel Snap, pixel-unit presets)
        float pixelWorldSize(Entity entity, const CameraComponent* camera) const;
        CameraControlSystem(Scene* scene);

        // The start of a frame, before the scripts and animations (Engine::runFrame): a
        // transform-mode shaker writes its base back, so they never see (and add to) the offset
        void preUpdate();

        // One frame: the followers, then the shakers. update(dt) calls it with the scene delta and
        // Engine::getUnscaledDeltatime(); tests call it with their own
        void step(float scaledDt, float unscaledDt);

        // ---- shakers (the CameraShaker object and CameraShake use these) ----

        // The runtime of a shaker, made when missing
        static camera::ShakerRuntime& ensureRuntime(CameraShakerComponent& shaker);
        // A new instance on the shaker; its id, 0 when `scale` <= 0 (nothing is added)
        uint64_t play(Entity entity, CameraShakerComponent& shaker, const camera::ShakePreset& preset,
                      const std::string& name, const ShakeOptions& options, float scale = 1.0f);
        // every instance fades out over `fade` (< 0: its own fade_out; 0: now)
        static void stopAll(CameraShakerComponent& shaker, float fade);
        static void addTrauma(CameraShakerComponent& shaker, float amount);
        // the 2D rule applies (mode, or Auto with an orthographic / UI camera or a UI GameObject)
        bool is2D(Entity entity, const CameraShakerComponent& shaker) const;
        // max(0, 1 - d / radius), d from the shaker's world position (x/y only in 2D)
        float distanceScale(Entity entity, const CameraShakerComponent& shaker, const Vector3& point, float radius) const;
        // the applied view / transform offset is cleared and every runtime dropped (the editor's Stop)
        void resetAll();

        // ---- follow ----

        // where a follower goes this frame (damped with `dt`, or placed rigidly with `snap`)
        FollowPlacement computeFollow(Entity entity, CameraFollowComponent& follow, float dt, bool snap);
        // the next update places the follower without damping
        static void snap(CameraFollowComponent& follow);
        CameraWorldPose computeWorldNow(Entity entity) const;
        static CameraWorldPose computeWorldNow(Scene* scene, Entity entity);
        // the scene `follow.target` is a GameObject of: this one, or the persistent scene
        Scene* targetSceneOf(const CameraFollowComponent& follow) const;
        bool targetAlive(const CameraFollowComponent& follow, Entity self) const;
        // GameObjects of this scene moved to `to` (ObjectManager::moveToScene, old -> new): a
        // follower of one follows it there when `to` is the persistent scene (DontDestroyOnLoad),
        // else it loses its target with a warning (other scenes cannot be followed)
        void remapTargets(Scene* to, const std::unordered_map<Entity, Entity>& moved);
        // Unity's Mathf.SmoothDamp (maxSpeed <= 0: unlimited; smoothTime <= 0: snaps)
        static float smoothDamp(float current, float target, float& velocity, float smoothTime, float maxSpeed, float dt);

        // ---- the editor's edit-mode preview (spec 7.3 step 3) ----

        // shakes `entity`'s camera through its view offset on a preview runtime, with its Camera
        // Shaker's settings (the defaults without one); false when the GameObject has no camera
        // (transform mode shows no preview)
        bool previewShake(Entity entity, const camera::ShakePreset& preset, const std::string& name);
        bool previewAddTrauma(Entity entity, float amount);
        // the preview's shakes fade out over `fade` (< 0: their fade_out), its trauma goes
        void previewStop(float fade = -1.0f);
        // stops the preview and clears its view offset
        void stopPreview();
        // a preview runs (instances or trauma left)
        bool isPreviewing() const;
        Entity getPreviewEntity() const;
        const camera::ShakerRuntime* getPreviewRuntime() const;

        void setPaused(bool value) override;
        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;
    };

}

#endif //CAMERACONTROLSYSTEM_H
