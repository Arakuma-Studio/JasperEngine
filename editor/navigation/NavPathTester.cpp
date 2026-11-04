// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavPathTester.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "Theme.h"
#include "component/NavMeshSurfaceComponent.h"
#include "external/IconsFontAwesome6.h"
#include "imgui.h"
#include "navigation/NavMesh.h"
#include "navigation/NavigationEditor.h"
#include "subsystem/NavigationSystem.h"
#include "util/UIProbe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // "1.25" (three decimals at most, no trailing zeros), for the panel and Copy Query
    std::string testerNumber(float value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f", std::fabs(value) < 5e-4f ? 0.0f : value);
        std::string out = text;
        while (!out.empty() && out.back() == '0') out.pop_back();
        if (!out.empty() && out.back() == '.') out.pop_back();
        return out;
    }

    std::string testerVector(const Vector3& v) {
        return "[" + testerNumber(v.x) + ", " + testerNumber(v.y) + ", " + testerNumber(v.z) + "]";
    }

    std::string jsonString(const std::string& text) {
        std::string out = "\"";
        for (char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + "\"";
    }

    const char* statusLabel(nav::PathStatus status) {
        switch (status) {
            case nav::PathStatus::Complete: return "Complete";
            case nav::PathStatus::Partial: return "Partial";
            case nav::PathStatus::Invalid: return "Invalid";
        }
        return "Invalid";
    }

    const char* modeLabel(NavPathTester::Mode mode) {
        switch (mode) {
            case NavPathTester::Mode::Path: return "Path";
            case NavPathTester::Mode::Raycast: return "Raycast";
            case NavPathTester::Mode::ClosestEdge: return "Closest Edge";
            case NavPathTester::Mode::Locate: return "Locate";
        }
        return "Path";
    }

    // the first enabled surface of the agent type (entity order): the probe's when the point
    // found no polygon on any
    Entity firstSurfaceOf(Scene* scene, int agentType) {
        auto array = scene->getComponentArray<NavMeshSurfaceComponent>();
        Entity first = NULL_ENTITY;
        for (size_t i = 0; i < array->size(); i++) {
            const NavMeshSurfaceComponent& surface = array->getComponentFromIndex(i);
            const Entity entity = array->getEntity(i);
            if (surface.enabled && surface.agentType == agentType && (first == NULL_ENTITY || entity < first)) first = entity;
        }
        return first;
    }

    std::string entityName(Scene* scene, Entity entity) {
        if (entity == NULL_ENTITY || !scene) return "";
        return scene->getEntityName(entity);
    }

}

NavPathTester& NavPathTester::get() {
    static NavPathTester instance;
    return instance;
}

nav::QueryFilter NavPathTester::filter() const {
    nav::QueryFilter f;
    f.agentType = agentType;
    f.areaMask = areaMask;
    return f;
}

void NavPathTester::start(SceneProject& sceneProject) {
    if (active && activeScene != sceneProject.id) stop();
    if (!active) {
        // the agent type stays valid when the project's types changed since the last time
        agentType = nav::getSettings().agentTypeOrDefault(agentType).id;
    }
    active = true;
    activeScene = sceneProject.id;
    requery(sceneProject);
}

void NavPathTester::stop() {
    if (active) NavigationEditor::get().clearQueryDraw(activeScene);
    active = false;
    dragged = Handle::None;
    popupOpen = false;
}

void NavPathTester::toggle(SceneProject& sceneProject) {
    if (isActive(sceneProject.id)) {
        stop();
        sceneProject.needUpdateRender = true;
    } else {
        start(sceneProject);
    }
}

void NavPathTester::setMode(SceneProject& sceneProject, Mode value) {
    mode = value;
    requery(sceneProject);
}

void NavPathTester::setAgentType(SceneProject& sceneProject, int value) {
    agentType = value;
    requery(sceneProject);
}

void NavPathTester::setAreaMask(SceneProject& sceneProject, uint16_t value) {
    areaMask = value;
    requery(sceneProject);
}

void NavPathTester::setStart(SceneProject& sceneProject, const Vector3& point) {
    startPoint = point;
    startSet = true;
    requery(sceneProject);
}

void NavPathTester::setEnd(SceneProject& sceneProject, const Vector3& point) {
    endPoint = point;
    endSet = true;
    requery(sceneProject);
}

void NavPathTester::press(SceneProject& sceneProject, const Vector3& world, const Vector2& screen, bool shift, const Projector& toView,
                          float handlePixels) {
    // the nearest handle within reach of the press
    dragged = Handle::None;
    float best = handlePixels;
    auto consider = [&](bool set, const Vector3& point, Handle handle) {
        Vector2 at;
        if (!set || !toView || !toView(point, at)) return;
        const float distance = (at - screen).length();
        if (distance <= best) {
            best = distance;
            dragged = handle;
        }
    };
    consider(startSet, startPoint, Handle::Start);
    if (usesEnd()) consider(endSet, endPoint, Handle::End);
    if (dragged != Handle::None) {
        // the press itself does not move the handle: a click on it keeps it where it is
        popupOpen = false;
        return;
    }
    if (shift || !usesEnd()) {
        startPoint = world;
        startSet = true;
    } else {
        endPoint = world;
        endSet = true;
    }
    popupOpen = mode == Mode::Locate;
    requery(sceneProject);
}

void NavPathTester::drag(SceneProject& sceneProject, const Vector3& world) {
    if (dragged == Handle::None) return;
    Vector3& point = dragged == Handle::Start ? startPoint : endPoint;
    if (point == world) return;
    point = world;
    // the probe rebuilds tiles (up to 200 ms): it waits for the release
    requery(sceneProject, false);
}

void NavPathTester::release(SceneProject& sceneProject) {
    if (dragged == Handle::None) return;
    dragged = Handle::None;
    if (mode == Mode::Locate) {
        popupOpen = true;
        requery(sceneProject);
    }
}

void NavPathTester::requery(SceneProject& sceneProject, bool probe) {
    const bool keepProbe = !probe && mode == Mode::Locate;
    const Result previous = keepProbe ? result : Result();
    result = Result();
    NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
    if (system && startSet && (!usesEnd() || endSet)) {
        const nav::QueryFilter f = filter();
        result.ran = true;
        switch (mode) {
            case Mode::Path:
                result.path = system->calculatePath(startPoint, endPoint, f);
                break;
            case Mode::Raycast:
                result.blocked = system->raycast(startPoint, endPoint, result.hit, f);
                result.found = true;
                break;
            case Mode::ClosestEdge:
                result.found = system->findClosestEdge(startPoint, result.hit, f);
                break;
            case Mode::Locate:
                result.location = system->locate(startPoint, f);
                result.found = result.location.found;
                if (probe && sceneProject.scene) {
                    result.probeSurface = result.location.surface != NULL_ENTITY ? result.location.surface
                                                                                 : firstSurfaceOf(sceneProject.scene, agentType);
                    if (result.probeSurface != NULL_ENTITY) {
                        result.probe = NavWalkabilityProbe::explain(sceneProject.scene, *system, result.probeSurface, startPoint, f,
                                                                    NavigationEditor::get().keptInput(sceneProject, result.probeSurface));
                        result.probed = true;
                    }
                } else if (keepProbe) {
                    result.probeSurface = previous.probeSurface;
                    result.probe = previous.probe;
                    result.probed = previous.probed;
                }
                break;
        }
    }
    publish(sceneProject);
}

uint64_t NavPathTester::stateKey(SceneProject& sceneProject) const {
    uint64_t key = nav::getSettingsVersion() * 1099511628211ull ^ (uint64_t)sceneProject.playState;
    NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
    if (!system || !sceneProject.scene) return key;
    auto array = sceneProject.scene->getComponentArray<NavMeshSurfaceComponent>();
    for (size_t i = 0; i < array->size(); i++) {
        const Entity entity = array->getEntity(i);
        const nav::NavMesh* mesh = system->getNavMesh(entity);
        key = (key ^ ((uint64_t)entity << 32 | (mesh ? mesh->getRevision() : 0u)) ^ (array->getComponentFromIndex(i).enabled ? 1u : 0u)) *
              1099511628211ull;
    }
    return key;
}

void NavPathTester::refresh(SceneProject& sceneProject) {
    if (!isActive(sceneProject.id) || dragged != Handle::None) return;
    if (stateKey(sceneProject) != answeredKey) requery(sceneProject);
}

void NavPathTester::publish(SceneProject& sceneProject) {
    answeredKey = stateKey(sceneProject);
    sceneProject.needUpdateRender = true;
    if (!active || activeScene != sceneProject.id) return;
    NavQueryDraw draw;
    draw.from = startPoint;
    draw.hasFrom = startSet;
    draw.to = endPoint;
    draw.hasTo = endSet && usesEnd();
    // what each mode draws as a path: its corners, the ray to where it stops, the line to the
    // nearest edge or to the nearest point on the mesh
    switch (mode) {
        case Mode::Path:
            draw.path = result.path;
            break;
        case Mode::Raycast:
            if (result.ran) {
                draw.path.corners = {startPoint, result.blocked ? result.hit.position : endPoint};
                draw.path.status = result.blocked ? nav::PathStatus::Partial : nav::PathStatus::Complete;
            }
            break;
        case Mode::ClosestEdge:
            if (result.found) {
                draw.path.corners = {startPoint, result.hit.position};
                draw.path.status = nav::PathStatus::Complete;
            }
            break;
        case Mode::Locate:
            if (result.found && !result.location.onMesh) {
                draw.path.corners = {startPoint, result.location.position};
                draw.path.status = nav::PathStatus::Partial;
            }
            break;
    }
    if (draw.path.status == nav::PathStatus::Complete) draw.path.failure = nav::PathFailure::None;
    NavigationEditor::get().setQueryDraw(sceneProject.id, std::move(draw));
}

std::string NavPathTester::queryJson() const {
    std::string op;
    switch (mode) {
        case Mode::Path: op = "path"; break;
        case Mode::Raycast: op = "raycast"; break;
        case Mode::ClosestEdge: op = "closest_edge"; break;
        case Mode::Locate: op = "locate"; break;
    }
    std::string json = "{\"op\": \"" + op + "\", \"from\": " + testerVector(startPoint);
    if (usesEnd()) json += ", \"to\": " + testerVector(endPoint);
    json += ", \"agent_type\": " + jsonString(nav::getSettings().agentTypeOrDefault(agentType).name);
    if (areaMask != nav::ALL_AREAS) json += ", \"area_mask\": " + std::to_string(areaMask);
    return json + "}";
}

// ---------------------------------------------------------------- the panel

void NavPathTester::drawPanel(SceneProject& sceneProject, float viewMinX, float viewWidth, float top,
                              const std::function<bool(const Vector3& world, float& x, float& y)>& toScreen) {
    if (!isActive(sceneProject.id)) return;
    const nav::NavigationSettings& settings = nav::getSettings();
    Scene* scene = sceneProject.scene;
    const float width = Theme::dpi(270.0f);
    const float margin = Theme::dpi(8.0f);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(viewMinX + viewWidth - width - margin, top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    const std::string id = "##nav_path_tester" + std::to_string(sceneProject.id);
    if (ImGui::BeginChild(id.c_str(), ImVec2(width, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        ImGui::TextUnformatted(ICON_FA_ROUTE " Path Tester");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (ImGui::SmallButton(ICON_FA_XMARK "##closePathTester")) {
            stop();
            sceneProject.needUpdateRender = true;
        }
        ImGui::SetItemTooltip("Close the Path Tester (Esc)");
        ImGui::Separator();

        const float labelWidth = Theme::dpi(80.0f);
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(labelWidth);
            ImGui::SetNextItemWidth(-1);
        };

        label("Agent Type");
        const nav::AgentTypeSettings current = settings.agentTypeOrDefault(agentType);
        if (ImGui::BeginCombo("##testerAgentType", current.name.c_str())) {
            for (const nav::AgentTypeSettings& type : settings.agentTypes) {
                if (ImGui::Selectable(type.name.c_str(), type.id == current.id)) setAgentType(sceneProject, type.id);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The agent type whose surfaces answer (Window > AI > Navigation > Agents)");

        label("Area Mask");
        int enabledAreas = 0;
        for (int area = 0; area < nav::AREA_COUNT; area++) enabledAreas += (areaMask >> area) & 1;
        const std::string maskText = areaMask == nav::ALL_AREAS ? std::string("Everything")
                                   : enabledAreas == 0 ? std::string("Nothing") : std::to_string(enabledAreas) + " areas";
        if (ImGui::BeginCombo("##testerAreaMask", maskText.c_str())) {
            if (ImGui::Selectable("Everything", areaMask == nav::ALL_AREAS, ImGuiSelectableFlags_DontClosePopups))
                setAreaMask(sceneProject, nav::ALL_AREAS);
            if (ImGui::Selectable("Nothing", areaMask == 0, ImGuiSelectableFlags_DontClosePopups)) setAreaMask(sceneProject, 0);
            ImGui::Separator();
            for (int area = 0; area < nav::AREA_COUNT; area++) {
                const std::string& name = settings.areas[(size_t)area].name;
                if (name.empty()) continue;
                bool on = (areaMask >> area) & 1;
                if (ImGui::Checkbox((name + "##area" + std::to_string(area)).c_str(), &on))
                    setAreaMask(sceneProject, (uint16_t)(on ? areaMask | (1u << area) : areaMask & ~(1u << area)));
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The areas the query may enter (a NavMesh Agent's Area Mask)");

        label("Mode");
        if (ImGui::BeginCombo("##testerMode", modeLabel(mode))) {
            for (Mode m : {Mode::Path, Mode::Raycast, Mode::ClosestEdge, Mode::Locate}) {
                if (ImGui::Selectable(modeLabel(m), m == mode)) setMode(sceneProject, m);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Path: calculatePath; Raycast: a straight walk on the mesh; Closest Edge: the nearest wall; "
                              "Locate: where a point is, and why it is not walkable");

        ImGui::Separator();
        const Result& r = result;
        if (!r.ran) {
            ImGui::TextDisabled(usesEnd() ? (startSet ? "Click to set the end." : "Shift+click to set the start, click the end.")
                                          : "Click a point.");
        } else if (mode == Mode::Path) {
            const nav::NavPath& path = r.path;
            const ImVec4 color = path.status == nav::PathStatus::Complete ? ImVec4(0.45f, 0.9f, 0.45f, 1.0f)
                               : path.status == nav::PathStatus::Partial ? ImVec4(1.0f, 0.65f, 0.2f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            ImGui::TextColored(color, "%s", statusLabel(path.status));
            // UI tests read the answer by name (util/UIProbe.h)
            UIProbe::item(std::string("pathTester.status.") + nav::pathStatusName(path.status));
            if (path.failure != nav::PathFailure::None) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", nav::pathFailureName(path.failure));
                UIProbe::item(std::string("pathTester.reason.") + nav::pathFailureName(path.failure));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(nav::pathFailureDescription(path.failure));
                ImGui::PopTextWrapPos();
            }
            if (path.status != nav::PathStatus::Invalid) {
                ImGui::Text("Length %s m, %d corners", testerNumber(path.length).c_str(), (int)path.corners.size());
                UIProbe::item("pathTester.length." + std::to_string((int)std::lround(path.length * 10.0f)));
            }
            if (path.status == nav::PathStatus::Partial) ImGui::Text("Gap %s m to the end", testerNumber(path.gap).c_str());
            auto snap = [](float distance) { return distance < 0.0f ? std::string("off the mesh") : testerNumber(distance) + " m"; };
            ImGui::Text("Snap: start %s, end %s", snap(path.startSnapDistance).c_str(), snap(path.endSnapDistance).c_str());
            auto island = [](int id) { return id < 0 ? std::string("\xE2\x80\x93") : std::to_string(id); };
            ImGui::Text("Islands: start %s, end %s", island(path.startIsland).c_str(), island(path.endIsland).c_str());
            if (path.surface != NULL_ENTITY) ImGui::TextDisabled("Surface: %s", entityName(scene, path.surface).c_str());
        } else if (mode == Mode::Raycast) {
            if (r.blocked && r.hit.distance <= 0.0f && (r.hit.position - startPoint).length() < 1e-4f) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "The start is off the mesh");
            } else if (r.blocked) {
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "Blocked after %s m", testerNumber(r.hit.distance).c_str());
                ImGui::Text("At %s (%s)", testerVector(r.hit.position).c_str(),
                            settings.areas[(size_t)std::min<int>(r.hit.area, nav::AREA_COUNT - 1)].name.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "Clear to the end");
            }
        } else if (mode == Mode::ClosestEdge) {
            if (r.found) {
                ImGui::Text("Nearest edge %s m away", testerNumber(r.hit.distance).c_str());
                ImGui::Text("At %s", testerVector(r.hit.position).c_str());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "The point is off the mesh");
            }
        } else {
            const nav::NavLocation& at = r.location;
            if (at.onMesh) {
                ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "On the mesh");
            } else if (at.found) {
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "Off the mesh: the nearest point is %s m away",
                                   testerNumber(at.snapDistance).c_str());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Off the mesh: nothing within 5 m");
            }
            UIProbe::item(std::string("pathTester.locate.") + (at.onMesh ? "on_mesh" : at.found ? "near_mesh" : "not_found"));
            if (at.found) {
                ImGui::Text("Island %s, area %s", at.island < 0 ? "\xE2\x80\x93" : std::to_string(at.island).c_str(),
                            settings.areas[(size_t)std::min<int>(at.area, nav::AREA_COUNT - 1)].name.c_str());
                ImGui::TextDisabled("Surface: %s", entityName(scene, at.surface).c_str());
            }
            if (r.probed) {
                ImGui::Text("%d reason(s) found", (int)r.probe.reasons.size());
                if (!r.probe.reasons.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(popupOpen ? "Hide" : "Show")) popupOpen = !popupOpen;
                }
            }
        }

        ImGui::Separator();
        ImGui::BeginDisabled(!startSet);
        if (ImGui::SmallButton(ICON_FA_COPY " Copy Query")) ImGui::SetClipboardText(queryJson().c_str());
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Copy the equivalent query_navigation arguments (JSON)");
        ImGui::SameLine();
        ImGui::TextDisabled(usesEnd() ? "Shift+click: start, click: end" : "Click: the point");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Locate: why the point is (not) walkable, beside it
    if (mode == Mode::Locate && popupOpen && result.probed && !result.probe.reasons.empty() && dragged == Handle::None) {
        float x = 0.0f, y = 0.0f;
        if (toScreen && toScreen(startPoint, x, y)) {
            const float popupWidth = Theme::dpi(300.0f);
            const float left = std::clamp(x + Theme::dpi(14.0f), viewMinX, std::max(viewMinX, viewMinX + viewWidth - popupWidth - margin));
            ImGui::SetCursorScreenPos(ImVec2(left, y + Theme::dpi(14.0f)));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
            const std::string popupId = "##nav_probe_popup" + std::to_string(sceneProject.id);
            if (ImGui::BeginChild(popupId.c_str(), ImVec2(popupWidth, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
                ImGui::TextUnformatted(ICON_FA_CIRCLE_QUESTION " Why not walkable?");
                ImGui::Separator();
                int index = 0;
                for (const NavProbeReason& reason : result.probe.reasons) {
                    ImGui::PushID(index++);
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", reason.code.c_str());
                    UIProbe::item("pathTester.why." + reason.code);
                    if (reason.limit != 0.0f) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s vs %s", testerNumber(reason.value).c_str(), testerNumber(reason.limit).c_str());
                    }
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(reason.message.c_str());
                    ImGui::PopTextWrapPos();
                    if (reason.entity != NULL_ENTITY && scene && scene->isEntityCreated(reason.entity)) {
                        if (ImGui::SmallButton((std::string(ICON_FA_ARROW_POINTER " Select ") + entityName(scene, reason.entity)).c_str())) {
                            if (Project* project = Backend::getApp().getProject()) project->setSelectedEntity(sceneProject.id, reason.entity);
                        }
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }
    ImGui::SetCursorScreenPos(cursor);
}
