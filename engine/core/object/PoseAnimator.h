// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POSEANIMATOR_H
#define POSEANIMATOR_H

#include "EntityHandle.h"
#include "component/PoseAnimatorComponent.h"

#include <string>

namespace jasperengine{

    // Plays a pose animation (.poseanim) live on a humanoid Model: playback, a blend weight over
    // the Model's animation clips and live goToPose transitions. Lives on the Model's GameObject
    // (Model::getPoseAnimator() returns it); runs in play mode and in games.
    class JASPERENGINE_API PoseAnimator: public EntityHandle{
    public:
        // adds the Pose Animator component to the entity when it has none
        PoseAnimator(Scene* scene, Entity entity);
        virtual ~PoseAnimator();

        PoseAnimator(const PoseAnimator& rhs);
        PoseAnimator& operator=(const PoseAnimator& rhs);
        PoseAnimator(PoseAnimator&& rhs) noexcept;
        PoseAnimator& operator=(PoseAnimator&& rhs) noexcept;

        // .poseanim path relative to the assets folder; setting it loads the file again (even the same path)
        void setAnimation(const std::string& path);
        std::string getAnimation() const;

        void play();
        // stops playback; a live goToPose transition freezes where it is
        void pause();
        // back to time 0, showing key pose 0
        void stop();
        bool isPlaying() const;

        // seconds
        void setTime(float time);
        float getTime() const;
        float getLength() const;

        void setSpeed(float speed);
        float getSpeed() const;
        // S48 Pose Frame Rate (fps, 0 = smooth): poses held on whole steps from each key
        void setPoseFrameRate(float fps);
        float getPoseFrameRate() const;
        // 0..1 blend over the clips (or the rest pose) under the animator
        void setWeight(float weight);
        float getWeight() const;

        void setAutoplay(bool autoplay);
        bool isAutoplay() const;

        // 0-based; findPose returns -1 when there is no pose with that name
        int getPoseCount() const;
        std::string getPoseName(int index) const;
        int findPose(const std::string& name) const;

        // Live transition from the pose shown now to a key pose, then holds it (playback stops).
        // Values < 0 take the animation's transition. False when the pose does not exist.
        bool goToPose(int index);
        bool goToPose(int index, float duration);
        bool goToPose(int index, float duration, float overshoot);
        bool goToPose(int index, float duration, float overshoot, float anticipation);
        bool goToPose(const std::string& name);
        bool goToPose(const std::string& name, float duration);
        bool goToPose(const std::string& name, float duration, float overshoot);
        bool goToPose(const std::string& name, float duration, float overshoot, float anticipation);

        // the rig is built and the frames are ready
        bool isLoaded() const;

        // called when a one-shot animation ends
        FunctionSubscribe<void()>& getOnFinish();
    };

}

#endif //POSEANIMATOR_H
