// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeSceneTools.h"
#include "component/Transform.h"

#include "LatticeEditorOps.h"
#include "App.h"
#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "lattice/LatticePresets.h"
#include "shortcuts/CommandCatalog.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    // the catalog's text for a menu item's shortcut (S25: the keys are rebindable)
    std::string shortcutText(const char* id){
        return CommandCatalog::editor().menuShortcutText(id);
    }

    // what the Presets submenu applies, kept between menus
    lattice::PresetParams presetParams;
    // Fit to Selection's padding (a fraction: 0.02 is 2%)
    float fitPadding = 0.02f;

    const char* const axisNames[] = {"X", "Y", "Z"};

    void report(bool ok, const std::string& what, const std::string& error){
        if (!ok && !error.empty()){
            Out::error("%s: %s", what.c_str(), error.c_str());
        }
    }

    void redraw(Project* project, uint32_t sceneId){
        if (SceneProject* sceneProject = project->getScene(sceneId)){
            sceneProject->needUpdateRender = true;
        }
    }

    LatticeComponent* latticeComponent(Project* project, uint32_t sceneId, Entity lattice){
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject || !sceneProject->scene || lattice == NULL_ENTITY){
            return nullptr;
        }
        return sceneProject->scene->findComponent<LatticeComponent>(lattice);
    }

    void select(Project* project, uint32_t sceneId, Entity lattice, const std::vector<int>& points){
        LatticeEditorOps::setSelectedPoints(sceneId, lattice, points);
        redraw(project, sceneId);
    }

    std::string displayName(const std::string& name){
        std::string text = name;
        if (!text.empty()){
            text[0] = (char)std::toupper((unsigned char)text[0]);
        }
        return text;
    }

    void drawPresetMenu(Project* project, uint32_t sceneId, Entity lattice, const std::vector<int>& selected){
        const std::vector<std::string> names = lattice::presetNames();
        const char* current = lattice::presetName(presetParams.preset);
        ImGui::SetNextItemWidth(Theme::dpi(130.0f));
        if (ImGui::BeginCombo("Preset##LatticePreset", displayName(current).c_str())){
            for (const std::string& name : names){
                lattice::LatticePreset preset;
                if (lattice::presetFromName(name, preset) && ImGui::Selectable(displayName(name).c_str(), preset == presetParams.preset)){
                    presetParams.preset = preset;
                }
            }
            ImGui::EndCombo();
        }

        int axis = (int)presetParams.axis;
        ImGui::SetNextItemWidth(Theme::dpi(130.0f));
        if (ImGui::Combo("Axis##LatticePresetAxis", &axis, axisNames, 3)){
            presetParams.axis = (lattice::LatticeAxis)axis;
            if (presetParams.toward == presetParams.axis){
                presetParams.toward = (lattice::LatticeAxis)((axis + 1) % 3);
            }
        }

        const bool usesToward = presetParams.preset == lattice::LatticePreset::BEND || presetParams.preset == lattice::LatticePreset::SHEAR;
        if (usesToward){
            int toward = (int)presetParams.toward;
            ImGui::SetNextItemWidth(Theme::dpi(130.0f));
            if (ImGui::Combo("Toward##LatticePresetToward", &toward, axisNames, 3) && toward != (int)presetParams.axis){
                presetParams.toward = (lattice::LatticeAxis)toward;
            }
        }

        const bool degrees = presetParams.preset == lattice::LatticePreset::TWIST || presetParams.preset == lattice::LatticePreset::BEND;
        float minAmount = degrees ? -360.0f : (presetParams.preset == lattice::LatticePreset::SQUASH ? 0.0f : -1.0f);
        float maxAmount = degrees ? 360.0f : (presetParams.preset == lattice::LatticePreset::SQUASH ? 0.95f : 1.0f);
        if (presetParams.preset == lattice::LatticePreset::STRETCH || presetParams.preset == lattice::LatticePreset::NOISE){
            minAmount = 0.0f;
            maxAmount = presetParams.preset == lattice::LatticePreset::NOISE ? 0.5f : 2.0f;
        }
        presetParams.amount = std::clamp(presetParams.amount, minAmount, maxAmount);
        ImGui::SetNextItemWidth(Theme::dpi(130.0f));
        ImGui::DragFloat("Amount##LatticePresetAmount", &presetParams.amount, degrees ? 0.5f : 0.005f, minAmount, maxAmount,
                         degrees ? "%.1f deg" : "%.3f", ImGuiSliderFlags_AlwaysClamp);

        if (presetParams.preset == lattice::LatticePreset::NOISE){
            int seed = (int)presetParams.seed;
            ImGui::SetNextItemWidth(Theme::dpi(130.0f));
            if (ImGui::InputInt("Seed##LatticePresetSeed", &seed)){
                presetParams.seed = (unsigned int)std::max(seed, 0);
            }
        }
        ImGui::Checkbox("Replace offsets##LatticePresetReplace", &presetParams.replace);
        ImGui::SetItemTooltip("Off: adds to the current offsets. On: replaces them.");

        const std::string apply = selected.empty() ? "Apply to All Points" : "Apply to " + std::to_string(selected.size()) + " Selected";
        if (ImGui::Button(apply.c_str())){
            std::string error;
            report(LatticeEditorOps::applyPreset(project, sceneId, lattice, presetParams, selected, &error), "Lattice preset", error);
            ImGui::CloseCurrentPopup();
        }
    }

}

Entity LatticeSceneTools::getMenuLattice(Project* project, uint32_t sceneId){
    Entity edited = LatticeEditorOps::getEditedLattice(project, sceneId);
    if (edited != NULL_ENTITY){
        return edited;
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        return NULL_ENTITY;
    }
    Entity found = NULL_ENTITY;
    for (Entity entity : sceneProject->selectedEntities){
        if (sceneProject->scene->findComponent<LatticeComponent>(entity) && sceneProject->scene->findComponent<Transform>(entity)){
            if (found != NULL_ENTITY){
                return NULL_ENTITY; // several lattices: no one menu
            }
            found = entity;
        }
    }
    return found;
}

bool LatticeSceneTools::selectAllPoints(Project* project, uint32_t sceneId){
    Entity lattice = LatticeEditorOps::getEditedLattice(project, sceneId);
    LatticeComponent* component = latticeComponent(project, sceneId, lattice);
    if (!component){
        return false;
    }
    select(project, sceneId, lattice, lattice::selectAll(component->resolutionX, component->resolutionY, component->resolutionZ));
    return true;
}

bool LatticeSceneTools::handleShortcuts(Project* project, uint32_t sceneId){
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput){
        return false;
    }
    Entity lattice = LatticeEditorOps::getEditedLattice(project, sceneId);
    if (!latticeComponent(project, sceneId, lattice)){
        return false;
    }
    const std::vector<int>& selected = LatticeEditorOps::getSelectedPoints(sceneId, lattice);
    const bool plain = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper;
    if (plain && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !selected.empty()){
        select(project, sceneId, lattice, {});
        return true;
    }
    return false;
}

bool LatticeSceneTools::isEditingPoints(Project* project, uint32_t sceneId){
    return latticeComponent(project, sceneId, LatticeEditorOps::getEditedLattice(project, sceneId)) != nullptr;
}

bool LatticeSceneTools::runPointAction(Project* project, uint32_t sceneId, PointAction action){
    Entity lattice = LatticeEditorOps::getEditedLattice(project, sceneId);
    LatticeComponent* component = latticeComponent(project, sceneId, lattice);
    if (!component){
        return false;
    }
    const int rx = component->resolutionX;
    const int ry = component->resolutionY;
    const int rz = component->resolutionZ;
    const std::vector<int> selected = LatticeEditorOps::getSelectedPoints(sceneId, lattice);
    switch (action){
        case PointAction::SelectNone:
            select(project, sceneId, lattice, {});
            // the Alt of Alt+A must not also toggle the menu layer on release: ImGui only
            // cancels that toggle for a key pressed in a later frame than Alt, so a fast chord (Alt
            // and A in one frame) moved the keyboard focus to the menu bar and the next Scene-view
            // shortcut (Ctrl+A) went nowhere
            if (ImGui::GetCurrentContext()) ImGui::GetCurrentContext()->NavWindowingToggleLayer = false;
            break;
        case PointAction::Invert:
            select(project, sceneId, lattice, lattice::invertSelection(rx, ry, rz, selected));
            break;
        case PointAction::Expand:
            select(project, sceneId, lattice, lattice::expandSelection(rx, ry, rz, selected));
            break;
        case PointAction::Shrink:
            select(project, sceneId, lattice, lattice::shrinkSelection(rx, ry, rz, selected));
            break;
        case PointAction::ToggleFalloff:
            LatticeEditorOps::falloff().enabled = !LatticeEditorOps::falloff().enabled;
            redraw(project, sceneId);
            break;
    }
    return true;
}

void LatticeSceneTools::drawContextMenu(Project* project, uint32_t sceneId, Entity lattice){
    SceneProject* sceneProject = project->getScene(sceneId);
    LatticeComponent* component = latticeComponent(project, sceneId, lattice);
    if (!sceneProject || !component){
        ImGui::TextDisabled("No lattice selected");
        return;
    }
    Scene* scene = sceneProject->scene;
    const int rx = component->resolutionX;
    const int ry = component->resolutionY;
    const int rz = component->resolutionZ;

    // points are edited on a lattice selected alone
    const bool editable = LatticeEditorOps::getEditedLattice(project, sceneId) == lattice;
    const std::vector<int> selected = editable ? LatticeEditorOps::getSelectedPoints(sceneId, lattice) : std::vector<int>();
    std::string error;

    ImGui::TextDisabled("%s %s", ICON_FA_BORDER_NONE, scene->getEntityName(lattice).c_str());
    ImGui::Separator();

    ImGui::BeginDisabled(!editable);
    if (ImGui::MenuItem("Select All", shortcutText("edit.selectAll").c_str())){
        select(project, sceneId, lattice, lattice::selectAll(rx, ry, rz));
    }
    if (ImGui::MenuItem("Select None", shortcutText("lattice.selectNone").c_str(), false, !selected.empty())){
        select(project, sceneId, lattice, {});
    }
    if (ImGui::MenuItem("Invert", shortcutText("lattice.invert").c_str())){
        select(project, sceneId, lattice, lattice::invertSelection(rx, ry, rz, selected));
    }
    if (ImGui::MenuItem("Select Exterior")){
        select(project, sceneId, lattice, lattice::selectExterior(rx, ry, rz));
    }
    if (ImGui::BeginMenu("Select Face")){
        for (int axis = 0; axis < 3; axis++){
            for (int sign = 0; sign < 2; sign++){
                const bool positive = sign == 0;
                const std::string label = std::string(positive ? "+" : "-") + axisNames[axis];
                if (ImGui::MenuItem(label.c_str())){
                    select(project, sceneId, lattice, lattice::selectFace(rx, ry, rz, (lattice::LatticeAxis)axis, positive));
                }
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Expand", shortcutText("lattice.expand").c_str(), false, !selected.empty())){
        select(project, sceneId, lattice, lattice::expandSelection(rx, ry, rz, selected));
    }
    if (ImGui::MenuItem("Shrink", shortcutText("lattice.shrink").c_str(), false, !selected.empty())){
        select(project, sceneId, lattice, lattice::shrinkSelection(rx, ry, rz, selected));
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    if (ImGui::BeginMenu("Mirror")){
        for (int axis = 0; axis < 3; axis++){
            for (int sign = 0; sign < 2; sign++){
                const bool fromPositive = sign == 0;
                const std::string label = std::string(axisNames[axis]) + (fromPositive ? ": + to -" : ": - to +");
                if (ImGui::MenuItem(label.c_str())){
                    report(LatticeEditorOps::mirror(project, sceneId, lattice, (lattice::LatticeAxis)axis, fromPositive, &error), "Lattice mirror", error);
                }
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset Selected", nullptr, false, !selected.empty())){
        report(LatticeEditorOps::resetPoints(project, sceneId, lattice, selected, &error), "Lattice reset", error);
    }
    if (ImGui::MenuItem("Reset All")){
        report(LatticeEditorOps::resetPoints(project, sceneId, lattice, {}, &error), "Lattice reset", error);
    }
    if (ImGui::BeginMenu("Presets...")){
        drawPresetMenu(project, sceneId, lattice, selected);
        ImGui::EndMenu();
    }

    ImGui::Separator();

    const std::vector<Entity> targets = LatticeEditorOps::findTargets(scene, lattice);
    if (ImGui::MenuItem("Fit to Targets", nullptr, false, !targets.empty())){
        report(LatticeEditorOps::fitTo(project, sceneId, lattice, targets, 0.02f, &error), "Lattice fit", error);
    }
    if (targets.empty()){
        ImGui::SetItemTooltip("No entity is bound to this lattice yet");
    }

    std::vector<Entity> others;
    for (Entity entity : sceneProject->selectedEntities){
        if (entity != lattice){
            others.push_back(entity);
        }
    }
    if (ImGui::BeginMenu("Fit to Selection...", !others.empty())){
        ImGui::TextDisabled("Fits the cage around the %zu other selected entit%s", others.size(), others.size() == 1 ? "y" : "ies");
        float percent = fitPadding * 100.0f;
        ImGui::SetNextItemWidth(Theme::dpi(130.0f));
        if (ImGui::DragFloat("Padding##LatticeFitPadding", &percent, 0.1f, 0.0f, 100.0f, "%.1f %%", ImGuiSliderFlags_AlwaysClamp)){
            fitPadding = percent / 100.0f;
        }
        if (ImGui::Button("Fit")){
            report(LatticeEditorOps::fitTo(project, sceneId, lattice, others, fitPadding, &error), "Lattice fit", error);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    if (others.empty()){
        ImGui::SetItemTooltip("Select the lattice together with the entities to fit it to");
    }

    if (ImGui::MenuItem("Copy as Lua")){
        // the selected points, or all of them
        std::string text = lattice::copyAsLua("lattice", rx, ry, rz, component->offsets, selected);
        if (text.empty()){
            Out::info("Copy as Lua: no %s point has an offset", selected.empty() ? "" : "selected");
        }else{
            ImGui::SetClipboardText(text.c_str());
            Out::info("Copied the lattice offsets as Lua (lattice:setOffset calls)");
        }
    }

    ImGui::Separator();

    LatticeFalloff& falloff = LatticeEditorOps::falloff();
    if (ImGui::MenuItem("Proportional Falloff", "O", falloff.enabled)){
        falloff.enabled = !falloff.enabled;
        redraw(project, sceneId);
    }
    if (ImGui::BeginMenu("Falloff")){
        static const char* curves[] = {"Smooth", "Linear", "Sharp", "Constant"};
        for (int curve = 0; curve < 4; curve++){
            if (ImGui::MenuItem(curves[curve], nullptr, (int)falloff.curve == curve)){
                falloff.curve = (LatticeFalloffCurve)curve;
            }
        }
        ImGui::SetNextItemWidth(Theme::dpi(130.0f));
        if (ImGui::DragFloat("Radius##LatticeFalloffRadius", &falloff.radius, 0.01f, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)){
            redraw(project, sceneId);
        }
        ImGui::SetItemTooltip("In lattice local units (the cage is 1 across). The mouse wheel changes it during a drag.");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Lattice Preferences...")){
        Backend::getApp().openPreferences("Lattice");
    }
}
