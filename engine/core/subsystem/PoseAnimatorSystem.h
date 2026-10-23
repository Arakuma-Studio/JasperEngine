// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POSEANIMATORSYSTEM_H
#define POSEANIMATORSYSTEM_H

#include "SubSystem.h"

#include "component/PoseAnimatorComponent.h"
#include "pose/PoseAnimatorRuntime.h"

namespace jasperengine{

    // Plays the Pose Animators of a scene (spec: pose animation phase 2, section 4.2). Registered
    // right after ActionSystem: it runs once the clips' blend is flushed to the bone transforms
    // and before MeshSystem/RenderSystem compute the world matrices and the skinning. The editor
    // pauses it in edit mode (Project::pauseEngineScene), like the ActionSystem.
    class JASPERENGINE_API PoseAnimatorSystem : public SubSystem {

    private:
        // the runtime for the pending load: the file parsed (errors logged once)
        bool startLoad(PoseAnimatorComponent& animator);
        // builds the rig, the keys and the frames on the model. False while the model is not
        // ready (`wait` set: retried next frame) or on an error (logged, animator disabled)
        bool buildRuntime(Entity entity, PoseAnimatorComponent& animator, bool& wait);
        bool fail(PoseAnimatorComponent& animator, const std::string& message);
        bool bonesAlive(const pose::PoseAnimatorRuntime& runtime) const;
        void applyPose(pose::PoseAnimatorRuntime& runtime, const pose::HumanPose& pose, float weight);

    public:
        PoseAnimatorSystem(Scene* scene);

        // Parses the animator's file if a load is pending, so the pose names and count are known
        // before the model is ready. True when the data is there.
        bool loadData(PoseAnimatorComponent& animator);

        // Back to the starting state (time 0, autoplay decided again, loaded again): the editor
        // calls it when Play starts and stops, since play reuses the edit-time scene
        void resetAnimators();

        // Live transition to a key pose (Lua goToPose); made before the rig is ready it waits
        bool goToPose(PoseAnimatorComponent& animator, const pose::PoseGoTo& request);

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;
    };

}

#endif //POSEANIMATORSYSTEM_H
