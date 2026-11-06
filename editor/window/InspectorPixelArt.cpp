// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pixel Perfect Camera's Inspector (docs/specs/S41-pixel-perfect-camera.md R19): its fields
// with Unity's names, Reference Resolution's Use Project Canvas, the Current Pixel Ratio for the
// Game view's size (the live one in Play) and the warnings. Every edit is one scene undo step.

#include "InspectorWindow.h"

#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/CameraComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/PixelPerfectEditor.h"
#include "reflect/Components.h"
#include "subsystem/RenderSystem.h"
#include "util/UIProbe.h"
#include "Theme.h"

#include "imgui.h"

using namespace jasperengine;

namespace {

    void drawPixelArtWarning(const std::string& text){
        ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
        ImGui::PopStyleColor();
    }

}

void editor::InspectorWindow::drawPixelPerfectCameraComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const Entity entity = entities[0];
    PixelPerfectCameraComponent& component = scene->getComponent<PixelPerfectCameraComponent>(entity);
    const bool playing = sceneProject->playState != ScenePlayState::STOPPED;

    // Reference Resolution: 0 x 0 is the project canvas
    const bool useCanvas = component.referenceWidth == 0 && component.referenceHeight == 0;
    char canvasLabel[96];
    std::snprintf(canvasLabel, sizeof(canvasLabel), "Use Project Canvas (%ux%u)##pixelperfect_canvas", project->getCanvasWidth(),
                  project->getCanvasHeight());
    bool checked = useCanvas;
    ImGui::TextUnformatted("Reference Resolution");
    ImGui::SameLine();
    if (ImGui::Checkbox(canvasLabel, &checked)){
        const unsigned int width = checked ? 0u : project->getCanvasWidth();
        const unsigned int height = checked ? 0u : project->getCanvasHeight();
        MultiPropertyCmd* multi = new MultiPropertyCmd();
        for (Entity selected : entities){
            multi->addPropertyCmd<unsigned int>(project, sceneId, selected, cpType, "referenceWidth", width);
            multi->addPropertyCmd<unsigned int>(project, sceneId, selected, cpType, "referenceHeight", height);
        }
        CommandHandle::get(sceneId)->addCommandNoMerge(multi);
    }
    UIProbe::item("pixelperfectcamera.use_project_canvas");

    RowSettings sizeSettings;
    sizeSettings.stepSize = 1.0f;
    sizeSettings.secondColSize = 6 * ImGui::GetFontSize();
    sizeSettings.help = "The base resolution the art is drawn at, in pixels.";
    RowSettings modeSettings;
    modeSettings.enumEntries = enumEntries(reflect::enums::kPixelUpscaleMode);
    modeSettings.help = "Integer (Letterbox): every base pixel k x k screen pixels, bars around. Integer (Expand): the same k, the base "
                        "grows to fill the screen. Stretch Fill: the integer image stretched to fill, sharp at its seams.";
    RowSettings textureSettings;
    textureSettings.help = "Draws the base resolution into a texture scaled by a whole number. Off: draws straight at the screen's "
                           "resolution on the same grid.";
    RowSettings snapSettings;
    snapSettings.help = "Puts the camera and every sprite, tile, UI element and particle on whole base pixels.";
    RowSettings rotationSettings;
    rotationSettings.enumEntries = enumEntries(reflect::enums::kPixelRotation,
                                               reflect::enums::bit((int)PixelRotation::STEPS_90) | reflect::enums::bit((int)PixelRotation::ON_GRID));
    rotationSettings.help = "90 Degree Steps: rotations are drawn rounded to the nearest 90 degrees (the Transform keeps its angle). "
                            "Rotate On Grid: the angle is kept, the pivot snaps.";
    RowSettings smoothingSettings;
    smoothingSettings.help = "Scrolls by whole screen pixels between base pixels: the image moves, the art stays on its grid.";
    RowSettings colorSettings;
    colorSettings.colorSrgb = true;

    beginTable(cpType, getLabelSize("Upscale Render Texture"), "pixelperfectcamera");
    if (!useCanvas){
        propertyRow(RowPropertyType::UInt, cpType, "referenceWidth", "Width", sceneProject, entities, sizeSettings);
        propertyRow(RowPropertyType::UInt, cpType, "referenceHeight", "Height", sceneProject, entities, sizeSettings);
    }
    propertyRow(RowPropertyType::Enum, cpType, "upscaleMode", "Upscale Mode", sceneProject, entities, modeSettings);
    propertyRow(RowPropertyType::Bool, cpType, "upscaleRenderTexture", "Upscale Render Texture", sceneProject, entities, textureSettings);
    propertyRow(RowPropertyType::Bool, cpType, "pixelSnapping", "Pixel Snapping", sceneProject, entities, snapSettings);
    propertyRow(RowPropertyType::Enum, cpType, "rotationMode", "Rotation", sceneProject, entities, rotationSettings);
    propertyRow(RowPropertyType::Bool, cpType, "subpixelSmoothing", "Subpixel Smoothing", sceneProject, entities, smoothingSettings);
    propertyRow(RowPropertyType::Color4L, cpType, "letterboxColor", "Letterbox Color", sceneProject, entities, colorSettings);
    endTable();

    if (entities.size() != 1)
        return;

    // Current Pixel Ratio: the Game view's (the renderer's own while it plays)
    pixelart::refreshEditLayout(*project, scene, entity, playing);
    int screenWidth = 0, screenHeight = 0;
    pixelart::gameViewSize(*project, screenWidth, screenHeight);
    if (component.layout.baseWidth > 0){
        const std::string readout = pixelart::describeLayout(component.layout, screenWidth, screenHeight);
        ImGui::TextDisabled("Current Pixel Ratio: %s", readout.c_str());
    }else{
        ImGui::TextDisabled("Current Pixel Ratio: -");
    }
    UIProbe::item("pixelperfectcamera.pixel_ratio");

    for (const std::string& warning : pixelart::pixelPerfectWarnings(scene, entity, sceneProject->mainCamera))
        drawPixelArtWarning(warning);

    // R5: a layer scene's camera (a HUD drawn over the main scene) takes the main scene's layout
    if (pixelart::isLayerScene(*project, sceneProject->id))
        ImGui::TextDisabled("Layer scene: takes the main scene's layout and draws on its grid directly (Upscale Render Texture is ignored).");
}
