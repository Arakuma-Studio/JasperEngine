// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelSliceView.h"

#include "Backend.h"
#include "Project.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "external/IconsFontAwesome6.h"
#include "object/Camera.h"
#include "object/TerrainEdit.h"
#include "render/SceneRender.h"
#include "subsystem/VoxelSystem.h"
#include "voxel/VoxelSceneOverlay.h"

#include "imgui.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

VoxelSliceView::VoxelSliceView(Project* project): project(project) {
}

VoxelSliceView::~VoxelSliceView() {
    retireTexture();
    for (Texture& old : retired) old.destroy();
}

void VoxelSliceView::open() {
    setOpen(true);
    focusRequested = true;
}

void VoxelSliceView::setOpen(bool value) {
    if (value == windowOpen) return;
    windowOpen = value;
    if (!windowOpen) {
        clearVoxelSlicePlane();
        focusRequested = false;
    }
}

void VoxelSliceView::retireTexture() {
    if (!texture.empty()) retired.push_back(texture);
    texture = Texture();
}

bool VoxelSliceView::movePivotTo(Project* project, uint32_t sceneId, Entity owner, const VoxelSliceImage& image, float pixelX, float pixelY) {
    SceneProject* sceneProject = project->getScene(sceneId);
    std::shared_ptr<VoxelSystem> system = sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<VoxelSystem>() : nullptr;
    if (!system || !sceneProject->sceneRender) return false;
    const Vector3 world = system->localToWorld(owner, voxelSlicePixelToLocal(image, pixelX, pixelY));
    Camera* camera = sceneProject->sceneRender->getCamera();
    if (!camera) return false;
    // the camera keeps its view: moving the pivot moves the eye with it
    const Vector3 offset = camera->getPosition() - camera->getTarget();
    camera->setTarget(world);
    camera->setPosition(world + offset);
    sceneProject->needUpdateRender = true;
    return true;
}

void VoxelSliceView::redrawImage(voxel::Store& store, Entity owner) {
    VoxelSliceRequest request;
    request.axis = axis;
    request.position = position;
    request.blockGrid = blockGrid;
    request.maxWidth = 768;

    // the terrain's outline, from T1's query over a copy of its texels
    SceneProject* sceneProject = project->getScene(sceneId);
    TerrainSurfaceSnapshot snapshot;
    std::shared_ptr<VoxelSystem> system = sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<VoxelSystem>() : nullptr;
    const VoxelVolumeComponent* component = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<VoxelVolumeComponent>(owner) : nullptr;
    TerrainComponent* terrain = component && component->terrain != NULL_ENTITY ? sceneProject->scene->findComponent<TerrainComponent>(component->terrain) : nullptr;
    const Transform* terrainTransform = terrain ? sceneProject->scene->findComponent<Transform>(component->terrain) : nullptr;
    const Transform* ownerTransform = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<Transform>(owner) : nullptr;
    if (terrain && terrainTransform && ownerTransform) {
        snapshotTerrainSurface(*terrain, snapshot);
        // R1: an attached volume has no rotation, so the frames differ by a translation
        const Vector3 offset = ownerTransform->worldPosition - terrainTransform->worldPosition;
        const float half = terrain->terrainSize * 0.5f;
        request.terrainHeight = [&snapshot, offset, half](double x, double z, double& height) {
            const float localX = static_cast<float>(x) + offset.x;
            const float localZ = static_cast<float>(z) + offset.z;
            if (localX < -half || localX > half || localZ < -half || localZ > half) return false;
            height = static_cast<double>(terraincore::sampleHeight(snapshot.view, localX, localZ)) - static_cast<double>(offset.y);
            return true;
        };
    }
    if (!renderVoxelSlice(store, request, image)) return;

    retireTexture();
    const size_t bytes = image.rgba.size();
    void* pixels = std::malloc(bytes);
    if (!pixels) return;
    std::memcpy(pixels, image.rgba.data(), bytes);
    // TextureData frees what it is given
    TextureData data(image.width, image.height, static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
    texture.setData("editor:voxel-slice:" + std::to_string(++textureCounter), data);
    texture.setMinFilter(TextureFilter::LINEAR);
    texture.setMagFilter(TextureFilter::NEAREST);
    texture.getRender();
    texture.releaseData();
}

void VoxelSliceView::show() {
    for (Texture& old : retired) old.destroy();
    retired.clear();
    if (!windowOpen) return;

    SceneProject* selected = project->getSelectedScene();
    if (selected && selected->scene) {
        const std::vector<Entity> entities = project->getSelectedEntities(selected->id);
        if (entities.size() == 1 && selected->scene->findComponent<VoxelVolumeComponent>(entities[0])) {
            sceneId = selected->id;
            target = entities[0];
        }
    }

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, ImGui::GetFontSize() * 28.0f), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    bool open = windowOpen;
    if (!ImGui::Begin(WINDOW_NAME, &open)) {
        ImGui::End();
        if (!open) setOpen(false);
        return;
    }

    SceneProject* sceneProject = project->getScene(sceneId);
    std::shared_ptr<VoxelSystem> system = sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<VoxelSystem>() : nullptr;
    voxel::Store* store = system && target != NULL_ENTITY && sceneProject->scene->isEntityCreated(target) ? system->getStore(target) : nullptr;
    if (!store) {
        clearVoxelSlicePlane();
        ImGui::TextWrapped("Select a loaded Voxel Volume to see a slice of it.");
    } else {
        const voxel::Int3 samples = store->levelSamples(0);
        int axisValue = static_cast<int>(axis);
        ImGui::RadioButton("X", &axisValue, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Y", &axisValue, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Z", &axisValue, 2);
        axis = static_cast<VoxelSliceAxis>(axisValue);
        const int count = axis == VoxelSliceAxis::X ? samples.x : (axis == VoxelSliceAxis::Y ? samples.y : samples.z);
        if (position < 0 || position >= count) position = count / 2;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        ImGui::SliderInt("Position", &position, 0, count - 1, "%d");
        ImGui::SameLine();
        ImGui::Checkbox("Block Grid", &blockGrid);
        setVoxelSlicePlane(sceneId, target, axis, position);

        if (drawnStore != store || drawnVersion != store->version() || drawnAxis != axis || drawnPosition != position || drawnGrid != blockGrid) {
            redrawImage(*store, target);
            drawnStore = store;
            drawnVersion = store->version();
            drawnAxis = axis;
            drawnPosition = position;
            drawnGrid = blockGrid;
        }

        ImGui::TextDisabled("Solid %.1f%%  Air %.1f%%  Iso-line %.1f m", image.solidFraction * 100.0, image.airFraction * 100.0, image.isoLineLength);

        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (image.width > 0 && image.height > 0 && available.x > 1.0f && available.y > 1.0f) {
            const float fit = std::min(available.x / static_cast<float>(image.width), available.y / static_cast<float>(image.height));
            const ImVec2 size(static_cast<float>(image.width) * fit, static_cast<float>(image.height) * fit);
            const ImVec2 corner = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("slice_image", size);
            ImTextureID id = texture.empty() ? ImTextureID{} : Backend::getImGuiTexture(texture.getRender());
            if (id != ImTextureID{}) ImGui::GetWindowDrawList()->AddImage(id, corner, ImVec2(corner.x + size.x, corner.y + size.y));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                const ImVec2 mouse = ImGui::GetMousePos();
                movePivotTo(project, sceneId, target, image, (mouse.x - corner.x) / fit, (mouse.y - corner.y) / fit);
            }
            if (ImGui::IsItemHovered()) {
                if (ImGui::GetIO().MouseWheel != 0.0f) {
                    position = std::clamp(position + (ImGui::GetIO().MouseWheel > 0.0f ? 1 : -1), 0, count - 1);
                }
                ImGui::SetItemTooltip("Click: the Scene camera's pivot moves here. Wheel: the plane moves.");
            }
        }
    }
    ImGui::End();
    if (!open) setOpen(false);
}
