// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsPlacementWindow.h"

#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "util/Angle.h"
#include "util/UIUtils.h"
#include "window/MainToolbar.h"
#include "window/TerrainEditWindow.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>

using namespace jasperengine;

namespace {

    // A settle run to its end at once (Physics turned off, Hand or Rect picked) steps at the
    // world's own rate
    constexpr double SETTLE_STEP_SECONDS = 1.0 / 120.0;

    const ImU32 HUD_BACKGROUND = IM_COL32(20, 20, 20, 210);
    const ImU32 HUD_TEXT = IM_COL32(230, 230, 230, 255);
    const ImU32 HUD_DIM_TEXT = IM_COL32(170, 170, 170, 255);

    const char* gestureName(editor::PlacementGesture gesture){
        switch (gesture){
            case editor::PlacementGesture::Move: return "Move";
            case editor::PlacementGesture::Rotate: return "Rotate";
            case editor::PlacementGesture::GatherSpread: return "Gather/Spread";
            case editor::PlacementGesture::Drop: return "Drop";
            case editor::PlacementGesture::Snap: return "Snap";
            case editor::PlacementGesture::Distribute: return "Distribute";
            case editor::PlacementGesture::Gather: return "Gather";
            case editor::PlacementGesture::None: break;
        }
        return "";
    }

    // What each transform tool does with Physics on
    const char* toolGestureName(editor::SceneRender& sceneRender){
        if (sceneRender.getCursorSelected() == editor::CursorSelected::HAND){
            return "Hand";
        }
        switch (sceneRender.getToolsLayer()->getGizmoSelected()){
            case editor::GizmoSelected::TRANSLATE: return "Move";
            case editor::GizmoSelected::ROTATE: return "Rotate";
            case editor::GizmoSelected::SCALE: return "Gather/Spread";
            case editor::GizmoSelected::OBJECT2D: break;
        }
        return "Rect";
    }

    std::string formatBytes(size_t bytes){
        char text[32];
        if (bytes >= 1024 * 1024){
            std::snprintf(text, sizeof(text), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
        }else if (bytes >= 1024){
            std::snprintf(text, sizeof(text), "%.1f KB", (double)bytes / 1024.0);
        }else{
            std::snprintf(text, sizeof(text), "%zu B", bytes);
        }
        return text;
    }

    // Mesh-local positions and triangle indices of every submesh: the geometry a decomposition
    // is requested and cached for. Submeshes sharing one POSITION buffer add its vertices once;
    // a submesh without indices is a plain triangle list.
    void meshGeometry(MeshComponent& mesh, std::vector<Vector3>& positions, std::vector<uint32_t>& indices){
        positions.clear();
        indices.clear();

        std::map<std::string, Buffer*> buffers;
        if (mesh.buffer.getSize() > 0) buffers["vertices"] = &mesh.buffer;
        if (mesh.indices.getSize() > 0) buffers["indices"] = &mesh.indices;
        for (int i = 0; i < mesh.numExternalBuffers; i++) buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];

        Buffer* vertexBuffer = nullptr;
        Attribute vertexAttr;
        Buffer* indexBuffer = nullptr;
        Attribute indexAttr;
        for (auto const& buf : buffers){
            if (buf.second->getAttribute(AttributeType::POSITION)){
                vertexBuffer = buf.second;
                vertexAttr = *buf.second->getAttribute(AttributeType::POSITION);
            }
            if (buf.second->getAttribute(AttributeType::INDEX)){
                indexBuffer = buf.second;
                indexAttr = *buf.second->getAttribute(AttributeType::INDEX);
            }
        }
        // an empty name keeps the buffer in use; an unknown one drops the attribute
        auto resolve = [&buffers](const std::string& name, Buffer* current) -> Buffer* {
            if (name.empty()) return current;
            auto named = buffers.find(name);
            return named != buffers.end() ? named->second : nullptr;
        };

        std::map<std::tuple<const void*, size_t, size_t>, uint32_t> vertexBases;
        for (size_t s = 0; s < mesh.numSubmeshes; s++){
            for (auto const& attr : mesh.submeshes[s].attributes){
                if (attr.first == AttributeType::POSITION){
                    if (Buffer* resolved = resolve(attr.second.getBufferName(), vertexBuffer)){
                        vertexBuffer = resolved;
                        vertexAttr = attr.second;
                    }
                }
                if (attr.first == AttributeType::INDEX){
                    if (Buffer* resolved = resolve(attr.second.getBufferName(), indexBuffer)){
                        indexBuffer = resolved;
                        indexAttr = attr.second;
                    }
                }
            }
            if (!vertexBuffer || !vertexBuffer->getData()){
                continue;
            }

            const auto range = std::make_tuple((const void*)vertexBuffer->getData(), (size_t)vertexAttr.getOffset(), (size_t)vertexAttr.getCount());
            const uint32_t vertexCount = (uint32_t)vertexAttr.getCount();
            auto base = vertexBases.find(range);
            if (base == vertexBases.end()){
                base = vertexBases.emplace(range, (uint32_t)positions.size()).first;
                for (uint32_t i = 0; i < vertexCount; i++){
                    positions.push_back(vertexBuffer->getVector3(&vertexAttr, i));
                }
            }
            // indices are local to their submesh, rebased onto its vertices
            const uint32_t vertexBase = base->second;

            if (!indexBuffer || !indexBuffer->getData()){
                for (uint32_t i = 0; i + 2 < vertexCount; i += 3){
                    indices.push_back(vertexBase + i);
                    indices.push_back(vertexBase + i + 1);
                    indices.push_back(vertexBase + i + 2);
                }
                continue;
            }
            const size_t indexCount = indexAttr.getCount() / 3 * 3;
            for (size_t i = 0; i < indexCount; i++){
                uint32_t index = 0;
                if (indexAttr.getDataType() == AttributeDataType::UNSIGNED_INT){
                    index = indexBuffer->getUInt32(&indexAttr, i);
                }else if (indexAttr.getDataType() == AttributeDataType::UNSIGNED_SHORT){
                    index = indexBuffer->getUInt16(&indexAttr, i);
                }
                indices.push_back(vertexBase + index);
            }
        }
    }

    bool beginProperties(const char* id){
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)){
            return false;
        }
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, std::min(ImGui::GetFontSize() * 8.5f, ImGui::GetContentRegionAvail().x * 0.42f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }

    // A labelled row of a settings table; the value cell is next, filling its width
    void propertyRow(const char* label, const char* tooltip, bool dim = false){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        if (dim){
            ImGui::TextDisabled("%s", label);
        }else{
            ImGui::TextUnformatted(label);
        }
        if (tooltip){
            ImGui::SetItemTooltip("%s", tooltip);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
    }

    // Equal buttons across the cell, the chosen one lit; true when it changed
    bool segmentedButtons(const char* id, int& value, const char* const* labels, const char* const* tooltips, int count){
        bool changed = false;
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - spacing * (float)(count - 1)) / (float)count);
        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, ImGui::GetStyle().ItemSpacing.y));
        for (int i = 0; i < count; i++){
            if (i > 0) ImGui::SameLine();
            const bool selected = value == i;
            if (selected){
                ImGui::PushStyleColor(ImGuiCol_Button, editor::Theme::Colors::ButtonActivated);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, editor::Theme::Colors::ButtonActivated);
            }
            if (ImGui::Button(labels[i], ImVec2(width, 0.0f)) && !selected){
                value = i;
                changed = true;
            }
            if (selected){
                ImGui::PopStyleColor(2);
            }
            if (tooltips && tooltips[i]){
                ImGui::SetItemTooltip("%s", tooltips[i]);
            }
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        return changed;
    }

    // A small ↺ before a value that overrides its Feel preset: clicked, the preset is back
    bool resetToPresetButton(const char* id){
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(editor::Theme::dpi(2.0f), ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        const bool clicked = ImGui::Button((std::string(ICON_FA_ROTATE_LEFT) + "##" + id).c_str());
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::SetItemTooltip("Back to the Feel preset");
        return clicked;
    }

}

editor::PhysicsPlacementWindow::PhysicsPlacementWindow(Project* project): project(project){
}

editor::PhysicsPlacementWindow::~PhysicsPlacementWindow() = default;

//=== Window ===

void editor::PhysicsPlacementWindow::open(){
    setOpen(true);
    focusRequested = true;
}

void editor::PhysicsPlacementWindow::setOpen(bool open){
    if (open && !windowOpen){
        focusRequested = true;
    }
    windowOpen = open;
    if (!open){
        focusRequested = false;
    }
}

bool editor::PhysicsPlacementWindow::isOpen() const{
    return windowOpen;
}

//=== Settings ===

editor::PhysicsPlacementSettings& editor::PhysicsPlacementWindow::settings(){
    return project->getPhysicsPlacementSettings();
}

const editor::PhysicsPlacementSettings& editor::PhysicsPlacementWindow::getSettings() const{
    return project->getPhysicsPlacementSettings();
}

void editor::PhysicsPlacementWindow::setSettings(const PhysicsPlacementSettings& newSettings){
    settings() = newSettings;
    applySettings();
}

void editor::PhysicsPlacementWindow::applySettings(){
    for (auto& entry : sessions){
        entry.second.session->setSettings(settings());
        if (SceneProject* sceneProject = sceneFor(entry.first)){
            sceneProject->needUpdateRender = true;
        }
    }
    // written once no field is being dragged or typed in (show())
    settingsDirty = true;
}

//=== Sessions ===

editor::SceneProject* editor::PhysicsPlacementWindow::sceneFor(uint32_t sceneId) const{
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->sceneRender){
        return nullptr;
    }
    return sceneProject;
}

editor::PhysicsPlacementWindow::SceneSession* editor::PhysicsPlacementWindow::findSession(uint32_t sceneId){
    auto it = sessions.find(sceneId);
    return it != sessions.end() ? &it->second : nullptr;
}

const editor::PhysicsPlacementWindow::SceneSession* editor::PhysicsPlacementWindow::findSession(uint32_t sceneId) const{
    auto it = sessions.find(sceneId);
    return it != sessions.end() ? &it->second : nullptr;
}

editor::PlacementSession* editor::PhysicsPlacementWindow::sessionFor(uint32_t sceneId){
    if (SceneSession* existing = findSession(sceneId)){
        return existing->session.get();
    }
    SceneProject* sceneProject = sceneFor(sceneId);
    if (!sceneProject || !isShownInScene(sceneId)){
        return nullptr;
    }
    SceneSession sceneSession;
    sceneSession.session = std::make_unique<PlacementSession>(project, sceneId);
    sceneSession.session->setSettings(settings());
    sceneSession.structureVersion = sceneProject->structureVersion;
    sceneSession.selection = project->getSelectedEntities(sceneId);
    sceneSession.session->setSelection(sceneSession.selection);
    return sessions.emplace(sceneId, std::move(sceneSession)).first->second.session.get();
}

void editor::PhysicsPlacementWindow::finish(SceneSession& sceneSession){
    PlacementSession& session = *sceneSession.session;
    if (session.isGestureActive()){
        session.end();
    }
    sceneSession.input = GestureInput::None;
    // the session gives up at maxSettleSeconds of simulated time; one more second is the margin
    const double limit = std::max(0.0, (double)settings().maxSettleSeconds) + 1.0;
    for (double simulated = 0.0; simulated < limit && session.isBusy(); simulated += SETTLE_STEP_SECONDS){
        session.tick(SETTLE_STEP_SECONDS);
    }
    if (session.isBusy()){
        session.cancel();
    }
    if (SceneProject* sceneProject = sceneFor(session.getSceneId())){
        sceneProject->needUpdateRender = true;
    }
}

void editor::PhysicsPlacementWindow::finishAll(){
    for (auto& entry : sessions){
        finish(entry.second);
    }
}

void editor::PhysicsPlacementWindow::dropSessions(bool cancel){
    for (auto& entry : sessions){
        if (cancel && entry.second.session->isBusy()){
            entry.second.session->cancel();
        }
        if (SceneProject* sceneProject = sceneFor(entry.first)){
            sceneProject->needUpdateRender = true;
        }
    }
    sessions.clear();
}

void editor::PhysicsPlacementWindow::physicsModeChanged(bool on){
    if (!on){
        // leaving physics keeps what was done: a settle runs to its end, then commits
        finishAll();
        dropSessions(false);
    }
    Backend::getApp().requestRedraw();
}

void editor::PhysicsPlacementWindow::toolChanged(bool physicsTool){
    if (!physicsTool){
        finishAll();
    }
}

void editor::PhysicsPlacementWindow::cancelGestures(){
    for (auto& entry : sessions){
        if (entry.second.session->isBusy()){
            entry.second.session->cancel();
            if (SceneProject* sceneProject = sceneFor(entry.first)){
                sceneProject->needUpdateRender = true;
            }
        }
        entry.second.input = GestureInput::None;
    }
}

void editor::PhysicsPlacementWindow::sceneDeleting(uint32_t sceneId){
    auto it = sessions.find(sceneId);
    if (it == sessions.end()){
        return;
    }
    if (it->second.session->isBusy()){
        it->second.session->cancel();
    }
    sessions.erase(it);
}

void editor::PhysicsPlacementWindow::resetForProjectSwitch(){
    dropSessions(true);
    placementProjectSwitching();
    settingsDirty = false;
    hasLastTick = false;
    lastSelectedSceneId = NULL_PROJECT_SCENE;
}

//=== Scene view ===

bool editor::PhysicsPlacementWindow::isShownInScene(uint32_t sceneId) const{
    MainToolbar* toolbar = Backend::getApp().getMainToolbar();
    if (!toolbar || !toolbar->isPhysicsMode() || project->isAnyScenePlaying()){
        return false;
    }
    const SceneProject* sceneProject = sceneFor(sceneId);
    return sceneProject && sceneProject->sceneType == SceneType::SCENE_3D && sceneProject->playState == ScenePlayState::STOPPED;
}

bool editor::PhysicsPlacementWindow::isActiveInScene(uint32_t sceneId) const{
    if (!isShownInScene(sceneId)){
        return false;
    }
    SceneProject* sceneProject = sceneFor(sceneId);
    SceneRender* sceneRender = sceneProject->sceneRender;
    if (sceneRender->getCursorSelected() != CursorSelected::POINTER || sceneRender->isPreviewCameraActive() || sceneRender->hasEditLayer()){
        return false;
    }
    if (sceneRender->getToolsLayer()->getGizmoSelected() == GizmoSelected::OBJECT2D){
        return false;
    }
    TerrainEditWindow* terrainEditWindow = Backend::getApp().getTerrainEditWindow();
    return !(terrainEditWindow && terrainEditWindow->isEditingScene(sceneProject->scene));
}

uint32_t editor::PhysicsPlacementWindow::findSceneId(const Scene* scene) const{
    if (!scene){
        return NULL_PROJECT_SCENE;
    }
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()){
        if (sceneProject->scene == scene){
            return sceneProject->id;
        }
    }
    return NULL_PROJECT_SCENE;
}

bool editor::PhysicsPlacementWindow::beginGesture(uint32_t sceneId, PlacementGesture gesture, GestureInput input){
    if (input == GestureInput::None || input == GestureInput::Panel || !isActiveInScene(sceneId)){
        return false;
    }
    PlacementSession* session = sessionFor(sceneId);
    SceneSession* sceneSession = findSession(sceneId);
    if (!session || !sceneSession){
        return false;
    }
    const std::vector<Entity> selection = project->getSelectedEntities(sceneId);
    sceneSession->selection = selection;
    const bool started = !selection.empty() && session->begin(gesture, selection);
    sceneSession->input = started ? input : GestureInput::None;
    if (SceneProject* sceneProject = sceneFor(sceneId)){
        sceneProject->needUpdateRender = true;
    }
    Backend::getApp().requestRedraw();
    return started;
}

editor::PlacementSession* editor::PhysicsPlacementWindow::heldGesture(uint32_t sceneId, GestureInput input) const{
    const SceneSession* sceneSession = findSession(sceneId);
    if (!sceneSession || input == GestureInput::None || sceneSession->input != input || !sceneSession->session->isGestureActive()){
        return nullptr;
    }
    return sceneSession->session.get();
}

void editor::PhysicsPlacementWindow::endGesture(uint32_t sceneId, GestureInput input){
    if (PlacementSession* session = heldGesture(sceneId, input)){
        session->end();
        findSession(sceneId)->input = GestureInput::None;
        if (SceneProject* sceneProject = sceneFor(sceneId)){
            sceneProject->needUpdateRender = true;
        }
        Backend::getApp().requestRedraw();
    }
}

bool editor::PhysicsPlacementWindow::cancelGesture(uint32_t sceneId){
    SceneSession* sceneSession = findSession(sceneId);
    if (!sceneSession || !sceneSession->session->isBusy()){
        return false;
    }
    sceneSession->session->cancel();
    sceneSession->input = GestureInput::None;
    if (SceneProject* sceneProject = sceneFor(sceneId)){
        sceneProject->needUpdateRender = true;
    }
    Backend::getApp().requestRedraw();
    return true;
}

bool editor::PhysicsPlacementWindow::isBusy(uint32_t sceneId) const{
    const SceneSession* sceneSession = findSession(sceneId);
    return sceneSession && sceneSession->session->isBusy();
}

bool editor::PhysicsPlacementWindow::collectOverlay(uint32_t sceneId, PlacementOverlay& overlay) const{
    const SceneSession* sceneSession = findSession(sceneId);
    if (!sceneSession || !isShownInScene(sceneId)){
        return false;
    }
    sceneSession->session->collectOverlay(overlay);
    return true;
}

//=== Tick ===

void editor::PhysicsPlacementWindow::tick(){
    // wall-clock time since the last frame; a stall (a modal dialog, a load) is not caught up
    const auto now = std::chrono::steady_clock::now();
    double deltaSeconds = hasLastTick ? std::chrono::duration<double>(now - lastTick).count() : 0.0;
    lastTick = now;
    hasLastTick = true;
    deltaSeconds = std::clamp(deltaSeconds, 0.0, 0.25);

    MainToolbar* toolbar = Backend::getApp().getMainToolbar();
    if (!toolbar || !toolbar->isPhysicsMode() || project->isAnyScenePlaying()){
        // Play cancelled every gesture already (stopTransientPreviews)
        if (!sessions.empty()){
            dropSessions(true);
        }
        lastSelectedSceneId = project->getSelectedSceneId();
        return;
    }

    // the input moved to another scene: a gesture held in the one it left is released there
    const uint32_t selectedSceneId = project->getSelectedSceneId();
    if (selectedSceneId != lastSelectedSceneId){
        for (auto& entry : sessions){
            if (entry.first != selectedSceneId && entry.second.session->isGestureActive()){
                entry.second.session->end();
                entry.second.input = GestureInput::None;
            }
        }
        lastSelectedSceneId = selectedSceneId;
    }

    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()){
        if (!isShownInScene(sceneProject->id) || !sessionFor(sceneProject->id)){
            continue;
        }
        SceneSession& sceneSession = *findSession(sceneProject->id);
        PlacementSession& session = *sceneSession.session;

        // a release the Scene view did not see (the key or the button went up over another window)
        if (sceneSession.input != GestureInput::None && session.isGestureActive()){
            const bool held = sceneSession.input == GestureInput::Space
                ? ImGui::IsKeyDown(ImGuiKey_Space)
                : ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (!held){
                session.end();
            }
        }
        if (!session.isGestureActive()){
            sceneSession.input = GestureInput::None;
        }

        if (sceneProject->structureVersion != sceneSession.structureVersion){
            sceneSession.structureVersion = sceneProject->structureVersion;
            session.notifySceneChanged();
        }

        if (!session.isBusy()){
            std::vector<Entity> selection = project->getSelectedEntities(sceneProject->id);
            if (selection != sceneSession.selection){
                sceneSession.selection = std::move(selection);
                session.setSelection(sceneSession.selection);
                sceneProject->needUpdateRender = true;
            }
        }

        session.tick(deltaSeconds);

        if (session.isBusy()){
            // rendering is on demand: a simulation needs every frame, held button or not
            sceneProject->needUpdateRender = true;
            Backend::getApp().requestRedraw();
        }
    }
}

//=== HUD ===

void editor::PhysicsPlacementWindow::drawSceneBadge(uint32_t sceneId) const{
    if (!isShownInScene(sceneId)){
        return;
    }
    const char* text = "PHYSICS";
    const ImVec2 padding = Theme::dpi(ImVec2(6.0f, 1.0f));
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 size(textSize.x + padding.x * 2.0f, ImGui::GetFrameHeight());

    ImGui::SameLine(0, Theme::dpi(8.0f));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const float inset = (size.y - textSize.y - padding.y * 2.0f) * 0.5f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ImVec2(min.x, min.y + inset), ImVec2(min.x + size.x, min.y + size.y - inset),
                            ImGui::ColorConvertFloat4ToU32(Theme::Colors::PhysicsAccent), Theme::dpi(3.0f));
    drawList->AddText(ImVec2(min.x + padding.x, min.y + (size.y - textSize.y) * 0.5f), IM_COL32(255, 255, 255, 255), text);
    ImGui::SetItemTooltip("Physics (G) is on: Move, Rotate and Scale simulate, the selection stops at what it touches.\n"
                          "Hold Space to drop, Shift-drag a selected object to snap, Esc to cancel.");
}

void editor::PhysicsPlacementWindow::drawSceneHud(uint32_t sceneId, ImDrawList* drawList, const ImVec2& canvasMin, const ImVec2& canvasMax,
                                                  const std::function<bool(const Vector3&, ImVec2&)>& projectPoint) const{
    if (!isShownInScene(sceneId)){
        return;
    }
    SceneProject* sceneProject = sceneFor(sceneId);
    const SceneSession* sceneSession = findSession(sceneId);
    PlacementHud hud;
    hud.status = "Ready";
    if (sceneSession){
        hud = sceneSession->session->getHud();
    }

    const bool active = isActiveInScene(sceneId);
    std::string title = std::string("PHYSICS \xC2\xB7 ") + (hud.gesture != PlacementGesture::None ? gestureName(hud.gesture) : toolGestureName(*sceneProject->sceneRender));
    std::string status = hud.status;
    if (hud.phase == PlacementPhase::Idle && !active){
        // why the Scene view input is not physics' now
        SceneRender* sceneRender = sceneProject->sceneRender;
        if (sceneRender->hasEditLayer()){
            status = "Not for objects of a layered scene";
        }else if (sceneRender->getCursorSelected() == CursorSelected::HAND){
            status = "Pick Move (W), Rotate (E) or Scale (R)";
        }else{
            status = "Paused while the terrain brush is on";
        }
    }
    char counts[96];
    std::snprintf(counts, sizeof(counts), "%zu %s \xC2\xB7 %zu context", hud.movers, hud.movers == 1 ? "mover" : "movers", hud.contextBodies);
    const char* hint = "Space drop \xC2\xB7 Shift-drag snap \xC2\xB7 Esc cancel";

    const char* lines[4] = {title.c_str(), status.c_str(), counts, hint};
    const ImU32 colors[4] = {ImGui::ColorConvertFloat4ToU32(ImLerp(Theme::Colors::PhysicsAccent, ImVec4(1, 1, 1, 1), 0.25f)),
                             HUD_TEXT, HUD_DIM_TEXT, ImGui::ColorConvertFloat4ToU32(Theme::Colors::SubtleText)};
    float boxWidth = 0.0f;
    for (const char* line : lines) boxWidth = std::max(boxWidth, ImGui::CalcTextSize(line).x);
    const ImVec2 pad = Theme::dpi(ImVec2(10.0f, 8.0f));
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 boxSize(boxWidth + pad.x * 2.0f, lineHeight * 4.0f + pad.y * 2.0f - ImGui::GetStyle().ItemSpacing.y);
    const float margin = Theme::dpi(8.0f);
    const ImVec2 boxMin(canvasMin.x + margin, canvasMin.y + margin);

    drawList->PushClipRect(canvasMin, canvasMax, true);
    drawList->AddRectFilled(boxMin, ImVec2(boxMin.x + boxSize.x, boxMin.y + boxSize.y), HUD_BACKGROUND, Theme::dpi(3.0f));
    for (int i = 0; i < 4; i++){
        drawList->AddText(ImVec2(boxMin.x + pad.x, boxMin.y + pad.y + lineHeight * (float)i), colors[i], lines[i]);
    }

    // labels at their world points: why an object can't move, how far the surroundings are
    PlacementOverlay overlay;
    if (sceneSession){
        sceneSession->session->collectOverlay(overlay);
    }
    auto label = [&](const Vector3& world, const std::string& text, ImU32 color){
        ImVec2 screen;
        if (!projectPoint(world, screen)){
            return;
        }
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 labelPad = Theme::dpi(ImVec2(4.0f, 2.0f));
        const ImVec2 min(screen.x - size.x * 0.5f - labelPad.x, screen.y - size.y - labelPad.y * 2.0f - Theme::dpi(4.0f));
        drawList->AddRectFilled(min, ImVec2(min.x + size.x + labelPad.x * 2.0f, min.y + size.y + labelPad.y * 2.0f), HUD_BACKGROUND, Theme::dpi(2.0f));
        drawList->AddText(ImVec2(min.x + labelPad.x, min.y + labelPad.y), color, text.c_str());
    };
    const ImU32 warning = ImGui::ColorConvertFloat4ToU32(Theme::Colors::WarningText);
    for (const PlacementRefusal& refusal : overlay.refused){
        const Transform* transform = sceneProject->scene->findComponent<Transform>(refusal.entity);
        if (!transform){
            continue;
        }
        Vector3 anchor = transform->worldPosition;
        if (const MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(refusal.entity)){
            if (!mesh->worldAABB.isNull() && !mesh->worldAABB.isInfinite()){
                const AABB& box = mesh->worldAABB;
                anchor = Vector3(box.getCenter().x, box.getMaximum().y, box.getCenter().z);
            }
        }
        label(anchor, "Physics: " + refusal.reason, warning);
    }
    for (const PlacementOverlay::Distance& distance : overlay.distances){
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f m", distance.meters);
        label(distance.to, text, HUD_TEXT);
    }
    drawList->PopClipRect();
}

//=== Panel ===

void editor::PhysicsPlacementWindow::show(){
    tick();

    if (settingsDirty && !ImGui::IsAnyItemActive()){
        project->saveWorkspaceFile();
        settingsDirty = false;
    }

    if (!windowOpen){
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, ImGui::GetFontSize() * 38.0f), ImGuiCond_FirstUseEver);
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (ImGui::Begin(WINDOW_NAME, &windowOpen)){
        drawPanel();
    }
    ImGui::End();
}

bool editor::PhysicsPlacementWindow::bakeMeshes(SceneProject* sceneProject, const std::vector<Entity>& roots, size_t& requested){
    requested = 0;
    ConvexDecompositionCache* cache = placementDecompositionCache(project);
    if (!cache || !sceneProject || !sceneProject->scene){
        return false;
    }
    Scene* scene = sceneProject->scene;
    auto underRoots = [&](Entity entity){
        if (roots.empty()){
            return true;
        }
        for (Entity current = entity; current != NULL_ENTITY; ){
            if (std::find(roots.begin(), roots.end(), current) != roots.end()){
                return true;
            }
            const Transform* transform = scene->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
        return false;
    };

    const DecompositionParams params = DecompositionParams::fromQuality(settings().quality);
    std::vector<Vector3> positions;
    std::vector<uint32_t> indices;
    for (Entity entity : sceneProject->entities){
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || !underRoots(entity)){
            continue;
        }
        meshGeometry(*mesh, positions, indices);
        if (positions.size() < 4 || indices.size() < 3){
            continue;
        }
        cache->request(positions, indices, params);
        requested++;
    }
    return true;
}

void editor::PhysicsPlacementWindow::drawCacheRow(){
    ConvexDecompositionCache* cache = placementDecompositionCache(project);
    propertyRow("Collider cache", "Convex decompositions of meshes, kept in .jasperengine/cache/colliders by geometry, so an edited mesh never gets stale ones.");
    if (!cache){
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("No project cache");
        return;
    }
    const size_t entries = cache->entryCount();
    const size_t pending = cache->pendingCount();
    std::string text = std::to_string(entries) + (entries == 1 ? " mesh" : " meshes") + " \xC2\xB7 " + formatBytes(cache->bytesOnDisk());
    if (pending > 0){
        text += " \xC2\xB7 baking " + std::to_string(pending);
        // the count moves on as jobs land
        Backend::getApp().requestRedraw();
    }
    const char* clearLabel = "Clear##physics_cache_clear";
    const float clearWidth = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text.c_str());
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - clearWidth));
    ImGui::BeginDisabled(entries == 0 && pending == 0);
    if (ImGui::Button(clearLabel)){
        Project* owner = project;
        Backend::getApp().registerConfirmAlert("Clear Collider Cache",
            "Delete every cached convex decomposition of this project?\nPhysics Placement bakes them again when it needs them.",
            [owner](){
                if (ConvexDecompositionCache* current = placementDecompositionCache(owner)){
                    current->clear();
                }
            });
    }
    ImGui::EndDisabled();
}

void editor::PhysicsPlacementWindow::drawPanel(){
    PhysicsPlacementSettings& s = settings();
    bool changed = false;

    MainToolbar* toolbar = Backend::getApp().getMainToolbar();
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = sceneFor(sceneId);
    const bool shown = isShownInScene(sceneId);
    const std::vector<Entity> selection = sceneProject ? project->getSelectedEntities(sceneId) : std::vector<Entity>();

    // Physics on/off and what it is doing in the selected scene
    {
        const bool on = toolbar->isPhysicsMode();
        ImGui::BeginDisabled(!on && !toolbar->canUsePhysics());
        if (on){
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::PhysicsAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Colors::PhysicsAccentHovered);
        }
        if (ImGui::Button(on ? ICON_FA_MAGNET "  Physics on##physics_toggle" : ICON_FA_MAGNET "  Physics off##physics_toggle",
                          ImVec2(ImGui::GetFontSize() * 8.0f, 0.0f))){
            toolbar->togglePhysicsMode();
        }
        if (on){
            ImGui::PopStyleColor(2);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Physics (G): Move, Rotate and Scale simulate in 3D scenes");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (shown){
            const SceneSession* sceneSession = findSession(sceneId);
            ImGui::TextUnformatted(sceneSession ? sceneSession->session->getHud().status.c_str() : "Ready");
        }else if (project->isAnyScenePlaying()){
            ImGui::TextDisabled("Not in Play mode");
        }else if (sceneProject && sceneProject->sceneType != SceneType::SCENE_3D){
            ImGui::TextDisabled("3D scenes only");
        }else{
            ImGui::TextDisabled("Press G in the Scene view");
        }
    }
    ImGui::Spacing();

    if (beginProperties("physics_placement_settings")){
        propertyRow("Feel", "How eagerly objects follow the handle: Precise is slow and careful, Fast is quick and loose. Advanced overrides single values.");
        {
            int feel = (int)s.feel;
            const char* labels[] = {"Precise", "Balanced", "Fast"};
            const char* tooltips[] = {"4 m/s, 0.3 m/s into contacts, 2 substeps", "10 m/s, 1 m/s into contacts", "25 m/s, 3 m/s into contacts"};
            if (segmentedButtons("feel", feel, labels, tooltips, 3)){
                s.feel = (PlacementFeel)feel;
                changed = true;
            }
        }

        propertyRow("Settle on release", "After a drag, the objects fall until they rest, then the move is committed (one undo step).");
        changed |= ImGui::Checkbox("##settle_on_release", &s.settleOnRelease);
        propertyRow("Keep formation", "A multi-selection keeps its layout while it moves.");
        changed |= ImGui::Checkbox("##keep_formation", &s.keepFormation);
        propertyRow("Push neighbours", "Small unselected objects near the selection can be knocked about (and are committed with it).");
        changed |= ImGui::Checkbox("##push_neighbours", &s.pushNeighbours);
        propertyRow("Rotate about", "Rotate (E): the selection turns rigidly about its centre, or each object about its own.");
        {
            int pivot = (int)s.rotatePivot;
            const char* labels[] = {"Group centre", "Each object"};
            if (segmentedButtons("rotate_pivot", pivot, labels, nullptr, 2)){
                s.rotatePivot = (PlacementRotatePivot)pivot;
                changed = true;
            }
        }

        // Gravity: the vector, a Down shortcut and its strength
        propertyRow("Gravity", "Drop (hold Space) and settling pull this way, in m/s\xC2\xB2.");
        {
            const float downWidth = ImGui::CalcTextSize("Down").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - downWidth - ImGui::GetStyle().ItemSpacing.x));
            float gravity[3] = {s.gravity.x, s.gravity.y, s.gravity.z};
            if (ImGui::DragFloat3("##gravity", gravity, 0.05f, -100.0f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)){
                s.gravity = Vector3(gravity[0], gravity[1], gravity[2]);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Down##gravity_down")){
                s.gravity = Vector3(0.0f, -std::max(s.gravity.length(), 0.0f), 0.0f);
                if (s.gravity.y == 0.0f){
                    s.gravity.y = -9.81f;
                }
                changed = true;
            }
            ImGui::SetItemTooltip("Straight down, same strength");
        }
        propertyRow("Strength", "Gravity's length, its direction kept.");
        {
            float strength = s.gravity.length();
            if (UIUtils::sliderFloatInput("##gravity_strength", &strength, 0.0f, 50.0f, "%.2f")){
                const Vector3 direction = s.gravity.length() > 1e-6f ? s.gravity.normalized() : Vector3(0.0f, -1.0f, 0.0f);
                s.gravity = direction * std::max(strength, 0.0f);
                changed = true;
            }
        }
        propertyRow("Toward surface", "Drop pulls toward the surface under the cursor instead of along the gravity vector.");
        changed |= ImGui::Checkbox("##gravity_toward_surface", &s.gravityTowardSurface);

        // Colliders and their decomposition quality
        propertyRow("Colliders", "Auto: authored colliders if any, else a fitted primitive, a decomposition or a convex hull.\n"
                                 "Authored: colliders only (objects without any are refused). Generated: ignore colliders. Both: the two together.");
        {
            const char* names[] = {"Auto", "Authored", "Generated", "Both"};
            int policy = (int)s.colliders;
            if (ImGui::Combo("##colliders", &policy, names, IM_ARRAYSIZE(names))){
                s.colliders = (PlacementColliderPolicy)policy;
                changed = true;
            }
        }
        propertyRow("Quality", "Convex decomposition detail: more and tighter hulls take longer to bake.");
        {
            const char* names[] = {"Low", "Medium", "High"};
            int quality = (int)s.quality;
            if (ImGui::Combo("##quality", &quality, names, IM_ARRAYSIZE(names))){
                s.quality = (DecompositionQuality)quality;
                changed = true;
            }
        }
        propertyRow("Bake", "Decompose meshes now, in the background, so they are ready when they are moved.");
        {
            ConvexDecompositionCache* cache = placementDecompositionCache(project);
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - spacing) * 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, ImGui::GetStyle().ItemSpacing.y));
            size_t requested = 0;
            ImGui::BeginDisabled(!cache || !sceneProject || selection.empty());
            if (ImGui::Button("Bake selection##bake_selection", ImVec2(width, 0.0f))){
                bakeMeshes(sceneProject, placementRoots(project, sceneId, selection), requested);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(cache ? "Every mesh under the selected objects" : "Needs an open project");
            ImGui::SameLine();
            ImGui::BeginDisabled(!cache || !sceneProject);
            if (ImGui::Button("Bake scene##bake_scene", ImVec2(width, 0.0f))){
                bakeMeshes(sceneProject, {}, requested);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(cache ? "Every mesh of the scene" : "Needs an open project");
            ImGui::PopStyleVar();
        }

        // Snap (Shift-drag)
        propertyRow("Align to surface", "Snap (Shift-drag a selected object): its up axis follows the surface under the cursor.");
        changed |= ImGui::Checkbox("##align_to_surface", &s.alignToSurface);
        propertyRow("Snap offset", "Snap: extra gap above the surface, in metres.");
        changed |= ImGui::DragFloat("##snap_offset", &s.snapOffset, 0.005f, -1.0f, 1.0f, "%.3f m", ImGuiSliderFlags_AlwaysClamp);

        // Gather / Distribute: one gesture each, then a settle
        propertyRow("Gather/Spread", "Pull the selection to its centre, or space it evenly along an axis (keeping its order); each is one undo step.");
        {
            PlacementSession* session = shown ? sessionFor(sceneId) : nullptr;
            const bool canRun = session && !session->isBusy() && selection.size() >= 2;
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - spacing * 3.0f) / 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, ImGui::GetStyle().ItemSpacing.y));
            const char* disabledTip = !shown ? "Turn Physics (G) on in a 3D scene" : "Select two or more objects";
            ImGui::BeginDisabled(!canRun);
            if (ImGui::Button("Gather##gather", ImVec2(width, 0.0f))){
                session->gather(selection);
                findSession(sceneId)->input = GestureInput::None;
            }
            ImGui::SetItemTooltip("%s", canRun ? "Toward the selection's centre; they stop where they touch" : disabledTip);
            const char* axes[] = {"X##distribute_x", "Y##distribute_y", "Z##distribute_z"};
            for (int axis = 0; axis < 3; axis++){
                ImGui::SameLine();
                if (ImGui::Button(axes[axis], ImVec2(width, 0.0f))){
                    session->distribute(selection, axis);
                    findSession(sceneId)->input = GestureInput::None;
                }
                ImGui::SetItemTooltip("%s", canRun ? "Distribute evenly along this axis, between the outermost two" : disabledTip);
            }
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
        }

        propertyRow("Region margin", "How far around the moving objects the scene is gathered as surroundings (at least the objects' own size).");
        changed |= ImGui::DragFloat("##region_margin", &s.regionMargin, 0.05f, 0.0f, 100.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);

        propertyRow("Ignore layers", "Objects on these layers are neither moved nor collided with.");
        {
            const std::vector<std::string>& userNames = project->getLayerNames();
            auto layerName = [&userNames](int layer) -> std::string {
                if (TagManager::isBuiltinLayer(layer)) return TagManager::getBuiltinLayerName(layer);
                return layer < (int)userNames.size() ? TagManager::trimName(userNames[layer]) : std::string();
            };
            std::string preview = "Mixed\xE2\x80\xA6";
            int count = 0;
            int single = -1;
            for (int layer = 0; layer < MAX_LAYERS; layer++){
                if (s.ignoreLayerMask & (1u << layer)){
                    count++;
                    single = layer;
                }
            }
            if (count == 0){
                preview = "Nothing";
            }else if (count == MAX_LAYERS){
                preview = "Everything";
            }else if (count == 1){
                const std::string name = layerName(single);
                preview = name.empty() ? "Layer " + std::to_string(single) : name;
            }
            if (ImGui::BeginCombo("##ignore_layers", preview.c_str())){
                if (ImGui::Selectable("Nothing", count == 0)){
                    s.ignoreLayerMask = 0;
                    changed = true;
                }
                if (ImGui::Selectable("Everything", count == MAX_LAYERS)){
                    s.ignoreLayerMask = 0xFFFFFFFFu;
                    changed = true;
                }
                ImGui::Separator();
                for (int layer = 0; layer < MAX_LAYERS; layer++){
                    const std::string name = layerName(layer);
                    if (name.empty()){
                        continue;
                    }
                    const uint32_t bit = 1u << layer;
                    const bool ignored = (s.ignoreLayerMask & bit) != 0;
                    const std::string label = (ignored ? ICON_FA_CHECK "  " : "     ") + name + "##ignore_layer_" + std::to_string(layer);
                    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups)){
                        s.ignoreLayerMask ^= bit;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        }

        drawCacheRow();
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (Theme::foldoutHeader("Advanced##physics_advanced")){
        if (beginProperties("physics_placement_advanced")){
            const PlacementTuning preset = placementFeelTuning(s.feel);
            // a value the Feel preset gives until it is set here; ↺ hands it back to the preset
            auto presetFloat = [&](const char* label, const char* id, const char* tooltip, float& value, float presetValue,
                                   float scale, float speed, float minValue, float maxValue, const char* format){
                const bool overridden = value >= 0.0f;
                propertyRow(label, tooltip, !overridden);
                if (overridden){
                    if (resetToPresetButton(id)){
                        value = -1.0f;
                        changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1.0f);
                }
                float display = (overridden ? value : presetValue) * scale;
                if (ImGui::DragFloat((std::string("##") + id).c_str(), &display, speed, minValue, maxValue, format, ImGuiSliderFlags_AlwaysClamp)){
                    value = display / scale;
                    changed = true;
                }
            };
            auto presetInt = [&](const char* label, const char* id, const char* tooltip, int& value, int presetValue, int minValue, int maxValue){
                const bool overridden = value >= 0;
                propertyRow(label, tooltip, !overridden);
                if (overridden){
                    if (resetToPresetButton(id)){
                        value = -1;
                        changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1.0f);
                }
                int display = overridden ? value : presetValue;
                if (ImGui::DragInt((std::string("##") + id).c_str(), &display, 0.1f, minValue, maxValue, "%d", ImGuiSliderFlags_AlwaysClamp)){
                    value = display;
                    changed = true;
                }
            };
            auto plainFloat = [&](const char* label, const char* id, const char* tooltip, float& value, float speed, float minValue, float maxValue, const char* format){
                propertyRow(label, tooltip);
                changed |= ImGui::DragFloat((std::string("##") + id).c_str(), &value, speed, minValue, maxValue, format, ImGuiSliderFlags_AlwaysClamp);
            };

            presetFloat("Max speed", "max_speed", "Fastest an object follows the handle.", s.maxSpeed, preset.maxSpeed, 1.0f, 0.1f, 0.1f, 100.0f, "%.2f m/s");
            presetFloat("Contact speed", "contact_speed", "Fastest an object moves into something it touches (sliding along is not slowed).",
                        s.contactSpeed, preset.contactSpeed, 1.0f, 0.02f, 0.01f, 50.0f, "%.2f m/s");
            presetFloat("Max angular speed", "max_angular_speed", "Fastest an object turns.", s.maxAngularSpeed, preset.maxAngularSpeed,
                        Angle::radToDeg(1.0f), 1.0f, 1.0f, 5000.0f, "%.0f deg/s");
            presetInt("Substeps", "substeps", "Collision steps per simulation step: more catch thin objects better.", s.substeps, preset.substeps, 1, 16);
            presetInt("Velocity iterations", "velocity_iterations", "Solver velocity passes per step.", s.velocityIterations, preset.velocityIterations, 1, 50);
            presetInt("Position iterations", "position_iterations", "Solver position passes per step.", s.positionIterations, preset.positionIterations, 1, 50);
            plainFloat("Teleport distance", "teleport_distance", "A handle farther than this from the objects moves them there at once (the dashed ring).",
                       s.teleportDistance, 0.05f, 0.1f, 1000.0f, "%.2f m");
            plainFloat("Sleep speed", "sleep_linear", "Below this speed (and the turn rate below) an object is at rest.", s.sleepLinear, 0.001f, 0.0f, 1.0f, "%.3f m/s");
            plainFloat("Sleep turn rate", "sleep_angular", "Below this turn rate (and the speed above) an object is at rest.", s.sleepAngularDeg, 0.05f, 0.0f, 90.0f, "%.2f deg/s");
            plainFloat("Sleep time", "sleep_seconds", "How long an object stays below both before it counts as resting.", s.sleepSeconds, 0.01f, 0.0f, 5.0f, "%.2f s");
            plainFloat("Max settle time", "max_settle_seconds", "Settling stops (and commits) after this much simulated time.", s.maxSettleSeconds, 0.05f, 0.1f, 60.0f, "%.2f s");
            plainFloat("Kill depth", "kill_y_depth", "Objects that fall this far below the surroundings are frozen and reported as fallen out.",
                       s.killYDepth, 0.5f, 0.0f, 10000.0f, "%.1f m");
            plainFloat("Max object size", "max_mover_size", "Objects longer than this are surroundings, never moved.", s.maxMoverSize, 0.1f, 0.1f, 10000.0f, "%.1f m");
            propertyRow("Decomposition", "Never: hulls and primitives only. Cached: a decomposition when one is cached (and one is baked meanwhile). "
                                         "Always: wait for decompositions before simulating.");
            {
                const char* names[] = {"Never", "Cached", "Always"};
                int mode = (int)s.decomposition;
                if (ImGui::Combo("##decomposition", &mode, names, IM_ARRAYSIZE(names))){
                    s.decomposition = (PlacementDecomposition)mode;
                    changed = true;
                }
            }
            ImGui::EndTable();
        }
    }

    if (changed){
        applySettings();
    }
}
