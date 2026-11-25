// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sprite Skin (docs/specs/S94-rendering-anim-gaps.md F49): Unity's 2D Animation component.
// Field order is the YAML key order. The bones list is the frame skin's bone order.

#include "reflect/Components.h"

#include "component/SpriteSkinComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    constexpr FieldDesc kSpriteSkinFields[] = {
        JE_FIELD(SpriteSkinComponent, enabled),
        JE_FIELD(SpriteSkinComponent, rootBone, .ops = &ops::Entity, .flags = OmitIfDefault, .label = "Root Bone",
                 .tooltip = "The top GameObject of the skeleton."),
        JE_FIELD(SpriteSkinComponent, bones, .flags = ListWhole | ListElements | OmitIfDefault,
                 .tooltip = "One GameObject per bone of the sprite's skin, in the skin's bone order.", .elementOps = &ops::Entity),
    };

}

const ComponentDesc kSpriteSkinDesc{
    .type = ComponentType::SpriteSkinComponent, .cppName = "SpriteSkinComponent", .key = "spriteskin",
    .displayName = "Sprite Skin", .icon = ICON_FA_BONE,
    .body = JE_STRUCT_DESC(SpriteSkinComponent, "SpriteSkinComponent", kSpriteSkinFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SpriteSkinComponent),
};

}
