// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MODEL_H
#define MODEL_H

#include "Mesh.h"
#include "Bone.h"
#include "action/Animation.h"
#include "PoseAnimator.h"

namespace jasperengine{
    class JASPERENGINE_API Model: public Mesh{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Model is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        Model(Scene* scene);
        Model(Scene* scene, Entity entity);
        virtual ~Model();

        Model(const Model& rhs);
        Model& operator=(const Model& rhs);
        Model(Model&& rhs) noexcept;
        Model& operator=(Model&& rhs) noexcept;

        bool loadModel(const std::string& filename);

        bool loadOBJ(const std::string& filename);
        bool loadGLTF(const std::string& filename);

        Animation getAnimation(int index);
        Animation findAnimation(const std::string& name);

        // Crossfade to an animation: fades out any currently running animations of
        // this model and fades the target in. Overloads without a fade time use the
        // target animation's authored defaultFadeTime.
        void playAnimation(int index);
        void playAnimation(int index, float fadeTime);
        void playAnimation(const std::string& name);
        void playAnimation(const std::string& name, float fadeTime);

        void stopAnimations(float fadeTime);

        Bone getBone(const std::string& name);
        Bone getBone(int id);

        float getMorphWeight(const std::string& name);
        float getMorphWeight(int id);
        void setMorphWeight(const std::string& name, float value);
        void setMorphWeight(int id, float value);

        void resetToBindPose();

        // the Pose Animator of this model (added when missing)
        PoseAnimator getPoseAnimator();
    };
}

#endif //MODEL_H
