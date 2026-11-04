// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemsWindow.h"

#include "Project.h"
#include "Catalog.h"
#include "Scene.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string componentLabel(const Scene* scene, ComponentId id){
        const std::string& name = scene->getComponentName(id);
        try {
            return Catalog::getComponentDisplayName(Catalog::getComponentType(name));
        } catch (const std::exception&) {
            return name;
        }
    }

    std::string bytesLabel(size_t bytes){
        char text[32];
        if (bytes >= 1024 * 1024){
            std::snprintf(text, sizeof(text), "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
        }else{
            std::snprintf(text, sizeof(text), "%.1f KiB", (double)bytes / 1024.0);
        }
        return text;
    }

}

void SystemsWindow::setOpen(bool open){
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void SystemsWindow::show(){
    rowRects.clear();
    if (!windowOpen) return;

    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)){
        ImGui::End();
        return;
    }

    // Play: the running game's scene; Edit: the selected scene (its statistics only)
    SceneProject* scene = nullptr;
    bool playing = false;
    const uint32_t playId = project->getPlaySessionSceneId();
    if (playId != NULL_PROJECT_SCENE){
        SceneProject* running = project->getScene(playId);
        if (running && running->scene && (running->playState == ScenePlayState::PLAYING || running->playState == ScenePlayState::PAUSED)){
            scene = running;
            playing = true;
        }
    }
    if (!scene){
        scene = project->getSelectedScene();
    }
    if (!scene || !scene->scene){
        ImGui::TextDisabled("No scene is open.");
        ImGui::End();
        return;
    }

    ImGui::Text("%s", scene->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(playing ? "(Play)" : "(Edit: Enabled and Every N apply in Play mode)");

    drawTable(scene, playing);
    if (ImGui::CollapsingHeader("Wave trace", ImGuiTreeNodeFlags_DefaultOpen)){
        drawTrace(scene);
    }
    ImGui::Separator();
    drawFooter(scene);

    ImGui::End();
}

void SystemsWindow::drawTable(SceneProject* sceneProject, bool playing){
    SystemScheduler& scheduler = sceneProject->scene->getScheduler();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    const float height = ImGui::GetTextLineHeightWithSpacing() * 14.0f;
    rowRects.clear();
    if (!ImGui::BeginTable("##systems", 8, flags, ImVec2(0.0f, height))){
        return;
    }
    // the rows' visible area: under the frozen header row, inside the table's scroll view
    const ImGuiTable* table = ImGui::GetCurrentTable();
    ImRect view = table->InnerClipRect;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Phase");
    ImGui::TableSetupColumn("Wave");
    ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Avg ms");
    ImGui::TableSetupColumn("Max ms");
    ImGui::TableSetupColumn("Last ms");
    ImGui::TableSetupColumn("Enabled");
    ImGui::TableSetupColumn("Every N");
    ImGui::TableHeadersRow();
    view.Min.y = std::max(view.Min.y, ImGui::GetCursorScreenPos().y);

    int row = 0;
    for (SystemPhase phase : {SystemPhase::FixedUpdate, SystemPhase::Update, SystemPhase::PostUpdate, SystemPhase::Draw}){
        const auto waves = scheduler.getWaves(phase);
        for (size_t wave = 0; wave < waves.size(); wave++){
            for (ScheduledSystem* system : waves[wave]){
                ImGui::PushID(row++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(systemPhaseName(phase));
                ImGui::TableNextColumn();
                ImGui::Text("%zu", wave);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(system->getName().c_str());
                const SystemTiming& timing = system->getTiming();
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", timing.averageMs());
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", timing.maxMs());
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", timing.lastMs());

                ImGui::BeginDisabled(!playing);
                ImGui::TableNextColumn();
                bool enabled = system->isEnabled();
                if (ImGui::Checkbox("##enabled", &enabled)){
                    scheduler.setEnabled(system->getName(), enabled);
                }
                const ImRect box(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                rowRects.push_back({systemPhaseName(phase), system->getName(), box.Min.x, box.Min.y, box.GetWidth(), box.GetHeight(),
                                    view.Contains(box)});
                ImGui::TableNextColumn();
                int every = (int)system->getEveryNFrames();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
                if (ImGui::InputInt("##every", &every, 0, 0)){
                    scheduler.setEveryNFrames(system->getName(), (uint32_t)std::clamp(every, 1, 1000));
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndTable();
}

void SystemsWindow::drawTrace(SceneProject* sceneProject){
    const auto& trace = sceneProject->scene->getScheduler().getLastTrace();
    if (trace.empty()){
        ImGui::TextDisabled("No frame has run yet.");
        return;
    }
    double end = 0.0;
    for (const WaveTraceEntry& entry : trace){
        end = std::max(end, entry.endUs);
    }
    end = std::max(end, 1.0);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float labelWidth = ImGui::GetFontSize() * 11.0f;
    const float barHeight = ImGui::GetTextLineHeight();
    const float width = std::max(ImGui::GetContentRegionAvail().x - labelWidth, 50.0f);
    for (const WaveTraceEntry& entry : trace){
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Text("%s/%s", systemPhaseName(entry.phase), entry.system->c_str());
        const float x0 = origin.x + labelWidth + (float)(entry.startUs / end) * width;
        const float x1 = std::max(x0 + 1.0f, origin.x + labelWidth + (float)(entry.endUs / end) * width);
        draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + barHeight),
                            ImGui::GetColorU32(entry.wave % 2 ? ImGuiCol_PlotHistogramHovered : ImGuiCol_PlotHistogram));
        if (ImGui::IsMouseHoveringRect(ImVec2(x0, origin.y), ImVec2(x1, origin.y + barHeight))){
            ImGui::SetTooltip("%s, wave %u: %.1f us to %.1f us", entry.system->c_str(), entry.wave, entry.startUs, entry.endUs);
        }
    }
    ImGui::TextDisabled("Frame: %.1f us", end);
}

void SystemsWindow::drawFooter(SceneProject* sceneProject){
    Scene* scene = sceneProject->scene;
    size_t holes = 0, dirty = 0, bytes = scene->getMetadataBytes(), heap = 0, used = 0;
    // what each type's components hold in arrays of their own (one pass: while the window draws)
    std::vector<size_t> heapOf(scene->getComponentTypeCount(), 0);
    for (size_t id = 0; id < scene->getComponentTypeCount(); id++){
        ComponentArrayBase* array = scene->getComponentArray((ComponentId)id);
        holes += array->getHoleCount();
        dirty += array->isOrderDirty() ? 1 : 0;
        bytes += array->componentBytes() + array->overheadBytes();
        heapOf[id] = array->arrayHeapBytes();
        heap += heapOf[id];
        used += array->size() > 0 ? 1 : 0;
    }
    const size_t entities = scene->getEntityCount();
    const size_t systemEntities = scene->getSystemEntityCount();
    ImGui::Text("Entities: %zu (%zu user, %zu system)", entities, entities - systemEntities, systemEntities);
    ImGui::Text("Component types: %zu / %zu (%zu in use)", scene->getComponentTypeCount(), kMaxComponentTypes, used);
    ImGui::Text("Pending holes: %zu   Arrays to re-sort: %zu   ECS memory: %s (+ %s in component arrays)", holes, dirty,
                bytesLabel(bytes).c_str(), bytesLabel(heap).c_str());
    if (ImGui::TreeNode("Components per type")){
        for (size_t id = 0; id < scene->getComponentTypeCount(); id++){
            const ComponentArrayBase* array = scene->getComponentArray((ComponentId)id);
            if (array->size() > 0){
                ImGui::Text("%s: %zu (%s each, %s in arrays)", componentLabel(scene, (ComponentId)id).c_str(), array->size(),
                            bytesLabel(array->componentSize()).c_str(), bytesLabel(heapOf[id]).c_str());
            }
        }
        ImGui::TreePop();
    }
}
