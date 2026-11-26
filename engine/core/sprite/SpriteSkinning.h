// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 2D sprite skinning (docs/specs/S94-rendering-anim-gaps.md F49): the geometry a frame's skin
// gives a Sprite, and the bone palette its Sprite Skin feeds the skinned-mesh vertex path.
// - Space: a skin's vertices and bones are the frame's source pixels; on the Sprite they land
//   where the frame's quad puts those pixels (SpriteFrameGeometry: pivot, size, Pixels Per
//   Unit, the camera's y and the flip), so a skinned frame at rest covers what its quad covers.
// - Bones: bone i's bind pose is a frame at its head, its x axis along head -> tail, in the
//   Sprite's local space. Palette entry i = inverse(sprite world) * bone GameObject world *
//   inverse(bind i): a bone GameObject placed at its bind pose (spriteBoneRestTransform) leaves
//   the mesh as drawn.
// - Pure functions but updateSpriteSkinBones (main thread, the render update).

#ifndef SPRITE_SKINNING_H
#define SPRITE_SKINNING_H

#include "Export.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "sprite/SpriteFrameGeometry.h"
#include "sprite/SpriteSheet.h"

#include <vector>

namespace jasperengine{

    class EntityRegistry;

    // a source pixel of the frame in the Sprite's local units (where its quad draws that pixel)
    JASPERENGINE_API Vector2 spriteFrameLocalPoint(const SpriteFrame& frame, const SpriteFrameGeometryInput& input, Vector2 pixel);

    // bone `bone`'s bind pose in the Sprite's local space
    JASPERENGINE_API Matrix4 spriteBoneBindMatrix(const SpriteFrame& frame, const SpriteSkinGeometry& skin,
                                                  const SpriteFrameGeometryInput& input, int bone);

    // the Transform a bone GameObject takes for the rest pose: relative to its parent bone's
    // GameObject, or to the Sprite for a root bone; rotation about z in the engine's angle unit
    struct JASPERENGINE_API SpriteBoneRest{
        Vector3 position;
        float rotationZ = 0.0f;
    };
    JASPERENGINE_API SpriteBoneRest spriteBoneRestTransform(const SpriteFrame& frame, const SpriteSkinGeometry& skin,
                                                            const SpriteFrameGeometryInput& input, int bone);

    // what the frame's skin draws: local positions, UVs inside the frame rect (v down, as the
    // texture rect), and each bone's bind-space bounds of the vertices it moves
    struct JASPERENGINE_API SpriteSkinnedVertices{
        std::vector<Vector3> positions;
        std::vector<Vector2> uvs;
        std::vector<Matrix4> inverseBindMatrices;
        std::vector<AABB> boneBounds;
    };
    JASPERENGINE_API SpriteSkinnedVertices spriteSkinnedVertices(const SpriteFrame& frame, const SpriteSkinGeometry& skin,
                                                                 const SpriteFrameGeometryInput& input);

    // fills the bone palettes of the scene's skinned Sprites whose bones or Sprite moved (the
    // render update, after transforms propagate; nothing without a Sprite Skin)
    JASPERENGINE_API void updateSpriteSkinBones(EntityRegistry* registry);

}

#endif // SPRITE_SKINNING_H
