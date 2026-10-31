// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Trail Renderers (lines and trails spec 6.4).
// - Clock: the scene's scaled game time (Engine.timeScale, like Unity): the delta of each
//   update. At time scale 0 no point is added or removed; the head still follows the object.
// - Registered right after RenderSystem, whose update makes this frame's world transforms:
//   each trail samples its GameObject's world position after scripts, animation and physics
//   moved it, then the render system builds its ribbon and loads it (RenderSystem::updateTrail),
//   so the head is drawn where the object is drawn, in the same frame.
// - Points: dropped once older than `time`; added while emitting when the head moved more than
//   minVertexDistance from the newest one (the first one where the head was when it started
//   moving); at most MAX_POINTS (the oldest go first, a warning once). While emitting the live
//   head is the first vertex of the ribbon (not emitting: the stored points only, detached from
//   the object, as Unity); width and colour run by age (0 head, 1 the oldest point).
// - A trail whose GameObject is inactive in the hierarchy, or which is disabled, is cleared
//   (pooled objects reactivated elsewhere draw no streak from where they were).
// - autodestruct: the GameObject is destroyed (end of frame, GameObject.destroy) once the trail
//   has had no points and the object has not moved for `time` seconds.
// - Edit mode (the editor pauses it): only the preview entities simulate, on the unscaled
//   delta (the Scene view's trail of a selected object being dragged, cleared on release).
// TrailSimulation is the engine-free core (an injected clock), gtested
// (editor/tests/TrailSimulationTest.cpp).

#ifndef TRAILSYSTEM_H
#define TRAILSYSTEM_H

#include "SubSystem.h"

#include "component/TrailRendererComponent.h"

#include <set>
#include <vector>

namespace jasperengine{

    class JASPERENGINE_API TrailSimulation{
    public:
        // stored points of one trail at most (one per frame with minVertexDistance 0)
        static constexpr size_t MAX_POINTS = 4096;

        enum StepResult : unsigned int {
            Step_None = 0,
            // the ribbon changes (points added or removed, or the head moved)
            Step_Changed = 1 << 0,
            // autodestruct: the GameObject should be destroyed
            Step_Destroy = 1 << 1,
            // MAX_POINTS reached for the first time: the oldest point was dropped
            Step_CapacityWarning = 1 << 2
        };

        // One frame of a trail: `now` is the clock after this frame, `dt` its delta (0 at time
        // scale 0), `head` the object's world position now
        static unsigned int step(TrailRendererComponent& trail, const Vector3& head, float now, float dt);

        // forgets every point and where the head was (Unity Clear())
        static void clear(TrailRendererComponent& trail);

        // adds a point now (Unity AddPosition), dropping the oldest at MAX_POINTS
        static void addPosition(TrailRendererComponent& trail, const Vector3& position, float now);

        // the curve and gradient parameter of a point born at `birthTime`: its age over `time`
        static float ageT(const TrailRendererComponent& trail, float birthTime, float now);

        // The centre line the ribbon is built from: the live head (while emitting), then the stored points
        // newest to oldest, with each one's parameter (head 0). Empty when the trail has no
        // stored point
        static void ribbonPoints(const TrailRendererComponent& trail, const Vector3& head, float now,
                                 std::vector<Vector3>& points, std::vector<float>& pointT);
    };

    class JASPERENGINE_API TrailSystem : public SubSystem {

    private:
        // seconds of scaled game time (the trails' clock)
        double clock = 0.0;
        // edit mode: entities whose trail simulates while the system is paused, and their clock
        std::set<Entity> previewEntities;
        double previewClock = 0.0;
        std::vector<Vector3> scratchPoints;
        std::vector<float> scratchT;

        void updateTrail(Entity entity, TrailRendererComponent& trail, float now, float dt, bool allowDestroy);

    public:
        TrailSystem(Scene* scene);

        // the trails' clock (scaled game time since the scene started)
        float getTime() const;

        // Edit mode preview (the editor, while a selected GameObject is dragged): the entity's
        // trail simulates although the system is paused; off clears it
        void setPreview(Entity entity, bool preview);
        void clearPreviews();
        bool isPreviewing(Entity entity) const;

        // the ribbon's centre line now (head first) and its points' parameters, for the
        // render system
        void getRibbonPoints(Entity entity, const TrailRendererComponent& trail, std::vector<Vector3>& points, std::vector<float>& pointT) const;

        void clear(Entity entity);

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentRemoved(Entity entity, ComponentId componentId) override;
    };

}

#endif //TRAILSYSTEM_H
