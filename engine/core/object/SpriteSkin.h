// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sprite Skin of a GameObject (docs/specs/S94-rendering-anim-gaps.md F49): Lua
// `SpriteSkin(scene, entity)` and C++ scripts. It ADDS a Sprite Skin when the GameObject has none.
// The bones are GameObjects in the order of the frame skin's bones; moving them bends the Sprite.
// Main thread only (ECS).

#ifndef SPRITE_SKIN_H
#define SPRITE_SKIN_H

#include "object/EntityHandle.h"
#include "component/SpriteSkinComponent.h"

#include <vector>

namespace jasperengine{

    class JASPERENGINE_API SpriteSkin: public EntityHandle{

    public:
        SpriteSkin(Scene* scene, Entity entity);
        virtual ~SpriteSkin();

        SpriteSkin(const SpriteSkin& rhs);
        SpriteSkin& operator=(const SpriteSkin& rhs);

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setRootBone(Entity bone);
        Entity getRootBone() const;
        // one GameObject per bone of the frame's skin, in its bone order
        void setBones(const std::vector<Entity>& bones);
        std::vector<Entity> getBones() const;
        // the bones of the shown frame's skin (0: the frame has none, the Sprite draws its quad)
        int getSkinBoneCount() const;
        // the Sprite draws through its bones now (the frame has a skin, the component is enabled)
        bool isSkinned() const;
    };

}

#endif //SPRITE_SKIN_H
