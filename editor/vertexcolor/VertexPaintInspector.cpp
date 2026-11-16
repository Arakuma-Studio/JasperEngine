// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexPaintInspector.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "component/ModelComponent.h"
#include "external/IconsFontAwesome6.h"
#include "util/UIProbe.h"
#include "vertexcolor/VertexPaintTool.h"

#include "imgui.h"

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

void editor::vertexcolor::paintVertexColorsButton(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, bool fromModel) {
    if (!project || !sceneProject || !sceneProject->scene || entities.size() != 1) return;
    if (sceneProject->playState != ScenePlayState::STOPPED) return;
    const Entity entity = entities[0];
    if (!fromModel && sceneProject->scene->findComponent<ModelComponent>(entity)) return;
    std::string why;
    const bool paintable = canPaint(project, sceneProject->id, entity, &why);
    // a mesh nobody could paint (no geometry yet) says nothing; a generated one says whose it is
    if (!paintable && why.empty()) return;
    ImGui::BeginDisabled(!paintable);
    const bool clicked = ImGui::Button(ICON_FA_PAINTBRUSH " Paint Vertex Colors", ImVec2(-1.0f, 0.0f));
    ImGui::EndDisabled();
    UIProbe::item(fromModel ? "inspector.model.paintVertexColors" : "inspector.mesh.paintVertexColors");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", paintable ? "Adds Vertex Colors if missing and opens the Vertex Paint window with the Paint tool" : why.c_str());
    }
    if (clicked) {
        std::string error;
        if (!openVertexPaint(project, sceneProject->id, entity, &error) && !error.empty()) Out::warning("Vertex Paint: %s", error.c_str());
    }
}
