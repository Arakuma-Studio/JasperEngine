// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S77 R26)
// - Purpose: where the Scene view's skeleton overlay draws a model's bones. Under an Animator with
//   Write Bones: Exposed most bones' Transforms are never written, so reading them would show the
//   rest pose while the mesh moves: those bones are read from the Animator's evaluated pose. Every
//   other bone's Transform holds the pose already.
// - A bone whose parent is not a bone (an armature node, the model) is a root: no segment to draw,
//   only its marker. It has one however deep it sits.
// - Reads the Animator's runtime when it has one (it played or previewed): this makes none, so an
//   Animator that never ran draws its Transforms, which are its rest, and models with no overlay on
//   never come here.
// - Thread-safety: main thread. Editor only.

#pragma once

#include "ecs/Entity.h"
#include "math/Vector3.h"

#include <vector>

namespace jasperengine{
    class Scene;
    struct ModelComponent;
}

namespace jasperengine::editor{

    struct BoneOverlaySegment{
        Vector3 bone = Vector3(0, 0, 0);    // world
        Vector3 parent = Vector3(0, 0, 0);  // world, when hasParent
        bool hasParent = false;
    };

    std::vector<BoneOverlaySegment> boneOverlaySegments(Scene* scene, const ModelComponent& model);

    // the Transform of `entity` is not what plays: a bone of an Animator with Write Bones: Exposed
    // that the Animator does not write (the Inspector says so)
    bool isUnwrittenAnimatorBone(Scene* scene, Entity entity);

}
