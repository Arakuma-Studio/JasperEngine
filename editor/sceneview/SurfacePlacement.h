// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Resting a dropped object on the surface under the pointer (docs/specs/S26-scene-view.md R16):
// its bottom (the deepest point of its bounds along the surface normal) on the hit point, and
// with Align to Surface its up axis turned onto the normal. Shared by the Scene view's drops,
// the prefab drag preview and the commands that make the dropped objects (so redo lands the
// same).

#include "Scene.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

namespace jasperengine::editor::sceneview {

    // The bounds of root and everything below it, in root's local space (its own mesh and UI
    // bounds included), from the local transforms (valid before the hierarchy's world matrices
    // are updated). Null when nothing below has bounds.
    AABB familyLocalBounds(Scene* scene, Entity root);

    // The rotation turning unit vector a onto unit vector b
    Quaternion rotationBetween(const Vector3& a, const Vector3& b);

    // The position (and rotation) that rest an object of local bounds `localBounds`, scale and
    // rotation on a surface point with normal: align turns its up axis onto the normal first
    void restOnSurface(const Vector3& point, const Vector3& normal, const AABB& localBounds, const Vector3& scale,
                       const Quaternion& rotation, bool align, Vector3& position, Quaternion& outRotation);

}
