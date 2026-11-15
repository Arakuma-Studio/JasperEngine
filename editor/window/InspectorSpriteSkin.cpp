// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sprite Skin in the Inspector (docs/specs/S94-rendering-anim-gaps.md F49): its rows (Root
// Bone, Bones), what the shown frame's skin holds, and the buttons Unity's Sprite Skin has: Edit
// Skin (the Skinning Editor on this Sprite), Create Bones (the skin's bone GameObjects at the rest
// pose, bound) and Reset Bind Pose (the bone GameObjects back to the rest pose, one undo step).

#include "InspectorWindow.h"
#include "SkinningEditorWindow.h"

#include "Out.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/MultiPropertyCmd.h"
#include "external/IconsFontAwesome6.h"
#include "sprite/SpriteSkinEdit.h"
#include "util/UIProbe.h"

#include "component/SpriteSkinComponent.h"
#include "math/Quaternion.h"
#include "sprite/SpriteSkinning.h"

#include "imgui.h"

using namespace jasperengine;

void editor::InspectorWindow::drawSpriteSkinComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    drawDefaultComponent(cpType, sceneProject, entities);
    if (entities.size() != 1) return;
    const Entity entity = entities[0];
    Scene* scene = sceneProject->scene;
    const SpriteSkinComponent& component = scene->getComponent<SpriteSkinComponent>(entity);

    sprite::SkinTarget target;
    std::string error;
    if (!sprite::resolveSkinTarget(project, sceneProject->id, entity, target, error)) {
        ImGui::TextWrapped("%s", error.c_str());
        return;
    }
    const SpriteFrame& frame = target.frameData();
    if (!frame.skin || frame.skin->bones.empty()) {
        ImGui::TextDisabled("Frame '%s' has no skin: Edit Skin to add bones, geometry and weights.", frame.name.c_str());
    } else {
        ImGui::TextDisabled("Frame '%s': %zu bones, %zu vertices%s", frame.name.c_str(), frame.skin->bones.size(), frame.skin->vertices.size(),
                            component.builtSkinned ? ", drawn skinned" : "");
        if (component.bones.size() != frame.skin->bones.size()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Bones lists %zu GameObjects for %zu bones: Create Bones binds them.",
                               component.bones.size(), frame.skin->bones.size());
        }
    }

    if (ImGui::Button(ICON_FA_BONE " Edit Skin")) SkinningEditorWindow::requestOpen(sceneProject->id, entity);
    ImGui::SetItemTooltip("Open the Skinning Editor on this Sprite");
    UIProbe::item("spriteskin.edit_skin");
    ImGui::SameLine();
    ImGui::BeginDisabled(!frame.skin || frame.skin->bones.empty());
    if (ImGui::Button("Create Bones")) {
        if (!sprite::applySkinEdit(target, *frame.skin, {}, true, error)) Out::warning("Create Bones: %s", error.c_str());
    }
    ImGui::SetItemTooltip("A GameObject per bone under this Sprite at the rest pose, bound to the Sprite Skin");
    ImGui::SameLine();
    if (ImGui::Button("Reset Bind Pose")) {
        // every bound bone GameObject back where its bone rests
        auto* multi = new MultiPropertyCmd();
        const SpriteFrameGeometryInput input = sprite::skinGeometryInput(scene, entity, *target.sheet, frame);
        for (size_t b = 0; b < component.bones.size() && b < frame.skin->bones.size(); b++) {
            const Entity bone = component.bones[b];
            if (bone == NULL_ENTITY || !scene->isEntityCreated(bone)) continue;
            const SpriteBoneRest rest = spriteBoneRestTransform(frame, *frame.skin, input, static_cast<int>(b));
            multi->addPropertyCmd<Vector3>(project, sceneProject->id, bone, ComponentType::Transform, "position", rest.position);
            multi->addPropertyCmd<Quaternion>(project, sceneProject->id, bone, ComponentType::Transform, "rotation", Quaternion(0.0f, 0.0f, rest.rotationZ));
        }
        multi->setNoMerge();
        CommandHandle::get(sceneProject->id)->addCommand(multi);
    }
    ImGui::SetItemTooltip("The bone GameObjects back to the skin's rest pose");
    ImGui::EndDisabled();
}
