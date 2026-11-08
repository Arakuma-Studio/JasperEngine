// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The ImGui side of SceneViewController: the Scene view toolbar items of S26 (R32), the panes'
// headers, the overlays (pill, rulers, measures, "N hidden") and the view's own shortcuts.
// Behaviour and constants of OpenEngine's CameraBookmarksWidget, SceneCameraSettingsPopup and
// SceneViewRulerOverlay (Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License, NOTICE.md); the widgets are ours.

#include "SceneViewController.h"

#include "AppSettings.h"
#include "Backend.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "navigation/NavPathTester.h"
#include "object/Camera.h"
#include "render/SceneRender.h"
#include "sceneview/SceneViewVisibility.h"
#include "window/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* axisLabel(SceneViewAxis axis){
        switch (axis){
            case SceneViewAxis::FREE: return "Persp";
            case SceneViewAxis::TOP: return "Top";
            case SceneViewAxis::BOTTOM: return "Bottom";
            case SceneViewAxis::FRONT: return "Front";
            case SceneViewAxis::BACK: return "Back";
            case SceneViewAxis::LEFT: return "Left";
            case SceneViewAxis::RIGHT: return "Right";
        }
        return "Persp";
    }

    // a toolbar button drawn pressed while `on`
    bool toggleButton(const char* label, bool on, const char* tooltip){
        ImGui::PushStyleColor(ImGuiCol_Button, on ? Theme::Colors::ButtonActivated : ImGui::GetStyle().Colors[ImGuiCol_Button]);
        const bool pressed = ImGui::Button(label);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tooltip);
        return pressed;
    }

    float buttonWidth(const char* label){
        return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    ImU32 measureColor(bool selected){
        return selected ? IM_COL32(255, 200, 40, 255) : IM_COL32(80, 200, 255, 255);
    }

    // a point of the world in the view (logical pixels from viewMin); false behind the camera
    bool projectToView(const Matrix4& viewProjection, const Vector3& world, ImVec2 viewMin, ImVec2 viewSize, ImVec2& out){
        const Vector4 clip = viewProjection * Vector4(world.x, world.y, world.z, 1.0f);
        if (clip.w <= 1e-6f){
            return false;
        }
        out = ImVec2(viewMin.x + (clip.x / clip.w * 0.5f + 0.5f) * viewSize.x, viewMin.y + (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * viewSize.y);
        return true;
    }

    void drawPill(ImDrawList* drawList, ImVec2 at, const std::string& text){
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 pad = Theme::dpi(ImVec2(6.0f, 3.0f));
        drawList->AddRectFilled(at, ImVec2(at.x + size.x + pad.x * 2.0f, at.y + size.y + pad.y * 2.0f), IM_COL32(20, 20, 24, 220), Theme::dpi(8.0f));
        drawList->AddText(ImVec2(at.x + pad.x, at.y + pad.y), IM_COL32(240, 240, 240, 255), text.c_str());
    }

}

// ---- toolbar

void SceneViewController::openCameraSettings(uint32_t sceneId){
    cameraPopupScene = sceneId;
}

void SceneViewController::drawCameraSettings(SceneProject& sceneProject){
    SceneCameraSettings settings = sceneProject.viewState.camera;
    bool changed = false;
    bool finished = false;
    ImGui::TextUnformatted("Scene Camera");
    ImGui::Separator();
    if (ImGui::BeginTable("##SceneCameraRows", 2, ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(140.0f));
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(160.0f));
        auto row = [](const char* label){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
        };
        row("Field of View");
        changed |= ImGui::SliderFloat("##fov", &settings.fieldOfView, SceneCameraSettings::MIN_FOV, SceneCameraSettings::MAX_FOV, "%.0f\xC2\xB0");
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        row("Dynamic Clipping");
        changed |= ImGui::Checkbox("##dynamicClipping", &settings.dynamicClipping);
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("Near and far follow the distance to the target");
        ImGui::BeginDisabled(settings.dynamicClipping);
        row("Near");
        changed |= ImGui::DragFloat("##near", &settings.nearClip, 0.01f, 0.001f, 1000.0f, "%.3f");
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        row("Far");
        changed |= ImGui::DragFloat("##far", &settings.farClip, 1.0f, 1.0f, 1000000.0f, "%.0f");
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::EndDisabled();
        row("Camera Speed");
        changed |= ImGui::DragFloat("##speed", &settings.speed, 0.05f, SceneCameraSettings::MIN_SPEED, SceneCameraSettings::MAX_SPEED, "%.2f",
                                    ImGuiSliderFlags_Logarithmic);
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("Fly speed (right mouse + WASD); the wheel changes it while flying");
        row("Speed Min");
        changed |= ImGui::DragFloat("##speedMin", &settings.speedMin, 0.01f, SceneCameraSettings::MIN_SPEED, SceneCameraSettings::MAX_SPEED, "%.2f",
                                    ImGuiSliderFlags_Logarithmic);
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        row("Speed Max");
        changed |= ImGui::DragFloat("##speedMax", &settings.speedMax, 1.0f, SceneCameraSettings::MIN_SPEED, SceneCameraSettings::MAX_SPEED, "%.1f",
                                    ImGuiSliderFlags_Logarithmic);
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        row("Shift Multiplier");
        changed |= ImGui::DragFloat("##shift", &settings.shiftMultiplier, 0.05f, 1.0f, 20.0f, "%.2f");
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        row("Camera Acceleration");
        changed |= ImGui::Checkbox("##acceleration", &settings.acceleration);
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("The fly ramps from 25%% to full speed over 0.6 s");
        row("Camera Easing");
        changed |= ImGui::Checkbox("##easing", &settings.easing);
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("Frame, axis views and bookmarks move over 0.25 s");
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset")){
        settings = SceneCameraSettings();
        changed = true;
        finished = true;
    }
    if (changed){
        applyCameraSettings(sceneProject, settings);
    }
    if (finished){
        saveWorkspace();
    }
}

void SceneViewController::drawToolbar(SceneProject& sceneProject, float rightLimit){
    SceneViewState& state = sceneProject.viewState;
    Runtime& rt = runtime(sceneProject.id);
    const bool is3D = sceneProject.sceneType == SceneType::SCENE_3D;
    const std::string sid = std::to_string(sceneProject.id);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    SceneViewSettings appSettings = AppSettings::getSceneViewSettings();

    // ---- bookmarks strip: Home, the stored slots, +
    ImGui::SameLine(0, Theme::dpi(10.0f));
    for (int slot = 0; slot < SceneViewState::BOOKMARK_SLOTS; slot++){
        const CameraBookmark& bookmark = state.bookmarks[(size_t)slot];
        if (slot > 0 && !bookmark.defined){
            continue;
        }
        const std::string label = (slot == 0 ? std::string(ICON_FA_HOUSE) : std::to_string(slot)) + "##Bookmark" + std::to_string(slot) + "_" + sid;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x * 0.6f, ImGui::GetStyle().FramePadding.y));
        const bool pressed = ImGui::Button(label.c_str());
        ImGui::PopStyleVar();
        std::string error;
        if (pressed){
            if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper){
                storeBookmark(sceneProject, slot, error);
            }else{
                recallBookmark(sceneProject, slot, error);
            }
        }
        if (ImGui::BeginItemTooltip()){
            ImGui::TextUnformatted(bookmarkName(state, slot).c_str());
            if (bookmark.defined){
                ImGui::TextDisabled("%s", bookmark.pose.orthographic ? "Orthographic" : "Perspective");
                if (Texture* preview = bookmarkPreview(sceneProject, slot)){
                    Widgets::image(Backend::getImGuiTexture(preview->getRender()),
                                   Theme::dpi(ImVec2((float)BOOKMARK_PREVIEW_WIDTH, (float)BOOKMARK_PREVIEW_HEIGHT)));
                }
            }else{
                ImGui::TextDisabled("Default view");
            }
            ImGui::TextDisabled("Click: go there. Ctrl+click: store the view here.");
            if (slot > 0){
                const std::string number = std::to_string(slot);
                const std::string recall = sceneViewShortcut(("sceneView.recallBookmark" + number).c_str());
                const std::string store = sceneViewShortcut(("sceneView.storeBookmark" + number).c_str());
                if (!recall.empty() || !store.empty()){
                    ImGui::TextDisabled("%s%s%s", recall.empty() ? "" : (recall + " recalls").c_str(), !recall.empty() && !store.empty() ? ", " : "",
                                        store.empty() ? "" : (store + " stores").c_str());
                }
            }
            ImGui::EndTooltip();
        }
        const std::string menuId = "BookmarkMenu" + std::to_string(slot) + "_" + sid;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)){
            ImGui::OpenPopup(menuId.c_str());
        }
        if (ImGui::BeginPopup(menuId.c_str())){
            if (bookmark.defined && slot > 0 && ImGui::BeginMenu("Rename")){
                if (rt.renameSlot != slot){
                    rt.renameSlot = slot;
                    std::snprintf(rt.renameBuffer, sizeof(rt.renameBuffer), "%s", bookmark.name.c_str());
                }
                if (ImGui::InputText("##rename", rt.renameBuffer, sizeof(rt.renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)){
                    renameBookmark(sceneProject, slot, rt.renameBuffer, error);
                    rt.renameSlot = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Overwrite")){
                storeBookmark(sceneProject, slot, error);
            }
            if (ImGui::MenuItem(slot == 0 ? "Reset" : "Delete", nullptr, false, bookmark.defined)){
                deleteBookmark(sceneProject, slot, error);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, Theme::dpi(2.0f));
    }
    {
        int freeSlot = -1;
        for (int slot = 1; slot < SceneViewState::BOOKMARK_SLOTS && freeSlot < 0; slot++){
            if (!state.bookmarks[(size_t)slot].defined) freeSlot = slot;
        }
        ImGui::BeginDisabled(freeSlot < 0);
        if (ImGui::Button((std::string(ICON_FA_PLUS) + "##BookmarkAdd" + sid).c_str())){
            std::string error;
            storeBookmark(sceneProject, freeSlot, error);
        }
        ImGui::EndDisabled();
        const std::string storeKey = freeSlot > 0 ? sceneViewShortcut(("sceneView.storeBookmark" + std::to_string(freeSlot)).c_str()) : std::string();
        ImGui::SetItemTooltip("Store this view as a camera bookmark%s", storeKey.empty() ? "" : (" (" + storeKey + ")").c_str());
    }

    // ---- the middle items, or a ... menu when they do not fit
    struct Item {
        std::string label;
        float width;
    };
    const std::string layoutLabel = std::string(ICON_FA_TABLE_CELLS_LARGE " ") + ICON_FA_CARET_DOWN + "##Layout" + sid;
    const std::string projectionLabel = std::string(isOrthographic(sceneProject) ? "Iso" : "Persp") + "##Projection" + sid;
    const std::string layersLabel = std::string("Layers ") + ICON_FA_CARET_DOWN + "##Layers" + sid;
    const std::string measureLabel = std::string(ICON_FA_RULER) + "##Measure" + sid;
    const std::string rulersLabel = std::string(ICON_FA_RULER_COMBINED) + "##Rulers" + sid;
    const std::string exactLabel = std::string(ICON_FA_CROSSHAIRS) + "##ExactPick" + sid;
    const std::string alignLabel = std::string(ICON_FA_ARROWS_DOWN_TO_LINE) + "##Align" + sid;
    const std::string cameraLabel = std::string(ICON_FA_CAMERA) + "##SceneCamera" + sid;
    // the Path Tester (S75 R25), in scenes with a NavMesh Surface or a Nav Grid
    const std::string pathTesterLabel = std::string(ICON_FA_ROUTE) + "##PathTester" + sid;
    const bool hasNavigation = sceneProject.scene && (sceneProject.scene->getComponentArray<NavMeshSurfaceComponent>()->size() > 0 ||
                                                      sceneProject.scene->getComponentArray<NavGridComponent>()->size() > 0);
    NavPathTester& pathTester = NavPathTester::get();
    const std::string pathTesterShortcut = sceneViewShortcut("navigation.pathTester");
    const std::string pathTesterTooltip = std::string("Path Tester: Shift+click sets the start, click the end, drag a handle to move it (") +
                                          (pathTesterShortcut.empty() ? "" : pathTesterShortcut + "; ") + "Esc ends it)";
    float middle = 0.0f;
    for (const std::string* label : {&layoutLabel, &projectionLabel, &layersLabel, &measureLabel, &rulersLabel, &exactLabel, &alignLabel}){
        middle += buttonWidth(label->c_str()) + spacing;
    }
    if (hasNavigation){
        middle += buttonWidth(pathTesterLabel.c_str()) + spacing;
    }
    const float cameraWidth = buttonWidth(cameraLabel.c_str()) + spacing;
    ImGui::SameLine(0, Theme::dpi(8.0f));
    const bool overflow = ImGui::GetCursorPosX() + middle + cameraWidth > rightLimit;

    const std::string layersPopup = "LayersPopup" + sid;
    const std::string layoutPopup = "LayoutPopup" + sid;
    auto drawLayersMenu = [&](){
        SceneLayerFilter filter = state.layerFilter;
        bool changed = false;
        if (ImGui::Selectable("Everything", filter.visible == LAYER_MASK_EVERYTHING && filter.pickable == LAYER_MASK_EVERYTHING,
                              ImGuiSelectableFlags_DontClosePopups)){
            filter.visible = LAYER_MASK_EVERYTHING;
            filter.pickable = LAYER_MASK_EVERYTHING;
            changed = true;
        }
        if (ImGui::Selectable("Nothing", filter.visible == LAYER_MASK_NOTHING, ImGuiSelectableFlags_DontClosePopups)){
            filter.visible = LAYER_MASK_NOTHING;
            changed = true;
        }
        ImGui::Separator();
        for (int layer = 0; layer < 32; layer++){
            const std::string name = TagManager::layerToName(layer);
            if (name.empty()){
                continue;
            }
            const uint32_t bit = 1u << layer;
            const bool visible = (filter.visible & bit) != 0;
            const bool pickable = (filter.pickable & bit) != 0;
            ImGui::PushID(layer);
            if (ImGui::SmallButton(visible ? ICON_FA_EYE : ICON_FA_EYE_SLASH)){
                filter.visible ^= bit;
                changed = true;
            }
            ImGui::SetItemTooltip(visible ? "Shown in the Scene view (click to hide)" : "Hidden in the Scene view (click to show)");
            ImGui::SameLine();
            if (ImGui::SmallButton(pickable ? ICON_FA_LOCK_OPEN : ICON_FA_LOCK)){
                filter.pickable ^= bit;
                changed = true;
            }
            ImGui::SetItemTooltip(pickable ? "Pickable (click to lock)" : "Locked: drawn, never picked (click to unlock)");
            ImGui::SameLine();
            ImGui::Text("%d  %s", layer, name.c_str());
            ImGui::PopID();
        }
        if (changed){
            setLayerFilter(sceneProject, filter.visible, filter.pickable);
        }
    };
    auto drawLayoutMenu = [&](){
        for (SceneViewLayout layout : {SceneViewLayout::SINGLE, SceneViewLayout::SPLIT, SceneViewLayout::QUAD}){
            const char* name = layout == SceneViewLayout::SINGLE ? "Single" : (layout == SceneViewLayout::SPLIT ? "Split (Persp | Top)" : "Quad (Top, Front, Side, Persp)");
            if (ImGui::MenuItem(name, nullptr, state.layout == layout)){
                setLayout(sceneProject, layout);
            }
        }
    };
    const std::string measureShortcut = sceneViewShortcut("sceneView.measure");
    auto measureTooltip = std::string("Measure: drag from A to B (") + (measureShortcut.empty() ? "" : measureShortcut + "; ") +
                          "V snaps to vertices, Shift shows the deltas)";

    if (!overflow){
        if (is3D){
            if (ImGui::Button(layoutLabel.c_str())) ImGui::OpenPopup(layoutPopup.c_str());
            ImGui::SetItemTooltip("Layout: Single, Split, Quad");
            if (ImGui::BeginPopup(layoutPopup.c_str())){
                drawLayoutMenu();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(isAxisLocked(sceneProject));
            if (ImGui::Button(projectionLabel.c_str())) toggleProjection(sceneProject);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Perspective / Orthographic (also: click the view gizmo's cube)");
            ImGui::SameLine();
        }
        if (ImGui::Button(layersLabel.c_str())) ImGui::OpenPopup(layersPopup.c_str());
        ImGui::SetItemTooltip("Layers: what the Scene view shows (eye) and picks (lock)");
        if (ImGui::BeginPopup(layersPopup.c_str())){
            drawLayersMenu();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (toggleButton(measureLabel.c_str(), rt.measuring, (measureTooltip + "; right-click: Clear Measures").c_str())){
            setMeasuring(sceneProject, !rt.measuring);
        }
        const std::string measureMenu = "MeasureMenu" + sid;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup(measureMenu.c_str());
        if (ImGui::BeginPopup(measureMenu.c_str())){
            if (ImGui::MenuItem("Measure", measureShortcut.empty() ? nullptr : measureShortcut.c_str(), rt.measuring)) setMeasuring(sceneProject, !rt.measuring);
            if (ImGui::MenuItem("Clear Measures", nullptr, false, !state.measures.empty())) clearMeasures(sceneProject);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (toggleButton(rulersLabel.c_str(), appSettings.rulers, "Rulers (2D, UI and orthographic axis views)")){
            appSettings.rulers = !appSettings.rulers;
            AppSettings::setSceneViewSettings(appSettings);
        }
        ImGui::SameLine();
        if (toggleButton(exactLabel.c_str(), appSettings.exactPick, "Exact Pick: a click selects what is under it, not its prefab or model root")){
            appSettings.exactPick = !appSettings.exactPick;
            AppSettings::setSceneViewSettings(appSettings);
        }
        if (is3D){
            ImGui::SameLine();
            if (toggleButton(alignLabel.c_str(), appSettings.alignToSurface, "Align to Surface: drops turn their up axis onto the surface (also Alt while dragging)")){
                appSettings.alignToSurface = !appSettings.alignToSurface;
                AppSettings::setSceneViewSettings(appSettings);
            }
        }
        if (hasNavigation){
            ImGui::SameLine();
            if (toggleButton(pathTesterLabel.c_str(), pathTester.isActive(sceneProject.id), pathTesterTooltip.c_str())){
                pathTester.toggle(sceneProject);
            }
        }
    }else{
        const std::string morePopup = "SceneViewMore" + sid;
        if (ImGui::Button((std::string(ICON_FA_ELLIPSIS) + "##More" + sid).c_str())) ImGui::OpenPopup(morePopup.c_str());
        ImGui::SetItemTooltip("More Scene view tools");
        if (ImGui::BeginPopup(morePopup.c_str())){
            if (is3D){
                if (ImGui::BeginMenu("Layout")){
                    drawLayoutMenu();
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(isOrthographic(sceneProject) ? "Perspective" : "Orthographic (Iso)", nullptr, false, !isAxisLocked(sceneProject))){
                    toggleProjection(sceneProject);
                }
            }
            if (ImGui::BeginMenu("Layers")){
                drawLayersMenu();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Measure", measureShortcut.empty() ? nullptr : measureShortcut.c_str(), rt.measuring)) setMeasuring(sceneProject, !rt.measuring);
            if (ImGui::MenuItem("Rulers", nullptr, appSettings.rulers)){
                appSettings.rulers = !appSettings.rulers;
                AppSettings::setSceneViewSettings(appSettings);
            }
            if (ImGui::MenuItem("Exact Pick", nullptr, appSettings.exactPick)){
                appSettings.exactPick = !appSettings.exactPick;
                AppSettings::setSceneViewSettings(appSettings);
            }
            if (is3D && ImGui::MenuItem("Align to Surface", nullptr, appSettings.alignToSurface)){
                appSettings.alignToSurface = !appSettings.alignToSurface;
                AppSettings::setSceneViewSettings(appSettings);
            }
            if (hasNavigation && ImGui::MenuItem("Path Tester", pathTesterShortcut.empty() ? nullptr : pathTesterShortcut.c_str(),
                                                 pathTester.isActive(sceneProject.id))){
                pathTester.toggle(sceneProject);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Measures", nullptr, false, !state.measures.empty())) clearMeasures(sceneProject);
            ImGui::EndPopup();
        }
    }

    // ---- Scene Camera
    if (is3D){
        ImGui::SameLine();
        const std::string popup = "SceneCameraPopup" + sid;
        if (ImGui::Button(cameraLabel.c_str()) || cameraPopupScene == sceneProject.id){
            cameraPopupScene = NULL_PROJECT_SCENE;
            ImGui::OpenPopup(popup.c_str());
        }
        ImGui::SetItemTooltip("Scene Camera: Field of View, clipping, speed, easing");
        if (ImGui::BeginPopup(popup.c_str())){
            drawCameraSettings(sceneProject);
            ImGui::EndPopup();
        }
    }
}

// ---- panes

bool SceneViewController::drawPaneHeader(SceneProject& sceneProject, int pane){
    SceneViewState& state = sceneProject.viewState;
    const SceneViewPane& paneState = state.panes[(size_t)pane];
    const std::string sid = std::to_string(sceneProject.id) + "_" + std::to_string(pane);
    const bool active = pane == state.activePane;
    const char* shading = paneState.shading == PaneShading::SHADED_SHADOWS ? "Shaded + Shadows" : "Shaded";
    const std::string label = std::string(axisLabel(paneState.axis)) + " " + ICON_FA_CARET_DOWN + "##PaneHeader" + sid;
    ImGui::PushStyleColor(ImGuiCol_Button, active ? Theme::Colors::ButtonActivated : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    const bool pressed = ImGui::Button(label.c_str());
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("%s pane (%s). Click: view and shading; double-click the header: maximize", axisLabel(paneState.axis),
                          active ? "Shaded" : shading);
    const std::string popup = "PaneMenu" + sid;
    if (pressed){
        ImGui::OpenPopup(popup.c_str());
    }
    bool used = pressed;
    if (ImGui::BeginPopup(popup.c_str())){
        used = true;
        for (SceneViewAxis axis : {SceneViewAxis::FREE, SceneViewAxis::TOP, SceneViewAxis::BOTTOM, SceneViewAxis::FRONT, SceneViewAxis::BACK,
                                   SceneViewAxis::LEFT, SceneViewAxis::RIGHT}){
            if (ImGui::MenuItem(axisLabel(axis), nullptr, paneState.axis == axis)){
                setPaneAxis(sceneProject, pane, axis);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Shaded", nullptr, paneState.shading == PaneShading::SHADED)){
            setPaneShading(sceneProject, pane, PaneShading::SHADED);
        }
        if (ImGui::MenuItem("Shaded + Shadows", nullptr, paneState.shading == PaneShading::SHADED_SHADOWS)){
            setPaneShading(sceneProject, pane, PaneShading::SHADED_SHADOWS);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Maximize", nullptr, state.maximizedPane == pane)){
            toggleMaximized(sceneProject, pane);
        }
        ImGui::EndPopup();
    }
    // a double click on the header strip (not its button) maximizes the pane
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()){
        toggleMaximized(sceneProject, pane);
        used = true;
    }
    return used;
}

void SceneViewController::drawInactivePane(SceneProject& sceneProject, int pane){
    Runtime& rt = runtime(sceneProject.id);
    Pane& p = rt.panes[(size_t)pane];
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGuiViewport* viewport = ImGui::GetWindowViewport();
    const ImVec2 scale = (viewport && viewport->FramebufferScale.x > 0.0f) ? viewport->FramebufferScale : ImGui::GetIO().DisplayFramebufferScale;
    const int width = std::max(0, (int)(size.x * std::max(scale.x, 1.0f)));
    const int height = std::max(0, (int)(size.y * std::max(scale.y, 1.0f)));
    if (width != p.width || height != p.height){
        p.width = width;
        p.height = height;
        p.dirty = true;
        sceneProject.needUpdateRender = true;
    }
    p.imageMin = ImGui::GetCursorScreenPos();
    p.imageSize = size;
    ImTextureID texture{};
    if (p.framebuffer.isCreated()){
        texture = Backend::getImGuiTexture(&p.framebuffer.getRender().getColorTexture());
    }
    Widgets::image(texture, size, IM_COL32(0, 0, 0, 255));
    // the pane the pointer moves onto (or clicks) takes the input; not while a drag of the active
    // one goes on, and not a pointer resting there (an agent's pick_at or layout chose another)
    const ImGuiIO& io = ImGui::GetIO();
    const bool anyDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    if (ImGui::IsWindowHovered() && (clicked || (moved && !anyDown))){
        requestActivePane(sceneProject, pane);
    }
}

bool SceneViewController::beginPane(SceneProject& sceneProject, int slot){
    const SceneViewState& state = sceneProject.viewState;
    if (state.layout == SceneViewLayout::SINGLE || sceneProject.sceneType != SceneType::SCENE_3D){
        return true;
    }
    const int pane = paneOfSlot(sceneProject, slot);
    // the slot's rectangle in the canvas
    ImGuiWindow* canvas = ImGui::GetCurrentWindow();
    // the canvas content in its own coordinates (no scrolling: the panes fill it exactly)
    const ImVec2 origin(canvas->ContentRegionRect.Min.x - canvas->Pos.x, canvas->ContentRegionRect.Min.y - canvas->Pos.y);
    const ImVec2 full = canvas->ContentRegionRect.GetSize();
    const float gap = Theme::dpi(2.0f);
    ImVec2 min(0.0f, 0.0f), size = full;
    if (state.maximizedPane < 0){
        if (state.layout == SceneViewLayout::SPLIT){
            size = ImVec2((full.x - gap) * 0.5f, full.y);
            min = ImVec2(slot == 0 ? 0.0f : size.x + gap, 0.0f);
        }else{
            size = ImVec2((full.x - gap) * 0.5f, (full.y - gap) * 0.5f);
            min = ImVec2((slot % 2) ? size.x + gap : 0.0f, (slot / 2) ? size.y + gap : 0.0f);
        }
    }
    size.x = std::max(size.x, 1.0f);
    size.y = std::max(size.y, 1.0f);
    ImGui::SetCursorPos(ImVec2(origin.x + min.x, origin.y + min.y));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::BeginChild(("Pane" + std::to_string(pane)).c_str(), size, ImGuiChildFlags_None, flags);
    ImGui::BeginChild("Header", ImVec2(0.0f, ImGui::GetFrameHeight()), ImGuiChildFlags_None, flags);
    drawPaneHeader(sceneProject, pane);
    ImGui::EndChild();
    ImGui::BeginChild("View", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, flags);
    if (pane == state.activePane){
        return true;
    }
    drawInactivePane(sceneProject, pane);
    return false;
}

void SceneViewController::endPane(SceneProject& sceneProject, int slot){
    (void)slot;
    const SceneViewState& state = sceneProject.viewState;
    if (state.layout == SceneViewLayout::SINGLE || sceneProject.sceneType != SceneType::SCENE_3D){
        return;
    }
    ImGui::EndChild();   // View
    ImGui::EndChild();   // Pane
}

// ---- overlays

void SceneViewController::drawOverlays(SceneProject& sceneProject, ImVec2 viewMin, ImVec2 viewSize, float engineScaleX, float engineScaleY){
    if (!sceneProject.sceneRender || viewSize.x <= 0.0f || viewSize.y <= 0.0f){
        return;
    }
    Runtime& rt = runtime(sceneProject.id);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    Camera* camera = sceneProject.sceneRender->getCamera();
    const Matrix4 viewProjection = camera->getViewProjectionMatrix();
    const bool is3D = sceneProject.sceneType == SceneType::SCENE_3D;
    const float sx = engineScaleX > 0.0f ? engineScaleX : 1.0f;
    const float sy = engineScaleY > 0.0f ? engineScaleY : 1.0f;

    // ---- measures (committed, then the one being dragged)
    auto drawMeasure = [&](const Vector3& from, const Vector3& to, bool selected){
        ImVec2 a, b;
        if (!projectToView(viewProjection, from, viewMin, viewSize, a) || !projectToView(viewProjection, to, viewMin, viewSize, b)){
            return;
        }
        const ImU32 color = measureColor(selected);
        drawList->AddLine(a, b, color, Theme::dpi(2.0f));
        drawList->AddCircleFilled(a, Theme::dpi(3.5f), color);
        drawList->AddCircleFilled(b, Theme::dpi(3.5f), color);
        const Vector3 delta = to - from;
        std::string text = sceneview::formatLength(delta.length(), is3D);
        if (io.KeyShift){
            char deltas[160];
            std::snprintf(deltas, sizeof(deltas), "  \xCE\x94X %.3f  \xCE\x94Y %.3f%s", delta.x, delta.y, is3D ? "" : "");
            text += deltas;
            if (is3D){
                char dz[48];
                std::snprintf(dz, sizeof(dz), "  \xCE\x94Z %.3f", delta.z);
                text += dz;
            }
        }
        drawPill(drawList, ImVec2((a.x + b.x) * 0.5f + Theme::dpi(6.0f), (a.y + b.y) * 0.5f - Theme::dpi(10.0f)), text);
    };
    for (const SceneMeasure& measure : sceneProject.viewState.measures){
        drawMeasure(measure.from, measure.to, measure.id == rt.selectedMeasure);
    }
    if (rt.measuring && rt.measureDragging && rt.measureValid){
        drawMeasure(rt.measureFrom, rt.measureTo, true);
    }

    // ---- rulers
    const bool orthoAxis = is3D && isOrthographic(sceneProject) && viewAxis(sceneProject) != SceneViewAxis::FREE;
    if (AppSettings::getSceneViewSettings().rulers && rulersApply(sceneProject, orthoAxis)){
        const float thickness = Theme::dpi(18.0f);
        const ImU32 background = IM_COL32(30, 30, 34, 210);
        const ImU32 tick = IM_COL32(200, 200, 200, 255);
        const ImU32 cursor = IM_COL32(255, 200, 40, 255);
        // the world under a view pixel (engine pixels): the ray's origin
        auto worldAt = [&](float x, float y){ return sceneProject.sceneRender->screenToRay(x * sx, y * sy).getOrigin(); };
        auto dominant = [](const Vector3& v){
            const float ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
            return (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
        };
        // horizontal ruler along the top, vertical along the left
        for (int horizontal = 1; horizontal >= 0; horizontal--){
            const float length = horizontal ? viewSize.x : viewSize.y;
            const Vector3 a = horizontal ? worldAt(0.0f, viewSize.y * 0.5f) : worldAt(viewSize.x * 0.5f, 0.0f);
            const Vector3 b = horizontal ? worldAt(viewSize.x, viewSize.y * 0.5f) : worldAt(viewSize.x * 0.5f, viewSize.y);
            const int axis = dominant(b - a);
            const double va = a[(unsigned)axis];
            const double vb = b[(unsigned)axis];
            if (!(std::fabs(vb - va) > 0.0) || length <= 0.0f){
                continue;
            }
            const double unitsPerPixel = std::fabs(vb - va) / length;
            const double step = sceneview::rulerMajorStep(unitsPerPixel / Theme::dpiScale());
            const double minor = step / (sceneview::RULER_MINOR_TICKS + 1);
            const ImVec2 rmin = horizontal ? ImVec2(viewMin.x + thickness, viewMin.y) : ImVec2(viewMin.x, viewMin.y + thickness);
            const ImVec2 rmax = horizontal ? ImVec2(viewMin.x + viewSize.x, viewMin.y + thickness) : ImVec2(viewMin.x + thickness, viewMin.y + viewSize.y);
            drawList->AddRectFilled(rmin, rmax, background);
            const double lo = std::min(va, vb), hi = std::max(va, vb);
            for (double v = std::floor(lo / minor) * minor; v <= hi; v += minor){
                const float t = (float)((v - va) / (vb - va));
                const float px = t * length;
                if (px < thickness){
                    continue;
                }
                const bool major = std::fabs(v / step - std::round(v / step)) < 1e-6;
                const float tickLength = major ? thickness * 0.6f : thickness * 0.25f;
                if (horizontal){
                    drawList->AddLine(ImVec2(viewMin.x + px, viewMin.y + thickness - tickLength), ImVec2(viewMin.x + px, viewMin.y + thickness), tick);
                    if (major){
                        drawList->AddText(ImVec2(viewMin.x + px + Theme::dpi(2.0f), viewMin.y), tick, sceneview::rulerLabel(v, step).c_str());
                    }
                }else{
                    drawList->AddLine(ImVec2(viewMin.x + thickness - tickLength, viewMin.y + px), ImVec2(viewMin.x + thickness, viewMin.y + px), tick);
                    if (major){
                        drawList->AddText(ImVec2(viewMin.x + Theme::dpi(1.0f), viewMin.y + px + Theme::dpi(1.0f)), tick,
                                          sceneview::rulerLabel(v, step).c_str());
                    }
                }
            }
            // the cursor tick
            const ImVec2 mouse = io.MousePos;
            if (mouse.x >= viewMin.x && mouse.x <= viewMin.x + viewSize.x && mouse.y >= viewMin.y && mouse.y <= viewMin.y + viewSize.y){
                if (horizontal){
                    drawList->AddLine(ImVec2(mouse.x, viewMin.y), ImVec2(mouse.x, viewMin.y + thickness), cursor, Theme::dpi(1.5f));
                }else{
                    drawList->AddLine(ImVec2(viewMin.x, mouse.y), ImVec2(viewMin.x + thickness, mouse.y), cursor, Theme::dpi(1.5f));
                }
            }
        }
    }

    // ---- the "name n/m" pill near the pointer of the click
    if (!rt.pillText.empty() && ImGui::GetTime() <= rt.pillUntil){
        drawPill(drawList, ImVec2(viewMin.x + rt.pillPosition.x / sx + Theme::dpi(14.0f), viewMin.y + rt.pillPosition.y / sy + Theme::dpi(14.0f)), rt.pillText);
    }

    // ---- "N hidden": a click shows them all
    const size_t hidden = sceneview::hiddenCount(sceneProject);
    const bool layersHidden = sceneProject.viewState.layerFilter.visible != LAYER_MASK_EVERYTHING;
    if (hidden > 0 || layersHidden){
        std::string text = hidden > 0 ? std::to_string(hidden) + " hidden" : std::string("Layers hidden");
        if (hidden > 0 && layersHidden) text += ", layers hidden";
        text += "  (click: show all)";
        const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
        const ImVec2 pad = Theme::dpi(ImVec2(8.0f, 3.0f));
        const ImVec2 at(viewMin.x + (viewSize.x - textSize.x) * 0.5f - pad.x, viewMin.y + Theme::dpi(6.0f));
        const ImVec2 end(at.x + textSize.x + pad.x * 2.0f, at.y + textSize.y + pad.y * 2.0f);
        const bool hovered = ImGui::IsMouseHoveringRect(at, end) && ImGui::IsWindowHovered();
        drawList->AddRectFilled(at, end, hovered ? IM_COL32(120, 90, 20, 235) : IM_COL32(90, 70, 20, 220), Theme::dpi(6.0f));
        drawList->AddText(ImVec2(at.x + pad.x, at.y + pad.y), IM_COL32(255, 230, 160, 255), text.c_str());
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            showAll(sceneProject);
        }
    }

    // ---- a click waiting for a BVH: the busy cursor
    if (rt.pending.active && ImGui::IsWindowHovered()){
        ImGui::SetMouseCursor(ImGuiMouseCursor_Progress);
    }
}

// ---- input

bool SceneViewController::handleInput(SceneProject& sceneProject, float x, float y, bool hovered, bool focused){
    Runtime& rt = runtime(sceneProject.id);
    ImGuiIO& io = ImGui::GetIO();
    bool used = false;
    // its keys are command catalog entries (shortcuts/DefaultCommands.cpp, Scene View: Alt+F, H,
    // Alt+H, L, M, Delete, Shift/Ctrl+Shift+1..9, run by AppCommands.cpp); Esc stays here, it
    // ends what runs in the view
    if (focused && rt.measuring && !io.WantTextInput && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)){
        setMeasuring(sceneProject, false);
    }

    // Ctrl/Cmd+wheel: step through what overlaps under the pointer (the zoom ignores it)
    if (hovered && (io.KeyCtrl || io.KeySuper) && io.MouseWheel != 0.0f && !ImGui::IsMouseDown(ImGuiMouseButton_Right)){
        cycleCandidates(sceneProject.id, x, y, io.MouseWheel < 0.0f ? 1 : -1);
        io.MouseWheel = 0.0f;
        used = true;
    }

    // the measure tool takes the left button
    if (rt.measuring && hovered && !io.KeyAlt){
        // held V: the catalog's gesture row sceneView.measureVertexSnap
        const MeasureSnap snap = ImGui::IsKeyDown(ImGuiKey_V) ? MeasureSnap::VERTEX
                               : (sceneProject.displaySettings.snapToGrid ? MeasureSnap::GRID : MeasureSnap::SURFACE);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            // a click on a committed measure selects it
            rt.selectedMeasure = 0;
            const Matrix4 viewProjection = sceneProject.sceneRender->getCamera()->getViewProjectionMatrix();
            const float w = (float)std::max(sceneProject.sceneRender->getViewWidth(), 1);
            const float h = (float)std::max(sceneProject.sceneRender->getViewHeight(), 1);
            const float pick = Theme::dpi(6.0f) * sceneProject.sceneRender->getOverlayScale() / std::max(Theme::dpiScale(), 0.01f);
            for (const SceneMeasure& measure : sceneProject.viewState.measures){
                ImVec2 a, b;
                if (!projectToView(viewProjection, measure.from, ImVec2(0, 0), ImVec2(w, h), a) || !projectToView(viewProjection, measure.to, ImVec2(0, 0), ImVec2(w, h), b)){
                    continue;
                }
                const ImVec2 ab(b.x - a.x, b.y - a.y);
                const float len2 = ab.x * ab.x + ab.y * ab.y;
                const float t = len2 > 0.0f ? std::clamp(((x - a.x) * ab.x + (y - a.y) * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
                const float dx = a.x + ab.x * t - x, dy = a.y + ab.y * t - y;
                if (std::sqrt(dx * dx + dy * dy) <= pick){
                    rt.selectedMeasure = measure.id;
                }
            }
            rt.measureDragging = rt.selectedMeasure == 0 && measurePoint(sceneProject, x, y, snap, rt.measureFrom);
            rt.measureTo = rt.measureFrom;
            rt.measureValid = rt.measureDragging;
            sceneProject.needUpdateRender = true;
            used = true;
        }else if (rt.measureDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            Vector3 point;
            if (measurePoint(sceneProject, x, y, snap, point)){
                rt.measureTo = point;
            }
            used = true;
        }
        if (rt.measureDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
            if ((rt.measureTo - rt.measureFrom).length() > 1e-6f){
                rt.selectedMeasure = addMeasure(sceneProject, rt.measureFrom, rt.measureTo);
            }
            rt.measureDragging = false;
            rt.measureValid = false;
            used = true;
        }
    }
    return used;
}
