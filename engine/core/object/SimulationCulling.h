// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Simulation Culling (docs/specs/culling-group-and-simulation-distance.md R10-R18): the
// GameObject's component (Lua `SimulationCulling(scene, entity)`, which ADDS one when it has none)
// and the scene-wide settings over the scene's CullingSystem: the simulation focuses (the main
// camera, plus up to 16 from scripts), the script ground check and the global distance scale.
// Main thread only.

#ifndef SIMULATION_CULLING_H
#define SIMULATION_CULLING_H

#include "object/EntityHandle.h"
#include "component/SimulationCullingComponent.h"
#include "math/Vector3.h"
#include "util/FunctionSubscribe.h"

#include <functional>

namespace jasperengine{

    class JASPERENGINE_API SimulationCulling: public EntityHandle{
    public:
        static constexpr int MAX_SCRIPT_FOCUSES = 16;

        SimulationCulling(Scene* scene, Entity entity);
        virtual ~SimulationCulling();

        SimulationCulling(const SimulationCulling& rhs);
        SimulationCulling& operator=(const SimulationCulling& rhs);

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setSimulationDistance(float distance);
        float getSimulationDistance() const;
        void setReducedRateDistance(float distance);
        float getReducedRateDistance() const;
        void setReducedUpdateInterval(int frames);
        int getReducedUpdateInterval() const;
        void setHysteresis(float metres);
        float getHysteresis() const;
        void setBoundsRadius(float radius);
        float getBoundsRadius() const;
        void setSimulateWhenVisible(bool simulate);
        bool isSimulateWhenVisible() const;
        void setFreezeWhenGroundNotLoaded(bool freeze);
        bool isFreezeWhenGroundNotLoaded() const;
        void setCullScripts(bool cull);
        bool isCullScripts() const;
        void setCullAnimator(bool cull);
        bool isCullAnimator() const;
        void setCullPhysics(bool cull);
        bool isCullPhysics() const;

        SimulationState getState() const;
        SimulationCullReason getReason() const;
        float getFocusDistance() const;
        // "full", "reduced", "culled" / "none", "distance", "ground_not_loaded"
        static const char* stateName(SimulationState state);
        static const char* reasonName(SimulationCullReason reason);

        // ---- the scene's focuses: a position, or a GameObject followed at each evaluation; the id
        // (> 0), 0 past MAX_SCRIPT_FOCUSES. A focus GameObject always simulates itself.
        static int addFocus(Scene* scene, const Vector3& position);
        static int addFocus(Scene* scene, Entity entity);
        static bool setFocus(Scene* scene, int id, const Vector3& position);
        static bool removeFocus(Scene* scene, int id);
        // with script focuses, whether the main camera is a focus too (default true)
        static void setCameraIsFocus(Scene* scene, bool cameraIsFocus);
        static bool isCameraFocus(Scene* scene);

        // multiplies every component's distances, on top of the Device Tier's Simulation Distance
        static void setDistanceScale(float scale);
        static float getDistanceScale();
        // what a script set ends with the Play (Engine::resetApplicationState): the scale back to 1
        static void resetSession();

        // for worlds a game streams itself: false = the ground under (position, radius) is not
        // loaded. Called for components within simulation distance, at most 256 a frame; empty removes it
        static void setGroundCheck(Scene* scene, std::function<bool(const Vector3& position, float radius)> check);

        // ---- the per-callback reads (scripts' guard, Animator): one relaxed load while no scene
        // has a component
        static bool anyActive() { return detail::simulationCullingActive.load(std::memory_order_relaxed) != 0; }
        static bool isCulled(Scene* scene, Entity entity);
        static SimulationState getState(Scene* scene, Entity entity);
        static bool isAnimatorCulled(Scene* scene, Entity entity);
        // the scaled time since the GameObject's previous reduced call, inside that call (a
        // SimulationReducedScope of the GameObject the Hit Stop script scope names, or of any when
        // none is open); false elsewhere
        static bool getReducedScriptDelta(Scene* scriptScene, Entity scriptEntity, double& delta);
    };

}

#endif //SIMULATION_CULLING_H
