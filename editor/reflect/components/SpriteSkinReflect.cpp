// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sprite Skin (docs/specs/S94-rendering-anim-gaps.md F49): Unity's 2D Animation component.
// Field order is the YAML key order. The bones list is the frame skin's bone order.

#include "reflect/Components.h"

#include "Factory.h"
#include "component/SpriteSkinComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    constexpr FieldDesc kSpriteSkinFields[] = {
        JE_FIELD(SpriteSkinComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(SpriteSkinComponent, rootBone, .ops = &ops::Entity, .flags = OmitIfDefault | CodegenIfChanged, .label = "Root Bone",
                 .tooltip = "The top GameObject of the skeleton."),
        JE_FIELD(SpriteSkinComponent, bones, .flags = ListWhole | ListElements | OmitIfDefault | CodegenIfChanged,
                 .tooltip = "One GameObject per bone of the sprite's skin, in the skin's bone order.", .elementOps = &ops::Entity),
    };

    constexpr const char* kSpriteSkinCodegenOrder[] = {"rootBone", "enabled"};

    // the bone list (an entity list has no literal of its own), each entity as the scene's code names it
    void spriteSkinCodegen(const void* comp, const CodegenContext& ctx, std::string& out) {
        const SpriteSkinComponent& skin = *static_cast<const SpriteSkinComponent*>(comp);
        if (skin.bones.empty()) return;
        std::string list;
        for (Entity bone : skin.bones) list += (list.empty() ? "" : ", ") + Factory::formatEntity(bone, ctx.entityVarNames);
        out += Factory::indentation(ctx.indentSpaces) + ctx.variable + ".bones = {" + list + "};\n";
    }

}

const ComponentDesc kSpriteSkinDesc{
    .type = ComponentType::SpriteSkinComponent, .cppName = "SpriteSkinComponent", .key = "spriteskin",
    .displayName = "Sprite Skin", .icon = ICON_FA_BONE,
    .body = JE_STRUCT_DESC(SpriteSkinComponent, "SpriteSkinComponent", kSpriteSkinFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SpriteSkinComponent),
    .codegenVar = "spriteSkin", .codegenOrder = kSpriteSkinCodegenOrder, .codegenExtra = &spriteSkinCodegen,
};

}
