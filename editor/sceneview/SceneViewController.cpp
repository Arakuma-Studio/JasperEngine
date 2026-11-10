// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The click flow (click, cycleCandidates) follows OpenEngine's SceneViewController::
// PickViaClickThrough and its Ctrl+wheel depth cycling (Apps/Editor/Source/SceneViewController.cpp)
// and the drop order SceneViewDropPlacement.cpp, Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License (NOTICE.md). Physics is never consulted for a drop (D9).

#include "SceneViewController.h"
#include "shortcuts/CommandCatalog.h"

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "object/Camera.h"
#include "picking/BvhDiskCache.h"
#include "picking/PickCache.h"
#include "picking/PickGeometry.h"
#include "picking/SceneBvh.h"
#include "render/SceneRender.h"
#include "render/SceneRender2D.h"
#include "render/SceneRender3D.h"
#include "sceneview/SceneViewVisibility.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"
#include "util/GraphicUtils.h"
#include "window/HierarchyWindow.h"
#include "window/StatusBar.h"

#include "component/InstancedMeshComponent.h"
#include "component/Transform.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    constexpr float PI = 3.14159265358979f;

    double now(){
        return ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    }

    bool sameVector(const Vector3& a, const Vector3& b){
        return std::fabs(a.x - b.x) <= 1e-6f && std::fabs(a.y - b.y) <= 1e-6f && std::fabs(a.z - b.z) <= 1e-6f;
    }

    bool samePose(const SceneCameraPose& a, const SceneCameraPose& b){
        return sameVector(a.position, b.position) && sameVector(a.target, b.target) && sameVector(a.up, b.up) &&
               a.orthographic == b.orthographic && std::fabs(a.orthoSize - b.orthoSize) <= 1e-6f &&
               std::fabs(a.fieldOfView - b.fieldOfView) <= 1e-6f;
    }

    bool paneIsAxis(const SceneViewState& state, int pane){
        return state.layout != SceneViewLayout::SINGLE && pane >= 0 && pane < SceneViewState::MAX_PANES &&
               state.panes[(size_t)pane].axis != SceneViewAxis::FREE;
    }

    float& paneOrthoSize(SceneViewState& state, int pane){
        return pane <= 0 ? state.orthoSize : state.panes[(size_t)pane].pose.orthoSize;
    }

    float paneOrthoSize(const SceneViewState& state, int pane){
        return pane <= 0 ? state.orthoSize : state.panes[(size_t)pane].pose.orthoSize;
    }

    bool paneOrthographic(const SceneViewState& state, int pane){
        if (paneIsAxis(state, pane)){
            return true;
        }
        if (pane <= 0){
            return state.orthographic || state.axisOrtho;
        }
        return state.panes[(size_t)pane].pose.orthographic;
    }

    // the grid plane of a view along axis: XZ, XY (front, back) or YZ (sides)
    int gridPlaneOf(SceneViewAxis axis){
        if (axis == SceneViewAxis::FRONT || axis == SceneViewAxis::BACK) return 1;
        if (axis == SceneViewAxis::LEFT || axis == SceneViewAxis::RIGHT) return 2;
        return 0;
    }

    // the position of a pose looking along axis at target from distance
    SceneCameraPose axisPose(SceneViewAxis axis, const Vector3& target, float distance, float orthoSize, float fov){
        SceneCameraPose pose;
        const Vector3 direction = sceneview::axisViewDirection(axis);
        pose.target = target;
        pose.position = target - direction * std::max(distance, MIN_EDITOR_CAMERA_DISTANCE);
        pose.up = sceneview::axisViewUp(axis);
        pose.orthographic = true;
        pose.orthoSize = orthoSize;
        pose.fieldOfView = fov;
        return pose;
    }

    // the selection the clicks of a view decide against: the scene whose GameObjects the
    // Inspector shows when that is the view or a layer of it
    uint32_t selectionSceneOf(Project* project, uint32_t viewSceneId){
        const uint32_t properties = project->getSelectedSceneForProperties();
        if (properties != viewSceneId && project->isSceneViewLayer(viewSceneId, properties)){
            return properties;
        }
        return viewSceneId;
    }

    // a vertex of a mesh's source, local space
    bool sourceVertex(const PickSubmeshSource& source, uint32_t vertex, Vector3& out){
        if (!source.valid || !source.positionBuffer || vertex >= source.vertexCount){
            return false;
        }
        float p[3];
        const size_t offset = source.positionOffset + (size_t)vertex * source.positionStride;
        if (offset + sizeof(p) > source.positionBuffer->getSize()){
            return false;
        }
        std::memcpy(p, source.positionBuffer->getData() + offset, sizeof(p));
        out = Vector3(p[0], p[1], p[2]);
        return true;
    }

}

// ---- commands: the catalog's Scene View group (S25)

std::string editor::sceneViewShortcut(const char* id){
    return CommandCatalog::editor().bindingsText(id);
}

// ---- lifecycle

SceneViewController::SceneViewController(Project* project): project(project){
}

SceneViewController::~SceneViewController(){
    forgetAll();
}

SceneViewController::Runtime& SceneViewController::runtime(uint32_t sceneId){
    return runtimes[sceneId];
}

const SceneViewController::Runtime* SceneViewController::findRuntime(uint32_t sceneId) const{
    auto it = runtimes.find(sceneId);
    return it == runtimes.end() ? nullptr : &it->second;
}

void SceneViewController::destroyPanes(Runtime& rt){
    for (size_t pane = 0; pane < rt.panes.size(); pane++){
        Pane& p = rt.panes[pane];
        delete p.camera;
        p.camera = nullptr;
        p.framebuffer.destroy();
        p.width = 0;
        p.height = 0;
        p.dirty = true;
    }
}

void SceneViewController::forgetScene(uint32_t sceneId){
    auto it = runtimes.find(sceneId);
    if (it == runtimes.end()){
        return;
    }
    destroyPanes(it->second);
    runtimes.erase(it);
    if (cameraPopupScene == sceneId){
        cameraPopupScene = NULL_PROJECT_SCENE;
    }
}

void SceneViewController::forgetAll(){
    for (auto& [sceneId, rt] : runtimes){
        destroyPanes(rt);
    }
    runtimes.clear();
    cameraPopupScene = NULL_PROJECT_SCENE;
}

void SceneViewController::saveWorkspace(){
    project->saveWorkspaceFile();
}

std::string SceneViewController::sceneKey(const SceneProject& sceneProject) const{
    std::string key = sceneProject.filepath.empty() ? ("scene-" + std::to_string(sceneProject.id))
                                                    : sceneProject.filepath.lexically_normal().generic_string();
    std::string safe;
    for (char c : key){
        safe += (std::isalnum((unsigned char)c) || c == '-' || c == '.') ? c : '_';
    }
    // distinct files with the same safe name stay apart
    const uint64_t hash = picking::hashBytes(key.data(), key.size(), 0x5343454e45ull);
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "-%08x", (unsigned)(hash & 0xffffffffu));
    return safe + suffix;
}

void SceneViewController::applyViewState(SceneProject& sceneProject){
    sceneview::applyToScene(sceneProject);
    SceneViewState& state = sceneProject.viewState;
    state.camera.clamp();
    state.setLayout(state.layout);
    if (sceneProject.sceneType == SceneType::SCENE_3D){
        applyProjection(sceneProject, runtime(sceneProject.id), true);
    }
    sceneProject.needUpdateRender = true;
}

namespace {

    // R10: the project's mesh BVH disk cache. A build job may still use one of a project left
    // behind, so they stay alive (one per project opened in the session).
    void ensureDiskCache(Project* project){
        static std::vector<std::unique_ptr<BvhDiskCache>> caches;
        static fs::path installed;
        const fs::path path = project->getProjectPath();
        if (path.empty() || path == installed){
            return;
        }
        installed = path;
        caches.push_back(std::make_unique<BvhDiskCache>(BvhDiskCache::directoryFor(path)));
        PickCache::get().setDiskCache(caches.back().get());
    }

}

void SceneViewController::ensureApplied(SceneProject& sceneProject){
    if (!sceneProject.scene || !sceneProject.sceneRender){
        return;
    }
    ensureDiskCache(project);
    Runtime& rt = runtime(sceneProject.id);
    if (!rt.applied || rt.appliedRender != sceneProject.sceneRender){
        if (rt.appliedRender && rt.appliedRender != sceneProject.sceneRender){
            destroyPanes(rt);
        }
        rt.appliedRender = sceneProject.sceneRender;
        rt.applied = true;
        applyViewState(sceneProject);
        // R9: in the editor every mesh is queued for its BVH when it loads
        sceneProject.scene->getPickBvh().setPrepareOnLoad(true);
    }
}

void SceneViewController::beginFrame(SceneProject& sceneProject){
    if (!sceneProject.scene || !sceneProject.sceneRender){
        return;
    }
    ensureApplied(sceneProject);
    Runtime& rt = runtime(sceneProject.id);
    if (rt.requestedPane >= 0){
        const int pane = rt.requestedPane;
        rt.requestedPane = -1;
        setActivePane(sceneProject, pane);
    }
    if (sceneProject.sceneType == SceneType::SCENE_3D){
        updateTween(sceneProject, rt);
        updateAxisOrtho(sceneProject);
        applyProjection(sceneProject, rt, false);
        // the grid lies in the plane an orthographic axis view looks at
        const SceneViewAxis axis = isOrthographic(sceneProject) ? viewAxis(sceneProject) : SceneViewAxis::FREE;
        sceneProject.sceneRender->setGridPlane(gridPlaneOf(axis));
        SceneViewState& state = sceneProject.viewState;
        if (state.layout != SceneViewLayout::SINGLE){
            SceneViewPane& active = state.panes[(size_t)state.activePane];
            const SceneCameraPose pose = currentPose(sceneProject);
            active.pose.position = pose.position;
            active.pose.target = pose.target;
            active.pose.up = pose.up;
            active.posed = true;
        }
    }else{
        updateTween(sceneProject, rt);
    }
    sceneProject.scene->getPickBvh().processPrepareQueue();
    processPendingClick(sceneProject, rt);
}

// ---- picking

ScenePickRequest SceneViewController::pickRequest(uint32_t sceneId, float x, float y, bool exactPick, bool waitForBvh) const{
    ScenePickRequest request;
    request.x = x;
    request.y = y;
    request.exactPick = exactPick;
    request.waitForBvh = waitForBvh;
    const SceneProject* sceneProject = project->getScene(sceneId);
    // the icon radius in engine pixels: the overlays' scale (UILayer) times the logical radius
    float scale = 1.0f;
    if (sceneProject && sceneProject->sceneRender){
        scale = std::max(0.25f, sceneProject->sceneRender->getOverlayScale());
    }
    request.iconRadius = 10.0f * scale;
    if (collapsedRow){
        auto rows = collapsedRow;
        request.collapsedRow = [rows, sceneId](uint32_t entitySceneId, Entity entity){ return rows(sceneId, entitySceneId, entity); };
    }
    return request;
}

ScenePickResult SceneViewController::pickAt(uint32_t sceneId, float x, float y, bool exactPick, bool waitForBvh){
    ScenePicker picker(project);
    return picker.pick(sceneId, pickRequest(sceneId, x, y, exactPick, waitForBvh));
}

ScenePickContext SceneViewController::clickContext(uint32_t sceneId, const SceneClickInput& input, const Runtime& rt) const{
    ScenePickContext context;
    context.shift = input.shift;
    context.ctrl = input.ctrl;
    context.exactPick = input.exactPick.value_or(AppSettings::getSceneViewSettings().exactPick);
    const uint32_t selectionScene = selectionSceneOf(project, sceneId);
    const SelectionState selection = project->getSelection(selectionScene);
    if (selection.active != NULL_ENTITY){
        context.primary.sceneId = selectionScene;
        context.primary.entity = selection.active;
    }else if (!selection.entities.empty()){
        context.primary.sceneId = selectionScene;
        context.primary.entity = selection.entities.back();
    }
    Project* p = project;
    context.isSelected = [p, selectionScene](const ScenePickEntry& entry){
        return entry.sceneId == selectionScene && p->isSelectedEntity(entry.sceneId, entry.entity);
    };
    context.isInside = [p](uint32_t entitySceneId, Entity entity, Entity ancestor){
        return ScenePicker(p).isInside(entitySceneId, entity, ancestor);
    };
    context.lastX = rt.lastClickX;
    context.lastY = rt.lastClickY;
    context.depthAnchor = rt.depthAnchor;
    context.x = input.x;
    context.y = input.y;
    return context;
}

void SceneViewController::applyClick(uint32_t sceneId, const ScenePickClick& click, const ScenePickResult& result){
    (void)result;
    const ScenePickEntry& target = click.target;
    switch (click.action){
        case ScenePickClick::Action::NONE:
            return;
        case ScenePickClick::Action::CLEAR:
            project->clearAllSelections(sceneId);
            project->setSelectedSceneForProperties(sceneId);
            project->clearInspectedAsset();
            project->syncSceneRenderEditLayer(sceneId);
            return;
        case ScenePickClick::Action::SELECT:
            project->clearAllSelections(sceneId);
            project->addSelectedEntity(target.sceneId, target.entity);
            break;
        case ScenePickClick::Action::ADD:
            // a selection is of one scene: Shift adds to it only within that scene
            if (project->getSelectedSceneForProperties() != target.sceneId){
                project->clearAllSelections(sceneId);
            }
            project->addSelectedEntity(target.sceneId, target.entity);
            break;
        case ScenePickClick::Action::TOGGLE:
            if (project->getSelectedSceneForProperties() == target.sceneId && project->isSelectedEntity(target.sceneId, target.entity)){
                SelectionState state = project->getSelection(target.sceneId);
                state.entities.erase(std::remove(state.entities.begin(), state.entities.end(), target.entity), state.entities.end());
                state.active = state.entities.empty() ? NULL_ENTITY : state.entities.back();
                project->setSelection(target.sceneId, state);
                project->syncSceneRenderEditLayer(sceneId);
                return;
            }
            if (project->getSelectedSceneForProperties() != target.sceneId){
                project->clearAllSelections(sceneId);
            }
            project->addSelectedEntity(target.sceneId, target.entity);
            break;
    }
    project->setSelectedSceneForProperties(target.sceneId);
    // a user selection makes the GameObject the inspected object (animation clips 9.R30)
    project->clearInspectedAsset();
    if (HierarchyWindow* structure = Backend::getApp().getHierarchyWindow()){
        structure->revealEntity(target.sceneId, target.entity);
    }
    project->syncSceneRenderEditLayer(sceneId);
}

void SceneViewController::showPill(Runtime& rt, const ScenePickClick& click, const ScenePickResult& result, float x, float y){
    (void)result;
    if (click.cycleCount < 2 || click.cycleIndex < 0 || click.target.entity == NULL_ENTITY){
        return;
    }
    std::string name;
    if (const SceneProject* targetScene = project->getScene(click.target.sceneId); targetScene && targetScene->scene){
        name = targetScene->scene->getEntityName(click.target.entity);
    }
    if (name.empty()){
        name = "Entity " + std::to_string(click.target.entity);
    }
    rt.pillText = name + "  " + std::to_string(click.cycleIndex + 1) + "/" + std::to_string(click.cycleCount);
    rt.pillPosition = ImVec2(x, y);
    rt.pillUntil = now() + PILL_SECONDS;
}

SceneClickOutcome SceneViewController::runClick(uint32_t sceneId, const SceneClickInput& input, Runtime& rt){
    SceneClickOutcome outcome;
    const bool exact = input.exactPick.value_or(AppSettings::getSceneViewSettings().exactPick);
    outcome.result = pickAt(sceneId, input.x, input.y, exact, input.waitForBvh);
    if (outcome.result.pending && !input.waitForBvh){
        outcome.pending = true;
        return outcome;
    }
    outcome.click = ScenePicker::decideClick(outcome.result, clickContext(sceneId, input, rt));
    if (input.apply){
        applyClick(sceneId, outcome.click, outcome.result);
        outcome.applied = outcome.click.action != ScenePickClick::Action::NONE;
        rt.lastClickX = input.x;
        rt.lastClickY = input.y;
        rt.depthAnchor = false;
        rt.wheelCandidates.clear();
        rt.wheelIndex = -1;
        showPill(rt, outcome.click, outcome.result, input.x, input.y);
    }
    return outcome;
}

SceneClickOutcome SceneViewController::click(uint32_t sceneId, const SceneClickInput& input){
    Runtime& rt = runtime(sceneId);
    // a new click replaces one still waiting
    rt.pending = PendingClick();
    SceneClickOutcome outcome = runClick(sceneId, input, rt);
    if (outcome.pending && input.apply){
        rt.pending.active = true;
        rt.pending.input = input;
        rt.pending.started = now();
        if (const SceneProject* hitScene = project->getScene(outcome.result.pendingHit.sceneId); hitScene && hitScene->scene){
            rt.pending.meshName = hitScene->scene->getEntityName(outcome.result.pendingHit.entity);
        }
        if (rt.pending.meshName.empty()){
            rt.pending.meshName = "the mesh";
        }
    }
    return outcome;
}

void SceneViewController::processPendingClick(SceneProject& sceneProject, Runtime& rt){
    if (!rt.pending.active){
        return;
    }
    SceneClickOutcome outcome = runClick(sceneProject.id, rt.pending.input, rt);
    if (!outcome.pending){
        // decided: never through a stale AABB (R33)
        rt.pending = PendingClick();
        sceneProject.needUpdateRender = true;
        return;
    }
    // said once the click waited PENDING_STATUS_SECONDS, then kept on the bar while it waits
    // (a 1M-triangle build can take longer than a transient message lasts)
    if (rt.pending.announced || now() - rt.pending.started >= PENDING_STATUS_SECONDS){
        rt.pending.announced = true;
        notifyStatus("Preparing picking for " + rt.pending.meshName + "\xE2\x80\xA6", 1.0);
    }
}

void SceneViewController::resetClickHistory(uint32_t sceneId){
    Runtime& rt = runtime(sceneId);
    rt.lastClickX = -1.0e9f;
    rt.lastClickY = -1.0e9f;
    rt.depthAnchor = false;
    rt.wheelCandidates.clear();
    rt.wheelIndex = -1;
}

bool SceneViewController::hasPendingClick(uint32_t sceneId) const{
    const Runtime* rt = findRuntime(sceneId);
    return rt && rt->pending.active;
}

std::string SceneViewController::pendingStatus(uint32_t sceneId) const{
    const Runtime* rt = findRuntime(sceneId);
    if (!rt || !rt->pending.active){
        return std::string();
    }
    return "Preparing picking for " + rt->pending.meshName + "\xE2\x80\xA6";
}

bool SceneViewController::cycleCandidates(uint32_t sceneId, float x, float y, int step){
    Runtime& rt = runtime(sceneId);
    const ScenePickResult result = pickAt(sceneId, x, y, AppSettings::getSceneViewSettings().exactPick, true);
    if (result.candidates.empty() || step == 0){
        return false;
    }
    // a new spot starts over from the candidate the selection is
    const bool sameList = rt.wheelCandidates == result.candidates;
    if (!sameList){
        rt.wheelCandidates = result.candidates;
        rt.wheelIndex = -1;
        const uint32_t selectionScene = selectionSceneOf(project, sceneId);
        const SelectionState selection = project->getSelection(selectionScene);
        for (size_t i = 0; i < result.candidates.size(); i++){
            if (result.candidates[i].sceneId == selectionScene && result.candidates[i].entity == selection.active){
                rt.wheelIndex = (int)i;
            }
        }
    }
    const int count = (int)result.candidates.size();
    rt.wheelIndex = rt.wheelIndex < 0 ? (step > 0 ? 0 : count - 1) : ((rt.wheelIndex + step) % count + count) % count;
    ScenePickClick click;
    click.action = ScenePickClick::Action::SELECT;
    click.target = result.candidates[(size_t)rt.wheelIndex];
    click.cycleIndex = rt.wheelIndex;
    click.cycleCount = count;
    applyClick(sceneId, click, result);
    // the next plain click at this spot confirms it
    rt.depthAnchor = true;
    rt.lastClickX = x;
    rt.lastClickY = y;
    showPill(rt, click, result, x, y);
    return true;
}

// ---- camera

SceneCameraPose SceneViewController::currentPose(const SceneProject& sceneProject) const{
    SceneCameraPose pose;
    if (!sceneProject.sceneRender){
        return pose;
    }
    Camera* camera = sceneProject.sceneRender->getCamera();
    pose.position = camera->getPosition();
    pose.target = camera->getTarget();
    pose.up = camera->getUp();
    if (sceneProject.sceneType == SceneType::SCENE_3D){
        pose.orthographic = isOrthographic(sceneProject);
        pose.orthoSize = paneOrthoSize(sceneProject.viewState, sceneProject.viewState.activePane);
        pose.fieldOfView = sceneProject.viewState.camera.fieldOfView;
    }else{
        pose.orthographic = true;
        pose.orthoSize = static_cast<SceneRender2D*>(sceneProject.sceneRender)->getZoom();
        pose.fieldOfView = 0.0f;
    }
    return pose;
}

bool SceneViewController::isOrthographic(const SceneProject& sceneProject) const{
    if (sceneProject.sceneType != SceneType::SCENE_3D){
        return true;
    }
    return paneOrthographic(sceneProject.viewState, sceneProject.viewState.activePane);
}

bool SceneViewController::isAxisLocked(const SceneProject& sceneProject) const{
    return sceneProject.sceneType == SceneType::SCENE_3D && paneIsAxis(sceneProject.viewState, sceneProject.viewState.activePane);
}

SceneViewAxis SceneViewController::viewAxis(const SceneProject& sceneProject) const{
    if (!sceneProject.sceneRender){
        return SceneViewAxis::FREE;
    }
    Camera* camera = sceneProject.sceneRender->getCamera();
    return sceneview::alignedAxis(camera->getTarget() - camera->getPosition());
}

float SceneViewController::navigationDistance(const SceneProject& sceneProject) const{
    if (!sceneProject.sceneRender){
        return 10.0f;
    }
    if (sceneProject.sceneType == SceneType::SCENE_3D && isOrthographic(sceneProject)){
        return sceneview::distanceForOrthoSize(paneOrthoSize(sceneProject.viewState, sceneProject.viewState.activePane),
                                               sceneProject.viewState.camera.fieldOfView);
    }
    return sceneProject.sceneRender->getCamera()->getDistanceFromTarget();
}

void SceneViewController::projectCamera(SceneProject& sceneProject, int width, int height, bool force){
    if (sceneProject.sceneType != SceneType::SCENE_3D || !sceneProject.sceneRender || !sceneProject.scene){
        return;
    }
    Camera* camera = sceneProject.sceneRender->getCamera();
    CameraComponent* component = sceneProject.scene->findComponent<CameraComponent>(camera->getEntity());
    if (!component){
        return;
    }
    if (width <= 0 || height <= 0){
        width = 16;
        height = 9;
    }
    const SceneCameraSettings& settings = sceneProject.viewState.camera;
    const float aspect = (float)width / (float)height;
    const float distance = camera->getDistanceFromTarget();
    const bool ortho = paneOrthographic(sceneProject.viewState, sceneProject.viewState.activePane);
    if (ortho){
        float& size = paneOrthoSize(sceneProject.viewState, sceneProject.viewState.activePane);
        size = std::clamp(size, MIN_ORTHO_SIZE, MAX_ORTHO_SIZE);
        float nearClip = 0.0f, farClip = 0.0f;
        sceneview::orthoClips(distance, settings.farClip, nearClip, farClip);
        const float halfWidth = size * aspect;
        const bool changed = force || component->type != CameraType::CAMERA_ORTHO || component->autoResize ||
                             component->leftClip != -halfWidth || component->rightClip != halfWidth ||
                             component->bottomClip != -size || component->topClip != size ||
                             component->nearClip != nearClip || component->farClip != farClip;
        if (changed){
            camera->setOrtho(-halfWidth, halfWidth, -size, size, nearClip, farClip);
            sceneProject.needUpdateRender = true;
        }
    }else{
        float nearClip = settings.nearClip;
        float farClip = settings.farClip;
        if (settings.dynamicClipping){
            sceneview::dynamicClips(distance, nearClip, farClip);
        }
        const float yfov = settings.fieldOfView * PI / 180.0f;
        const bool changed = force || component->type != CameraType::CAMERA_PERSPECTIVE || !component->autoResize ||
                             std::fabs(component->yfov - yfov) > 1e-6f || component->nearClip != nearClip ||
                             component->farClip != farClip;
        if (changed){
            camera->setPerspective(Angle::radToDefault(yfov), aspect, nearClip, farClip);
            camera->setAutoResize(true);
            sceneProject.scene->getSystem<RenderSystem>()->updateCameraSizeForCanvas(camera->getEntity(), (float)width, (float)height);
            sceneProject.needUpdateRender = true;
        }
    }
}

void SceneViewController::applyProjection(SceneProject& sceneProject, Runtime& rt, bool force){
    if (sceneProject.sceneType != SceneType::SCENE_3D || !sceneProject.sceneRender || !sceneProject.scene){
        return;
    }
    Camera* camera = sceneProject.sceneRender->getCamera();
    CameraComponent* component = sceneProject.scene->findComponent<CameraComponent>(camera->getEntity());
    if (!component){
        return;
    }
    int width = sceneProject.sceneRender->getViewWidth();
    int height = sceneProject.sceneRender->getViewHeight();
    if (viewSize){
        int w = 0, h = 0;
        viewSize(sceneProject.id, w, h);
        if (w > 0 && h > 0){
            width = w;
            height = h;
        }
    }
    if (width <= 0 || height <= 0){
        width = 16;
        height = 9;
    }
    projectCamera(sceneProject, width, height, force);
    rt.projectedOrtho = isOrthographic(sceneProject);
    rt.projectedWidth = width;
    rt.projectedHeight = height;
}

void SceneViewController::applyPoseNow(SceneProject& sceneProject, const SceneCameraPose& pose){
    if (!sceneProject.sceneRender){
        return;
    }
    Camera* camera = sceneProject.sceneRender->getCamera();
    camera->setPosition(pose.position);
    camera->setUp(pose.up);
    camera->setTarget(pose.target);
    if (sceneProject.sceneType == SceneType::SCENE_3D){
        paneOrthoSize(sceneProject.viewState, sceneProject.viewState.activePane) = std::clamp(pose.orthoSize, MIN_ORTHO_SIZE, MAX_ORTHO_SIZE);
        applyProjection(sceneProject, runtime(sceneProject.id), false);
    }else if (pose.orthoSize > 0.0f){
        static_cast<SceneRender2D*>(sceneProject.sceneRender)->setZoom(std::clamp(pose.orthoSize, 0.01f, 1000.0f));
    }
    sceneProject.needUpdateRender = true;
}

void SceneViewController::moveTo(SceneProject& sceneProject, const SceneCameraPose& pose, bool animate){
    Runtime& rt = runtime(sceneProject.id);
    SceneViewState& state = sceneProject.viewState;
    if (sceneProject.sceneType == SceneType::SCENE_3D){
        // the projection switches at once (R18); the pose eases
        const int pane = state.activePane;
        if (!paneIsAxis(state, pane)){
            if (pane <= 0){
                if (pose.orthographic != (state.orthographic || state.axisOrtho)){
                    state.orthographic = pose.orthographic;
                    state.axisOrtho = false;
                }
            }else{
                state.panes[(size_t)pane].pose.orthographic = pose.orthographic;
            }
        }
        if (pose.fieldOfView >= SceneCameraSettings::MIN_FOV && std::fabs(pose.fieldOfView - state.camera.fieldOfView) > 1e-4f){
            state.camera.fieldOfView = std::clamp(pose.fieldOfView, SceneCameraSettings::MIN_FOV, SceneCameraSettings::MAX_FOV);
        }
        applyProjection(sceneProject, rt, false);
    }
    if (!animate || !state.camera.easing){
        rt.tween.active = false;
        applyPoseNow(sceneProject, pose);
        markPanesDirty(rt);
        return;
    }
    rt.tween.active = true;
    rt.tween.from = currentPose(sceneProject);
    rt.tween.to = pose;
    rt.tween.start = now();
    sceneProject.needUpdateRender = true;
}

void SceneViewController::finishTween(SceneProject& sceneProject){
    Runtime& rt = runtime(sceneProject.id);
    if (rt.tween.active){
        rt.tween.active = false;
        applyPoseNow(sceneProject, rt.tween.to);
        markPanesDirty(rt);
    }
}

void SceneViewController::updateTween(SceneProject& sceneProject, Runtime& rt){
    if (!rt.tween.active){
        return;
    }
    const float t = (float)((now() - rt.tween.start) / SceneCameraSettings::EASING_SECONDS);
    if (t >= 1.0f){
        rt.tween.active = false;
        applyPoseNow(sceneProject, rt.tween.to);
    }else{
        applyPoseNow(sceneProject, sceneview::interpolatePose(rt.tween.from, rt.tween.to, sceneview::easeOutCubic(t)));
    }
    sceneProject.needUpdateRender = true;
}

void SceneViewController::lookFrom(SceneProject& sceneProject, const Vector3& position, const Vector3& target, const Vector3& up){
    SceneCameraPose pose = currentPose(sceneProject);
    pose.position = position;
    pose.target = target;
    pose.up = up;
    if (sceneProject.sceneType == SceneType::SCENE_3D && pose.orthographic){
        // the same framing in orthographic: the half height of the perspective view at that distance
        pose.orthoSize = sceneview::orthoSizeForDistance((position - target).length(), sceneProject.viewState.camera.fieldOfView);
    }
    moveTo(sceneProject, pose, true);
}

void SceneViewController::setProjection(SceneProject& sceneProject, bool orthographic){
    if (sceneProject.sceneType != SceneType::SCENE_3D || !sceneProject.sceneRender){
        return;
    }
    SceneViewState& state = sceneProject.viewState;
    const int pane = state.activePane;
    if (paneIsAxis(state, pane) || isOrthographic(sceneProject) == orthographic){
        if (pane <= 0 && !paneIsAxis(state, pane)){
            state.orthographic = orthographic;
            state.axisOrtho = false;
        }
        return;
    }
    Camera* camera = sceneProject.sceneRender->getCamera();
    const float fov = state.camera.fieldOfView;
    if (orthographic){
        // the framing is kept: the half height the perspective view shows at its target
        paneOrthoSize(state, pane) = std::clamp(sceneview::orthoSizeForDistance(camera->getDistanceFromTarget(), fov), MIN_ORTHO_SIZE, MAX_ORTHO_SIZE);
    }else{
        // back in perspective at the distance whose framing the orthographic size had
        const float distance = std::clamp(sceneview::distanceForOrthoSize(paneOrthoSize(state, pane), fov), MIN_EDITOR_CAMERA_DISTANCE,
                                          MAX_EDITOR_CAMERA_DISTANCE);
        const Vector3 target = camera->getTarget();
        Vector3 back = camera->getPosition() - target;
        if (back.squaredLength() < 1e-12f) back = Vector3(0, 0, 1);
        back.normalize();
        camera->setPosition(target + back * distance);
        camera->setTarget(target);
    }
    if (pane <= 0){
        state.orthographic = orthographic;
        state.axisOrtho = false;
    }else{
        state.panes[(size_t)pane].pose.orthographic = orthographic;
    }
    Runtime& rt = runtime(sceneProject.id);
    rt.tween.active = false;
    applyProjection(sceneProject, rt, true);
    markPanesDirty(rt);
    saveWorkspace();
}

void SceneViewController::toggleProjection(SceneProject& sceneProject){
    setProjection(sceneProject, !isOrthographic(sceneProject));
}

void SceneViewController::updateAxisOrtho(SceneProject& sceneProject){
    SceneViewState& state = sceneProject.viewState;
    if (state.activePane != 0 || !state.axisOrtho || paneIsAxis(state, 0)){
        return;
    }
    // rotated out of the axis view: back to the user's projection
    if (viewAxis(sceneProject) == SceneViewAxis::FREE && !runtime(sceneProject.id).tween.active){
        state.axisOrtho = false;
        if (!state.orthographic){
            Camera* camera = sceneProject.sceneRender->getCamera();
            const float distance = std::clamp(sceneview::distanceForOrthoSize(state.orthoSize, state.camera.fieldOfView), MIN_EDITOR_CAMERA_DISTANCE,
                                              MAX_EDITOR_CAMERA_DISTANCE);
            const Vector3 target = camera->getTarget();
            Vector3 back = camera->getPosition() - target;
            if (back.squaredLength() > 1e-12f){
                back.normalize();
                camera->setPosition(target + back * distance);
                camera->setTarget(target);
            }
        }
        sceneProject.needUpdateRender = true;
    }
}

void SceneViewController::setAxisView(SceneProject& sceneProject, SceneViewAxis axis){
    if (sceneProject.sceneType != SceneType::SCENE_3D || !sceneProject.sceneRender || axis == SceneViewAxis::FREE){
        return;
    }
    SceneViewState& state = sceneProject.viewState;
    const int pane = state.activePane;
    if (paneIsAxis(state, pane)){
        // an axis pane's gizmo and Numpad change its axis
        state.panes[(size_t)pane].axis = axis;
    }
    Camera* camera = sceneProject.sceneRender->getCamera();
    const Vector3 target = camera->getTarget();
    const float distance = camera->getDistanceFromTarget();
    const bool wasOrtho = isOrthographic(sceneProject);
    const float size = wasOrtho ? paneOrthoSize(state, pane) : sceneview::orthoSizeForDistance(distance, state.camera.fieldOfView);
    SceneCameraPose pose = axisPose(axis, target, distance, size, state.camera.fieldOfView);
    // Unity: an axis view is orthographic while it stays aligned (the user's choice is kept)
    if (pane <= 0 && !paneIsAxis(state, pane) && !state.orthographic){
        state.axisOrtho = true;
        paneOrthoSize(state, pane) = size;
    }
    pose.orthographic = true;
    Runtime& rt = runtime(sceneProject.id);
    applyProjection(sceneProject, rt, false);
    moveTo(sceneProject, pose, true);
    saveWorkspace();
}

bool SceneViewController::frameAll(SceneProject& sceneProject){
    if (!sceneProject.scene || !sceneProject.sceneRender){
        return false;
    }
    PickFilter filter;
    filter.kinds = PICK_ALL & ~PICK_ICON & ~PICK_TERRAIN;
    filter.layerMask = sceneProject.viewState.layerFilter.visible;
    filter.excludeEditorFlags = EDITOR_HIDDEN;
    auto drawn = std::make_shared<std::vector<Entity>>(project->getPickableEntities(&sceneProject));
    std::sort(drawn->begin(), drawn->end());
    filter.accept = [drawn](Entity entity){ return std::binary_search(drawn->begin(), drawn->end(), entity); };
    AABB bounds = sceneProject.scene->getPickBvh().getBounds(filter);
    if (bounds.isNull() || bounds.isInfinite()){
        // nothing but terrain: the terrain then
        filter.kinds = PICK_ALL & ~PICK_ICON;
        bounds = sceneProject.scene->getPickBvh().getBounds(filter);
    }
    if (bounds.isNull() || bounds.isInfinite()){
        // only icons (lights, cameras): their positions
        for (Entity entity : *drawn){
            if (const Transform* transform = sceneProject.scene->findComponent<Transform>(entity)){
                bounds.merge(transform->worldPosition);
            }
        }
    }
    if (bounds.isNull() || bounds.isInfinite()){
        return false;
    }

    Camera* camera = sceneProject.sceneRender->getCamera();
    if (sceneProject.sceneType != SceneType::SCENE_3D){
        int width = 0, height = 0;
        if (viewSize) viewSize(sceneProject.id, width, height);
        if (width <= 0 || height <= 0) return false;
        const Vector3 size = bounds.getSize();
        float zoom = std::max(size.x / (float)width, size.y / (float)height) * 1.25f;
        if (!std::isfinite(zoom) || zoom <= 0.0f){
            zoom = static_cast<SceneRender2D*>(sceneProject.sceneRender)->getZoom();
        }
        zoom = std::clamp(zoom, 0.01f, 1000.0f);
        const Vector3 center = bounds.getCenter();
        SceneCameraPose pose = currentPose(sceneProject);
        const float x = center.x - (float)width * zoom * 0.5f;
        const float y = center.y - (float)height * zoom * 0.5f;
        pose.position = Vector3(x, y, pose.position.z);
        pose.target = Vector3(x, y, pose.target.z);
        pose.orthoSize = zoom;
        moveTo(sceneProject, pose, true);
        return true;
    }

    const Vector3 center = bounds.getCenter();
    const float radius = std::max(bounds.getSize().length() * 0.5f, 0.5f);
    int width = 0, height = 0;
    if (viewSize) viewSize(sceneProject.id, width, height);
    const float aspect = (width > 0 && height > 0) ? (float)width / (float)height : std::max(camera->getAspect(), 0.0001f);
    const float halfY = sceneProject.viewState.camera.fieldOfView * PI / 360.0f;
    const float halfX = std::atan(std::tan(halfY) * aspect);
    const float padded = radius * 1.1f;
    float distance = padded / std::sin(std::min(halfY, halfX));
    distance = std::clamp(std::max(distance, padded * 2.0f), MIN_EDITOR_CAMERA_DISTANCE, MAX_EDITOR_CAMERA_DISTANCE);
    Vector3 direction = camera->getTarget() - camera->getPosition();
    if (direction.squaredLength() < 1e-12f) direction = Vector3(0, 0, -1);
    direction.normalize();
    SceneCameraPose pose = currentPose(sceneProject);
    pose.target = center;
    pose.position = center - direction * distance;
    if (pose.orthographic){
        // the whole box inside the orthographic view
        pose.orthoSize = std::max(padded, padded / aspect);
    }
    moveTo(sceneProject, pose, true);
    return true;
}

void SceneViewController::setOrthoSize(SceneProject& sceneProject, float orthoSize){
    if (sceneProject.sceneType != SceneType::SCENE_3D){
        if (sceneProject.sceneRender){
            static_cast<SceneRender2D*>(sceneProject.sceneRender)->setZoom(std::clamp(orthoSize, 0.01f, 1000.0f));
            sceneProject.needUpdateRender = true;
        }
        return;
    }
    paneOrthoSize(sceneProject.viewState, sceneProject.viewState.activePane) = std::clamp(orthoSize, MIN_ORTHO_SIZE, MAX_ORTHO_SIZE);
    applyProjection(sceneProject, runtime(sceneProject.id), false);
    sceneProject.needUpdateRender = true;
}

void SceneViewController::orthoZoom(SceneProject& sceneProject, float wheel){
    if (wheel == 0.0f){
        return;
    }
    const float size = paneOrthoSize(sceneProject.viewState, sceneProject.viewState.activePane);
    setOrthoSize(sceneProject, size * std::pow(0.9f, wheel));
}

void SceneViewController::applyCameraSettings(SceneProject& sceneProject, const SceneCameraSettings& settings){
    SceneCameraSettings clamped = settings;
    clamped.clamp();
    sceneProject.viewState.camera = clamped;
    applyProjection(sceneProject, runtime(sceneProject.id), true);
    markPanesDirty(runtime(sceneProject.id));
    sceneProject.needUpdateRender = true;
}

float SceneViewController::flySpeed(SceneProject& sceneProject, bool flying, bool shift, float dt){
    (void)dt;
    Runtime& rt = runtime(sceneProject.id);
    const SceneCameraSettings& settings = sceneProject.viewState.camera;
    if (!flying){
        rt.flyStart = -1.0;
        return settings.speed * (shift ? settings.shiftMultiplier : 1.0f);
    }
    if (rt.flyStart < 0.0){
        rt.flyStart = now();
    }
    const float factor = sceneview::flyAccelerationFactor((float)(now() - rt.flyStart), settings.acceleration);
    return settings.speed * factor * (shift ? settings.shiftMultiplier : 1.0f);
}

void SceneViewController::stepFlySpeed(SceneProject& sceneProject, float wheel){
    SceneCameraSettings& settings = sceneProject.viewState.camera;
    settings.speed = sceneview::stepFlySpeed(settings.speed, wheel, settings);
}

// ---- layouts and panes

void SceneViewController::markPanesDirty(Runtime& rt){
    for (Pane& pane : rt.panes){
        pane.dirty = true;
    }
}

void SceneViewController::ensurePaneCamera(SceneProject& sceneProject, int pane){
    Runtime& rt = runtime(sceneProject.id);
    Pane& p = rt.panes[(size_t)pane];
    if (p.camera || !sceneProject.scene){
        return;
    }
    ScopedDefaultEntityPool system(*sceneProject.scene, EntityPool::System);
    p.camera = new Camera(sceneProject.scene);
    p.camera->setType(CameraType::CAMERA_PERSPECTIVE);
    if (CameraComponent* component = sceneProject.scene->findComponent<CameraComponent>(p.camera->getEntity())){
        // what the Scene view hides, every pane hides
        component->editorView = true;
    }
}

void SceneViewController::writePaneCamera(SceneProject& sceneProject, int pane){
    Runtime& rt = runtime(sceneProject.id);
    ensurePaneCamera(sceneProject, pane);
    Pane& p = rt.panes[(size_t)pane];
    if (!p.camera){
        return;
    }
    const SceneViewState& state = sceneProject.viewState;
    const SceneViewPane& paneState = state.panes[(size_t)pane];
    const SceneCameraPose& pose = paneState.pose;
    p.camera->setPosition(pose.position);
    p.camera->setUp(pose.up);
    p.camera->setTarget(pose.target);
    p.camera->setCullingMask(state.layerFilter.visible);
    const int width = std::max(p.width, 1);
    const int height = std::max(p.height, 1);
    const float aspect = (float)width / (float)height;
    const float distance = (pose.position - pose.target).length();
    if (paneOrthographic(state, pane)){
        const float size = std::clamp(paneOrthoSize(state, pane), MIN_ORTHO_SIZE, MAX_ORTHO_SIZE);
        float nearClip = 0.0f, farClip = 0.0f;
        sceneview::orthoClips(distance, state.camera.farClip, nearClip, farClip);
        p.camera->setOrtho(-size * aspect, size * aspect, -size, size, nearClip, farClip);
    }else{
        float nearClip = state.camera.nearClip;
        float farClip = state.camera.farClip;
        if (state.camera.dynamicClipping){
            sceneview::dynamicClips(distance, nearClip, farClip);
        }
        p.camera->setPerspective(Angle::degToDefault(state.camera.fieldOfView), aspect, nearClip, farClip);
    }
}

void SceneViewController::setLayout(SceneProject& sceneProject, SceneViewLayout layout){
    if (sceneProject.sceneType != SceneType::SCENE_3D){
        layout = SceneViewLayout::SINGLE;
    }
    SceneViewState& state = sceneProject.viewState;
    if (state.layout == layout){
        return;
    }
    Runtime& rt = runtime(sceneProject.id);
    // the editor camera goes back to the free pane before the layout changes
    if (state.activePane != 0){
        setActivePane(sceneProject, 0);
    }
    const SceneCameraPose mainPose = currentPose(sceneProject);
    state.setLayout(layout);
    state.maximizedPane = -1;
    for (int pane = 1; pane < state.paneCount(); pane++){
        SceneViewPane& paneState = state.panes[(size_t)pane];
        if (!paneState.posed){
            const float distance = (mainPose.position - mainPose.target).length();
            const float size = mainPose.orthographic ? mainPose.orthoSize : sceneview::orthoSizeForDistance(distance, state.camera.fieldOfView);
            if (paneState.axis == SceneViewAxis::FREE){
                paneState.pose = mainPose;
            }else{
                paneState.pose = axisPose(paneState.axis, mainPose.target, distance, size, state.camera.fieldOfView);
            }
            paneState.posed = true;
        }
    }
    if (layout == SceneViewLayout::SINGLE){
        destroyPanes(rt);
    }
    markPanesDirty(rt);
    sceneProject.needUpdateRender = true;
    saveWorkspace();
}

void SceneViewController::setPaneAxis(SceneProject& sceneProject, int pane, SceneViewAxis axis){
    SceneViewState& state = sceneProject.viewState;
    if (pane < 0 || pane >= state.paneCount() || state.layout == SceneViewLayout::SINGLE){
        return;
    }
    SceneViewPane& paneState = state.panes[(size_t)pane];
    if (pane == state.activePane){
        if (axis == SceneViewAxis::FREE){
            paneState.axis = SceneViewAxis::FREE;
            if (pane > 0) paneState.pose.orthographic = false;
            applyProjection(sceneProject, runtime(sceneProject.id), true);
        }else{
            paneState.axis = axis;
            setAxisView(sceneProject, axis);
        }
        saveWorkspace();
        return;
    }
    const SceneCameraPose from = paneState.pose;
    paneState.axis = axis;
    if (axis == SceneViewAxis::FREE){
        paneState.pose.orthographic = false;
    }else{
        const float distance = (from.position - from.target).length();
        paneState.pose = axisPose(axis, from.target, distance, from.orthoSize, state.camera.fieldOfView);
    }
    paneState.posed = true;
    runtime(sceneProject.id).panes[(size_t)pane].dirty = true;
    sceneProject.needUpdateRender = true;
    saveWorkspace();
}

void SceneViewController::setPaneShading(SceneProject& sceneProject, int pane, PaneShading shading){
    SceneViewState& state = sceneProject.viewState;
    if (pane < 0 || pane >= SceneViewState::MAX_PANES){
        return;
    }
    state.panes[(size_t)pane].shading = shading;
    runtime(sceneProject.id).panes[(size_t)pane].dirty = true;
    sceneProject.needUpdateRender = true;
    saveWorkspace();
}

void SceneViewController::toggleMaximized(SceneProject& sceneProject, int pane){
    SceneViewState& state = sceneProject.viewState;
    if (state.layout == SceneViewLayout::SINGLE){
        return;
    }
    state.maximizedPane = state.maximizedPane == pane ? -1 : pane;
    if (state.maximizedPane >= 0 && state.activePane != pane){
        setActivePane(sceneProject, pane);
    }
    markPanesDirty(runtime(sceneProject.id));
    sceneProject.needUpdateRender = true;
    saveWorkspace();
}

void SceneViewController::requestActivePane(SceneProject& sceneProject, int pane){
    Runtime& rt = runtime(sceneProject.id);
    if (pane != sceneProject.viewState.activePane){
        rt.requestedPane = pane;
        sceneProject.needUpdateRender = true;
    }
}

void SceneViewController::setActivePane(SceneProject& sceneProject, int pane){
    SceneViewState& state = sceneProject.viewState;
    if (pane < 0 || pane >= state.paneCount() || pane == state.activePane || !sceneProject.sceneRender){
        return;
    }
    Runtime& rt = runtime(sceneProject.id);
    rt.tween.active = false;
    // the editor camera's pose stays with the pane it was, the new pane's comes onto it
    const int previous = state.activePane;
    const SceneCameraPose leaving = currentPose(sceneProject);
    SceneViewPane& left = state.panes[(size_t)previous];
    left.pose.position = leaving.position;
    left.pose.target = leaving.target;
    left.pose.up = leaving.up;
    left.posed = true;
    if (previous > 0 && !paneIsAxis(state, previous)){
        left.pose.orthographic = leaving.orthographic;
    }
    state.activePane = pane;
    const SceneViewPane& entered = state.panes[(size_t)pane];
    Camera* camera = sceneProject.sceneRender->getCamera();
    camera->setPosition(entered.pose.position);
    camera->setUp(entered.pose.up);
    camera->setTarget(entered.pose.target);
    // the pane it left draws through its own camera from now on
    rt.panes[(size_t)previous].dirty = true;
    applyProjection(sceneProject, rt, true);
    sceneProject.needUpdateRender = true;
}

int SceneViewController::paneOfSlot(const SceneProject& sceneProject, int slot) const{
    const SceneViewState& state = sceneProject.viewState;
    if (state.layout == SceneViewLayout::SINGLE || sceneProject.sceneType != SceneType::SCENE_3D){
        return 0;
    }
    if (state.maximizedPane >= 0){
        return state.maximizedPane;
    }
    if (state.layout == SceneViewLayout::SPLIT){
        return slot;
    }
    // Quad: Top | Front over Side | Persp
    static const int quad[4] = {1, 2, 3, 0};
    return quad[std::clamp(slot, 0, 3)];
}

int SceneViewController::paneSlotCount(const SceneProject& sceneProject) const{
    const SceneViewState& state = sceneProject.viewState;
    if (state.layout == SceneViewLayout::SINGLE || sceneProject.sceneType != SceneType::SCENE_3D || state.maximizedPane >= 0){
        return 1;
    }
    return state.paneCount();
}

bool SceneViewController::paneViewSize(const SceneProject& sceneProject, int pane, int& width, int& height) const{
    width = height = 0;
    if (pane == sceneProject.viewState.activePane || sceneProject.viewState.layout == SceneViewLayout::SINGLE){
        if (viewSize){
            viewSize(sceneProject.id, width, height);
        }
        return width > 0 && height > 0;
    }
    const Runtime* rt = findRuntime(sceneProject.id);
    if (!rt || pane < 0 || pane >= SceneViewState::MAX_PANES){
        return false;
    }
    width = rt->panes[(size_t)pane].width;
    height = rt->panes[(size_t)pane].height;
    return width > 0 && height > 0;
}

uint64_t SceneViewController::paneDrawCount(const SceneProject& sceneProject, int pane) const{
    const Runtime* rt = findRuntime(sceneProject.id);
    if (!rt || pane < 0 || pane >= SceneViewState::MAX_PANES){
        return 0;
    }
    return rt->panes[(size_t)pane].draws;
}

Framebuffer* SceneViewController::paneFramebuffer(SceneProject& sceneProject, int pane){
    if (pane < 0 || pane >= SceneViewState::MAX_PANES || pane == sceneProject.viewState.activePane){
        return nullptr;
    }
    Pane& p = runtime(sceneProject.id).panes[(size_t)pane];
    return p.framebuffer.isCreated() ? &p.framebuffer : nullptr;
}

SceneCameraPose SceneViewController::panePose(const SceneProject& sceneProject, int pane) const{
    if (pane == sceneProject.viewState.activePane || pane < 0 || pane >= SceneViewState::MAX_PANES){
        return currentPose(sceneProject);
    }
    SceneCameraPose pose = sceneProject.viewState.panes[(size_t)pane].pose;
    pose.orthographic = paneOrthographic(sceneProject.viewState, pane);
    pose.orthoSize = paneOrthoSize(sceneProject.viewState, pane);
    pose.fieldOfView = sceneProject.viewState.camera.fieldOfView;
    return pose;
}

bool SceneViewController::paneRay(const SceneProject& sceneProject, int pane, float x, float y, Ray& ray) const{
    if (!sceneProject.sceneRender){
        return false;
    }
    if (pane == sceneProject.viewState.activePane || sceneProject.viewState.layout == SceneViewLayout::SINGLE){
        ray = sceneProject.sceneRender->screenToRay(x, y);
        return true;
    }
    const Runtime* rt = findRuntime(sceneProject.id);
    if (!rt || pane < 0 || pane >= SceneViewState::MAX_PANES || !rt->panes[(size_t)pane].camera){
        return false;
    }
    const Pane& p = rt->panes[(size_t)pane];
    Camera* camera = p.camera;
    const bool ortho = paneOrthographic(sceneProject.viewState, pane);
    ray = screenToRayFromMatrix(camera->getViewProjectionMatrix(), ortho, camera->getNearClip(), camera->getFarClip(), x, y,
                                (float)std::max(p.width, 1), (float)std::max(p.height, 1));
    return true;
}

bool SceneViewController::panesNeedRedraw(SceneProject& sceneProject, Runtime& rt){
    // the scene changed (the view redraws for something else than its own camera moving), the
    // selection changed, or the layout did: every inactive pane; a pane's own changes: that one
    const SceneCameraPose pose = currentPose(sceneProject);
    const bool cameraMoved = !rt.renderedPoseValid || !samePose(pose, rt.renderedPose);
    uint64_t selectionHash = 1469598103934665603ull;
    for (Entity entity : project->getSelectedEntities(sceneProject.id)){
        selectionHash = (selectionHash ^ (uint64_t)entity) * 1099511628211ull;
    }
    const bool contentChanged = !cameraMoved || selectionHash != rt.renderedSelection || rt.renderedLayout != sceneProject.viewState.layout ||
                                sceneProject.playState != ScenePlayState::STOPPED;
    rt.renderedPose = pose;
    rt.renderedPoseValid = true;
    rt.renderedSelection = selectionHash;
    rt.renderedLayout = sceneProject.viewState.layout;
    if (contentChanged){
        markPanesDirty(rt);
    }
    for (const Pane& pane : rt.panes){
        if (pane.dirty){
            return true;
        }
    }
    return false;
}

void SceneViewController::renderInactivePanes(SceneProject& sceneProject){
    if (sceneProject.sceneType != SceneType::SCENE_3D || !sceneProject.sceneRender || !sceneProject.scene){
        return;
    }
    const SceneViewState& state = sceneProject.viewState;
    if (state.layout == SceneViewLayout::SINGLE || state.maximizedPane >= 0){
        return;
    }
    Runtime& rt = runtime(sceneProject.id);
    if (!panesNeedRedraw(sceneProject, rt)){
        return;
    }
    for (int pane = 0; pane < state.paneCount(); pane++){
        Pane& p = rt.panes[(size_t)pane];
        if (pane == state.activePane){
            p.dirty = false;
            continue;
        }
        if (!p.dirty || p.width <= 0 || p.height <= 0){
            continue;
        }
        writePaneCamera(sceneProject, pane);
        if (!p.camera){
            continue;
        }
        const SceneViewPane& paneState = state.panes[(size_t)pane];
        SceneRender::PaneViewFlags flags;
        flags.shadows = paneState.shading == PaneShading::SHADED_SHADOWS;
        flags.postProcessing = false;
        flags.gridPlane = paneOrthographic(state, pane) ? gridPlaneOf(sceneview::alignedAxis(paneState.pose.target - paneState.pose.position)) : 0;
        sceneProject.sceneRender->drawPaneView(PANE_VIEW_ID_BASE + (uint32_t)pane, p.camera->getEntity(), &p.framebuffer, p.width, p.height, flags);
        p.dirty = false;
        p.draws++;
    }
}

// ---- bookmarks

std::string SceneViewController::bookmarkName(const SceneViewState& state, int slot){
    if (slot < 0 || slot >= SceneViewState::BOOKMARK_SLOTS){
        return std::string();
    }
    const CameraBookmark& bookmark = state.bookmarks[(size_t)slot];
    if (!bookmark.name.empty()){
        return bookmark.name;
    }
    return slot == 0 ? std::string("Home") : std::to_string(slot);
}

fs::path SceneViewController::bookmarkPreviewPath(const SceneProject& sceneProject, int slot) const{
    return project->getProjectPath() / ".jasperengine" / "user" / "bookmarks" / sceneKey(sceneProject) / (std::to_string(slot) + ".png");
}

bool SceneViewController::storeBookmark(SceneProject& sceneProject, int slot, std::string& error){
    if (slot < 0 || slot >= SceneViewState::BOOKMARK_SLOTS){
        error = "slot must be 0 (Home) to 9.";
        return false;
    }
    if (!sceneProject.sceneRender){
        error = "The scene has no Scene view.";
        return false;
    }
    CameraBookmark& bookmark = sceneProject.viewState.bookmarks[(size_t)slot];
    bookmark.defined = true;
    bookmark.pose = currentPose(sceneProject);
    // the view as it shows now, 160x90 (the last drawn frame of the active pane)
    std::vector<uint8_t> png;
    if (GraphicUtils::encodeFramebufferThumbnailPng(sceneProject.sceneRender->getFramebuffer(), BOOKMARK_PREVIEW_WIDTH, BOOKMARK_PREVIEW_HEIGHT, png)){
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        const io::WriteResult written = io::writeFileAtomic(bookmarkPreviewPath(sceneProject, slot),
                                                            std::string_view(reinterpret_cast<const char*>(png.data()), png.size()), options);
        if (!written.ok()){
            Out::warning("Camera bookmark preview not saved: %s", written.message.c_str());
        }
    }
    Runtime& rt = runtime(sceneProject.id);
    rt.previewRevision[(size_t)slot]++;
    saveWorkspace();
    return true;
}

bool SceneViewController::recallBookmark(SceneProject& sceneProject, int slot, std::string& error){
    if (slot < 0 || slot >= SceneViewState::BOOKMARK_SLOTS){
        error = "slot must be 0 (Home) to 9.";
        return false;
    }
    const CameraBookmark& bookmark = sceneProject.viewState.bookmarks[(size_t)slot];
    if (!bookmark.defined){
        if (slot == 0){
            // Home without a stored pose: the default view of the scene
            if (sceneProject.sceneType == SceneType::SCENE_3D){
                SceneCameraPose home;
                home.position = Vector3(10, 4, 10);
                home.target = Vector3(0, 0, 0);
                home.fieldOfView = sceneProject.viewState.camera.fieldOfView;
                moveTo(sceneProject, home, true);
                return true;
            }
            return frameAll(sceneProject);
        }
        error = "Camera bookmark " + std::to_string(slot) + " is empty.";
        return false;
    }
    moveTo(sceneProject, bookmark.pose, true);
    return true;
}

bool SceneViewController::deleteBookmark(SceneProject& sceneProject, int slot, std::string& error){
    if (slot < 0 || slot >= SceneViewState::BOOKMARK_SLOTS){
        error = "slot must be 0 (Home) to 9.";
        return false;
    }
    sceneProject.viewState.bookmarks[(size_t)slot] = CameraBookmark();
    std::error_code ec;
    fs::remove(bookmarkPreviewPath(sceneProject, slot), ec);
    Runtime& rt = runtime(sceneProject.id);
    rt.previews[(size_t)slot].reset();
    rt.previewRevision[(size_t)slot]++;
    saveWorkspace();
    return true;
}

bool SceneViewController::renameBookmark(SceneProject& sceneProject, int slot, const std::string& name, std::string& error){
    if (slot < 0 || slot >= SceneViewState::BOOKMARK_SLOTS){
        error = "slot must be 0 (Home) to 9.";
        return false;
    }
    CameraBookmark& bookmark = sceneProject.viewState.bookmarks[(size_t)slot];
    if (!bookmark.defined){
        error = "Camera bookmark " + std::to_string(slot) + " is empty.";
        return false;
    }
    bookmark.name = name;
    saveWorkspace();
    return true;
}

Texture* SceneViewController::bookmarkPreview(SceneProject& sceneProject, int slot){
    Runtime& rt = runtime(sceneProject.id);
    std::unique_ptr<Texture>& texture = rt.previews[(size_t)slot];
    if (texture && rt.loadedRevision[(size_t)slot] == rt.previewRevision[(size_t)slot]){
        return texture->getRender() && texture->getRender()->isCreated() ? texture.get() : nullptr;
    }
    texture.reset();
    rt.loadedRevision[(size_t)slot] = rt.previewRevision[(size_t)slot];
    const fs::path path = bookmarkPreviewPath(sceneProject, slot);
    std::error_code ec;
    if (!fs::exists(path, ec)){
        return nullptr;
    }
    // a new id per revision: the texture pool keys by it
    const std::string id = path.string() + "#" + std::to_string(rt.previewRevision[(size_t)slot]);
    texture = std::make_unique<Texture>(id, TextureData(path.string().c_str()));
    if (!texture->load()){
        texture.reset();
        return nullptr;
    }
    return texture->getRender() && texture->getRender()->isCreated() ? texture.get() : nullptr;
}

// ---- measures

uint32_t SceneViewController::addMeasure(SceneProject& sceneProject, const Vector3& from, const Vector3& to){
    SceneViewState& state = sceneProject.viewState;
    SceneMeasure measure;
    measure.id = state.nextMeasureId++;
    measure.from = from;
    measure.to = to;
    state.measures.push_back(measure);
    sceneProject.needUpdateRender = true;
    saveWorkspace();
    return measure.id;
}

bool SceneViewController::deleteMeasure(SceneProject& sceneProject, uint32_t id){
    std::vector<SceneMeasure>& measures = sceneProject.viewState.measures;
    auto it = std::find_if(measures.begin(), measures.end(), [id](const SceneMeasure& m){ return m.id == id; });
    if (it == measures.end()){
        return false;
    }
    measures.erase(it);
    Runtime& rt = runtime(sceneProject.id);
    if (rt.selectedMeasure == id){
        rt.selectedMeasure = 0;
    }
    sceneProject.needUpdateRender = true;
    saveWorkspace();
    return true;
}

void SceneViewController::clearMeasures(SceneProject& sceneProject){
    sceneProject.viewState.measures.clear();
    runtime(sceneProject.id).selectedMeasure = 0;
    sceneProject.needUpdateRender = true;
    saveWorkspace();
}

bool SceneViewController::isMeasuring(uint32_t sceneId) const{
    const Runtime* rt = findRuntime(sceneId);
    return rt && rt->measuring;
}

uint32_t SceneViewController::selectedMeasure(uint32_t sceneId) const{
    const Runtime* rt = findRuntime(sceneId);
    return rt ? rt->selectedMeasure : 0;
}

bool SceneViewController::hasSelectedMeasure(uint32_t sceneId) const{
    const Runtime* rt = findRuntime(sceneId);
    return rt && rt->measuring && rt->selectedMeasure != 0;
}

void SceneViewController::setMeasuring(SceneProject& sceneProject, bool measuring){
    Runtime& rt = runtime(sceneProject.id);
    rt.measuring = measuring;
    rt.measureDragging = false;
    rt.measureValid = false;
    sceneProject.needUpdateRender = true;
}

bool SceneViewController::measurePoint(SceneProject& sceneProject, float x, float y, MeasureSnap snap, Vector3& point){
    if (!sceneProject.sceneRender){
        return false;
    }
    const SceneSurfaceHit hit = surfaceAt(sceneProject, x, y, {}, true);
    if (hit.source == SceneSurfaceHit::Source::NONE){
        return false;
    }
    point = hit.point;
    if (snap == MeasureSnap::VERTEX && hit.source == SceneSurfaceHit::Source::SURFACE &&
            (hit.hit.kind == PickKind::MESH || hit.hit.kind == PickKind::INSTANCE)){
        // the hit triangle's corner nearest the pointer, within 12 px
        Scene* scene = sceneProject.scene;
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(hit.entity);
        const Transform* transform = scene->findComponent<Transform>(hit.entity);
        PickGeometryLayout layout;
        uint32_t submesh = 0;
        uint32_t corners[3];
        if (mesh && transform && picking::resolveSources(*mesh, layout) && picking::triangleCorners(layout, hit.hit.triangle, submesh, corners) &&
                submesh < layout.submeshes.size()){
            Matrix4 model = transform->modelMatrix;
            if (hit.hit.kind == PickKind::INSTANCE && hit.hit.instance >= 0){
                if (const InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(hit.entity);
                        instmesh && (size_t)hit.hit.instance < instmesh->instances.size()){
                    const InstanceData& instance = instmesh->instances[(size_t)hit.hit.instance];
                    const Matrix4 rotation = instmesh->instancedBillboard ? Matrix4() : instance.rotation.getRotationMatrix();
                    model = model * Matrix4::translateMatrix(instance.position) * rotation * Matrix4::scaleMatrix(instance.scale);
                }
            }
            const Matrix4 viewProjection = sceneProject.sceneRender->getCamera()->getViewProjectionMatrix();
            const float width = (float)std::max(sceneProject.sceneRender->getViewWidth(), 1);
            const float height = (float)std::max(sceneProject.sceneRender->getViewHeight(), 1);
            float best = VERTEX_SNAP_PIXELS * sceneProject.sceneRender->getOverlayScale();
            for (uint32_t corner : corners){
                Vector3 local;
                if (!sourceVertex(layout.submeshes[submesh], corner, local)){
                    continue;
                }
                const Vector3 world = model * local;
                Vector4 clip = viewProjection * Vector4(world.x, world.y, world.z, 1.0f);
                if (clip.w <= 0.0f){
                    continue;
                }
                const float sx = (clip.x / clip.w * 0.5f + 0.5f) * width;
                const float sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height;
                const float d = std::hypot(sx - x, sy - y);
                if (d <= best){
                    best = d;
                    point = world;
                }
            }
        }
    }else if (snap == MeasureSnap::GRID){
        const float spacing = sceneProject.sceneType == SceneType::SCENE_3D ? sceneProject.displaySettings.gridSpacing3D
                                                                             : sceneProject.displaySettings.gridSpacing2D;
        if (spacing > 0.0f){
            point = Vector3(std::round(point.x / spacing) * spacing, std::round(point.y / spacing) * spacing,
                            std::round(point.z / spacing) * spacing);
        }
    }
    return true;
}

bool SceneViewController::rulersApply(const SceneProject& sceneProject, bool orthographicAxisView){
    return sceneProject.sceneType != SceneType::SCENE_3D || orthographicAxisView;
}

// ---- visibility

void SceneViewController::setLayerFilter(SceneProject& sceneProject, uint32_t visible, uint32_t pickable){
    sceneview::applyLayerFilter(sceneProject, visible, pickable);
    Runtime& rt = runtime(sceneProject.id);
    for (Pane& pane : rt.panes){
        if (pane.camera){
            pane.camera->setCullingMask(visible);
        }
    }
    markPanesDirty(rt);
    saveWorkspace();
}

void SceneViewController::hideSelection(SceneProject& sceneProject){
    const std::vector<Entity> selection = project->getSelectedEntities(sceneProject.id);
    if (selection.empty()){
        return;
    }
    // hidden already, all of them: H shows them again (Unity's toggle)
    const bool allHidden = std::all_of(selection.begin(), selection.end(), [&](Entity e){ return sceneview::isHidden(sceneProject, e); });
    for (Entity entity : selection){
        sceneview::setHidden(sceneProject, entity, !allHidden, true);
    }
    markPanesDirty(runtime(sceneProject.id));
    saveWorkspace();
}

void SceneViewController::showAll(SceneProject& sceneProject){
    sceneview::showAll(sceneProject);
    if (sceneProject.viewState.layerFilter.visible != LAYER_MASK_EVERYTHING){
        sceneview::applyLayerFilter(sceneProject, LAYER_MASK_EVERYTHING, sceneProject.viewState.layerFilter.pickable);
    }
    markPanesDirty(runtime(sceneProject.id));
    saveWorkspace();
}

void SceneViewController::togglePickLockSelection(SceneProject& sceneProject){
    const std::vector<Entity> selection = project->getSelectedEntities(sceneProject.id);
    if (selection.empty()){
        return;
    }
    const bool allLocked = std::all_of(selection.begin(), selection.end(), [&](Entity e){ return sceneview::isPickLocked(sceneProject, e); });
    for (Entity entity : selection){
        sceneview::setPickLocked(sceneProject, entity, !allLocked, true);
    }
    saveWorkspace();
}

// ---- placement

SceneSurfaceHit SceneViewController::surfaceAt(SceneProject& sceneProject, float x, float y, std::span<const Entity> ignore, bool waitForBvh){
    SceneSurfaceHit result;
    if (!sceneProject.scene || !sceneProject.sceneRender){
        return result;
    }
    const Ray ray = sceneProject.sceneRender->screenToRay(x, y);
    const float length = ray.getDirection().length();
    if (!(length > 0.0f)){
        return result;
    }
    const Vector3 direction = ray.getDirection() / length;
    result.sceneId = sceneProject.id;

    // 1. what the view draws there: a triangle, a terrain or a UI rect on a pickable layer
    ScenePicker picker(project);
    PickFilter filter = picker.makeFilter(&sceneProject, &sceneProject, waitForBvh);
    filter.ignore = ignore;
    std::vector<PickHit> hits;
    sceneProject.scene->getPickBvh().raycast(ray, filter, hits, true);
    std::sort(hits.begin(), hits.end(), [](const PickHit& a, const PickHit& b){ return a.distance < b.distance; });
    for (const PickHit& hit : hits){
        if (hit.pending){
            result.pending = true;
        }
        if (hit.kind == PickKind::BOUNDS && !hit.pending){
            continue;
        }
        result.source = SceneSurfaceHit::Source::SURFACE;
        result.point = hit.point;
        result.entity = hit.entity;
        result.hit = hit;
        Vector3 normal = hit.normal;
        if (normal.squaredLength() < 1e-12f){
            normal = -direction;
        }
        normal.normalize();
        result.normal = normal;
        return result;
    }

    // 2. the ground plane: y = 0 in 3D, z = 0 in 2D and UI
    const bool is3D = sceneProject.sceneType == SceneType::SCENE_3D;
    const Plane ground = is3D ? Plane(Vector3(0, 1, 0), Vector3(0, 0, 0)) : Plane(Vector3(0, 0, 1), Vector3(0, 0, 0));
    if (RayReturn planeHit = ray.intersects(ground); planeHit){
        result.source = SceneSurfaceHit::Source::PLANE;
        result.point = planeHit.point;
        result.normal = is3D ? Vector3(0, 1, 0) : Vector3(0, 0, 1);
        return result;
    }

    // 3. along the ray
    result.source = SceneSurfaceHit::Source::ALONG_RAY;
    result.point = ray.getOrigin() + direction * DROP_FALLBACK_DISTANCE;
    result.normal = is3D ? Vector3(0, 1, 0) : Vector3(0, 0, 1);
    return result;
}

void SceneViewController::notifyStatus(const std::string& text, double seconds){
    StatusBar::setTransientMessage(text, seconds);
}
