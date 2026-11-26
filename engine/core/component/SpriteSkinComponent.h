// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPRITESKIN_COMPONENT_H
#define SPRITESKIN_COMPONENT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine{

    struct SpriteSkinGeometry;   // sprite/SpriteSheet.h

    // Unity's Sprite Skin (2D Animation; docs/specs/S94-rendering-anim-gaps.md F49): deforms the
    // Sprite on its GameObject with the bones of its frame's skin, each bone driven by one
    // GameObject's Transform. The deformation is the skinned-mesh vertex path (a bone palette and
    // per-vertex weights, MeshSystem builds the mesh, SpriteSkinning fills the palette), so it costs
    // nothing without the component and draws the same on every backend. A frame without a skin
    // draws as its quad.
    struct JASPERENGINE_API SpriteSkinComponent{
        // component checkbox: off draws the frame as its quad
        bool enabled = true;
        // the skeleton's top GameObject (Unity's Root Bone); informative for tools, the bones list
        // is what drives the mesh
        Entity rootBone = NULL_ENTITY;
        // one GameObject per bone of the frame's skin, in the skin's bone order; a missing one
        // (NULL_ENTITY, destroyed) keeps its bone in the bind pose
        std::vector<Entity> bones;

        // runtime, not saved: what MeshSystem built the mesh for (SpriteSkinning reads them)
        std::shared_ptr<const SpriteSkinGeometry> builtSkin;
        // the bind pose of each bone in the Sprite's local space, inverted
        std::vector<Matrix4> inverseBindMatrices;
        // the mesh holds the skinned layout (bone ids and weights): a change reloads it
        bool builtSkinned = false;
        // the Sprite's Flip X the mesh was built with: the vertices mirrored about the pivot, and
        // each palette entry conjugated by that mirror, so the bones still move the unmirrored rig
        bool builtFlipX = false;
        // the palette is filled again on the next render update, moved or not (a rebuilt mesh)
        bool paletteDirty = false;
        // the bones the palette was last filled from (another list fills it again)
        std::vector<Entity> paletteBones;
    };

}

#endif //SPRITESKIN_COMPONENT_H
