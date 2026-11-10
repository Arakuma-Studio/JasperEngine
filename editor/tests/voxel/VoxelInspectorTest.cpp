// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R1, R53, R63, R65 (AC45): the Inspector's fields and buttons, the slice image's bytes, and what a
// click on a chunk selects.

#include "VoxelTestScene.h"

#include "Catalog.h"
#include "command/type/PropertyCmd.h"
#include "picking/ScenePicker.h"
#include "voxel/VoxelSliceRender.h"
#include "voxel/VoxelVolumeOps.h"

#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::voxeltest;

namespace {

    class InspectorVoxels : public VoxelSceneTest {};
    class VoxelSlice : public VoxelSceneTest {};
    class VoxelPicking : public VoxelSceneTest {};

    constexpr ComponentType VOLUME_TYPE = ComponentType::VoxelVolumeComponent;

}

TEST_F(InspectorVoxels, UndoableFields) {
    addVolume();
    Scene* scene = sceneProject->scene;
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    history->clear();

    // R65's labels, in Unity's words, each on a real field of the component
    std::set<std::string> labels;
    for (const VoxelInspectorRow& row : voxelInspectorRows()) {
        labels.insert(row.label);
        const PropertyData property = Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property);
        EXPECT_NE(property.ref, nullptr) << row.property;
        EXPECT_STRNE(row.help, "") << row.property;
    }
    for (const char* wanted : {"Quality", "LOD Levels", "Cast Shadows", "Collision", "Terrain", "Terrain Cut", "Overlap Depth"}) {
        EXPECT_TRUE(labels.count(wanted)) << wanted;
    }
    EXPECT_FALSE(labels.count("Voxel Size")) << "read-only: the asset's, not a row of the component";

    // every row writes through a command, and undo brings the old value back
    const VoxelVolumeComponent& component = scene->getComponent<VoxelVolumeComponent>(owner);
    size_t steps = 0;
    for (const VoxelInspectorRow& row : voxelInspectorRows()) {
        const PropertyData property = Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property);
        ASSERT_NE(property.ref, nullptr) << row.property;
        switch (row.kind) {
            case VoxelInspectorRowKind::Bool: {
                const bool old = *static_cast<bool*>(property.ref);
                history->addCommandNoMerge(new PropertyCmd<bool>(project.get(), sceneProject->id, owner, VOLUME_TYPE, row.property, !old));
                EXPECT_EQ(*static_cast<bool*>(Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property).ref), !old) << row.property;
                ASSERT_TRUE(history->undo());
                EXPECT_EQ(*static_cast<bool*>(Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property).ref), old) << row.property;
                break;
            }
            case VoxelInspectorRowKind::Int:
            case VoxelInspectorRowKind::Enum: {
                const int old = *static_cast<int*>(property.ref);
                const int next = std::string(row.property) == "lodRadius" ? 6 : old + 1;
                history->addCommandNoMerge(new PropertyCmd<int>(project.get(), sceneProject->id, owner, VOLUME_TYPE, row.property, next));
                EXPECT_EQ(*static_cast<int*>(Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property).ref), next) << row.property;
                ASSERT_TRUE(history->undo());
                EXPECT_EQ(*static_cast<int*>(Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property).ref), old) << row.property;
                break;
            }
            case VoxelInspectorRowKind::Float: {
                const float old = *static_cast<float*>(property.ref);
                history->addCommandNoMerge(new PropertyCmd<float>(project.get(), sceneProject->id, owner, VOLUME_TYPE, row.property, old + 2.0f));
                EXPECT_FLOAT_EQ(*static_cast<float*>(Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property).ref), old + 2.0f) << row.property;
                ASSERT_TRUE(history->undo());
                EXPECT_FLOAT_EQ(*static_cast<float*>(Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property).ref), old) << row.property;
                break;
            }
            case VoxelInspectorRowKind::Entity: {
                const Entity old = *static_cast<Entity*>(property.ref);
                history->addCommandNoMerge(new PropertyCmd<Entity>(project.get(), sceneProject->id, owner, VOLUME_TYPE, row.property, owner));
                EXPECT_EQ(*static_cast<Entity*>(Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property).ref), owner) << row.property;
                ASSERT_TRUE(history->undo());
                EXPECT_EQ(*static_cast<Entity*>(Catalog::findProperty(scene, owner, VOLUME_TYPE, row.property).ref), old) << row.property;
                break;
            }
            case VoxelInspectorRowKind::Asset:
                EXPECT_EQ(std::string(row.property), "volumeData");
                break;
        }
        steps++;
    }
    EXPECT_GT(steps, 10u);
    EXPECT_EQ(component.lodRadius, 4);

    // Voxel Size is the asset's, and a Scale goes into it in one undo step
    Transform& transform = scene->getComponent<Transform>(owner);
    transform.scale = Vector3(1.0f, 2.0f, 1.0f);
    std::string error;
    EXPECT_FALSE(applyScaleToVoxelSize(project.get(), sceneProject->id, owner, &error)) << "a non-uniform scale has nowhere to go";
    EXPECT_FALSE(error.empty());
    transform.scale = Vector3(1.0f, 1.0f, 1.0f);
    history->addCommandNoMerge(new PropertyCmd<Vector3>(project.get(), sceneProject->id, owner, ComponentType::Transform, "scale", Vector3(2.0f, 2.0f, 2.0f)));
    const std::vector<voxel::EditOp> dug = digAt(2);
    ASSERT_TRUE(system->edit(owner, dug, voxel::EditOptions{}).ok);
    const uint64_t contentBefore = voxel::contentHash(*asset->store);
    const float sizeBefore = asset->store->desc().voxelSize;
    const size_t before = history->entries().size();
    error.clear();
    ASSERT_TRUE(applyScaleToVoxelSize(project.get(), sceneProject->id, owner, &error)) << error;
    EXPECT_EQ(history->entries().size(), before + 1);
    EXPECT_EQ(scene->getComponent<Transform>(owner).scale, Vector3(1.0f, 1.0f, 1.0f));
    system->update(0.0);
    std::shared_ptr<VoxelAsset> resized = system->getAsset(owner);
    ASSERT_NE(resized, nullptr);
    ASSERT_NE(resized->store, nullptr);
    EXPECT_FLOAT_EQ(resized->store->desc().voxelSize, sizeBefore * 2.0f);
    EXPECT_EQ(voxel::contentHash(*resized->store), contentBefore) << "samples are in voxels: only the metres change";
    EXPECT_TRUE(resized->dirty);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(scene->getComponent<Transform>(owner).scale, Vector3(2.0f, 2.0f, 2.0f));
    system->update(0.0);
    resized = system->getAsset(owner);
    ASSERT_NE(resized, nullptr);
    ASSERT_NE(resized->store, nullptr);
    EXPECT_FLOAT_EQ(resized->store->desc().voxelSize, sizeBefore);
}

TEST_F(InspectorVoxels, MakeUnique) {
    addVolume();
    Scene* scene = sceneProject->scene;

    // what Duplicate leaves: two GameObjects on one file
    const Entity twin = scene->createEntity();
    scene->addComponent<Transform>(twin, {});
    // (a copy first: adding to the array can move the component it would be copied from)
    const VoxelVolumeComponent same = scene->getComponent<VoxelVolumeComponent>(owner);
    scene->addComponent<VoxelVolumeComponent>(twin, same);
    sceneProject->entities.push_back(twin);
    system->update(0.0);
    ASSERT_EQ(system->getAsset(twin), asset);
    EXPECT_EQ(voxelAssetUserCount(project.get(), assetPath), 2);

    const std::vector<voxel::EditOp> ops = digAt(1);
    ASSERT_TRUE(system->edit(owner, ops, voxel::EditOptions{}).ok);
    const uint64_t content = hash();
    const AssetRef oldRef = scene->getComponent<VoxelVolumeComponent>(owner).volumeData;

    CommandHistory* history = CommandHandle::get(sceneProject->id);
    history->clear();
    std::string error;
    ASSERT_TRUE(makeVoxelVolumeUnique(project.get(), sceneProject->id, twin, &error)) << error;
    EXPECT_EQ(history->entries().size(), 1u);

    const AssetRef newRef = scene->getComponent<VoxelVolumeComponent>(twin).volumeData;
    EXPECT_NE(newRef.path, oldRef.path);
    EXPECT_FALSE(newRef.guid.isNull());
    EXPECT_NE(newRef.guid, oldRef.guid);
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / newRef.path));
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(owner).volumeData.path, oldRef.path) << "the others keep the old file";

    // it opens with what the old one held, and from now on they part
    system->update(0.0);
    system->update(0.0);
    const std::shared_ptr<VoxelAsset> copy = system->getAsset(twin);
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(copy->store, nullptr);
    EXPECT_NE(copy, asset);
    EXPECT_EQ(voxel::contentHash(*copy->store), content);
    EXPECT_EQ(voxelAssetUserCount(project.get(), assetPath), 1);
    const std::vector<voxel::EditOp> more = digAt(9);
    ASSERT_TRUE(system->edit(twin, more, voxel::EditOptions{}).ok);
    EXPECT_NE(voxel::contentHash(*copy->store), content);
    EXPECT_EQ(hash(), content) << "an edit of the copy leaves the original";

    ASSERT_TRUE(history->undo());
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(twin).volumeData.path, oldRef.path);
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(twin).volumeData.guid, oldRef.guid);
    ASSERT_TRUE(history->redo());
    EXPECT_EQ(scene->getComponent<VoxelVolumeComponent>(twin).volumeData.path, newRef.path);
}

TEST_F(VoxelSlice, DeterministicImage) {
    addVolume();
    const std::vector<voxel::EditOp> ops = digAt(0);
    ASSERT_TRUE(system->edit(owner, ops, voxel::EditOptions{}).ok);

    VoxelSliceRequest request;
    request.axis = VoxelSliceAxis::Z;
    request.position = 32;
    request.maxWidth = 256;
    VoxelSliceImage first, second;
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, first));
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, second));
    ASSERT_FALSE(first.rgba.empty());
    EXPECT_EQ(first.rgba, second.rgba) << "the same store and request give the same bytes";
    EXPECT_EQ(first.width, 256);
    EXPECT_EQ(first.height, 256);
    EXPECT_EQ(first.rgba.size(), static_cast<size_t>(first.width) * first.height * 4);
    for (size_t i = 3; i < first.rgba.size(); i += 4) ASSERT_EQ(first.rgba[i], 255) << "opaque";

    // ... through every way a block can be held: the volume as read back from its file, blocks cold
    std::string error;
    ASSERT_TRUE(saveVoxelAsset(*asset, &error)) << error;
    VoxelAssets::forget(assetPath);
    const std::shared_ptr<VoxelAsset> reread = VoxelAssets::acquire(assetPath);
    ASSERT_NE(reread->store, nullptr);
    VoxelSliceImage fromFile;
    ASSERT_TRUE(renderVoxelSlice(*reread->store, request, fromFile));
    EXPECT_EQ(fromFile.rgba, first.rgba);
    EXPECT_DOUBLE_EQ(fromFile.isoLineLength, first.isoLineLength);

    // what it reports: ground up to y = 32 in a plane of 64: about half solid; a line across it
    EXPECT_GT(first.solidFraction, 0.35);
    EXPECT_LT(first.solidFraction, 0.55);
    EXPECT_DOUBLE_EQ(first.solidFraction + first.airFraction, 1.0);
    EXPECT_GT(first.isoLineLength, 55.0);

    // the row order: Y is up in the image; the bottom middle is solid, darker than the shading at the iso-line
    const auto pixel = [](const VoxelSliceImage& image, int x, int y) { return image.rgba.data() + 4 * (static_cast<size_t>(y) * image.width + x); };
    const uint8_t* air = pixel(first, first.width / 2, first.height / 8);
    const uint8_t* solid = pixel(first, first.width / 2, first.height - first.height / 8);
    EXPECT_NE(std::memcmp(air, solid, 3), 0);
    const VoxelSlotColor slot = voxelSlotColor(0);
    EXPECT_GT(static_cast<int>(solid[0]) - static_cast<int>(solid[2]), 0) << "solid carries slot 0's warm colour";
    EXPECT_LE(solid[0], slot.r);
    EXPECT_GT(air[2], air[0]) << "air is the cool shade";

    // a pixel's place in the volume, for the click that moves the pivot
    const voxel::Vec3d middle = voxelSlicePixelToLocal(first, static_cast<float>(first.width) * 0.5f, static_cast<float>(first.height) * 0.25f);
    EXPECT_NEAR(middle.x, 32.0, 1e-3);
    EXPECT_NEAR(middle.y, 48.0, 1e-3) << "a quarter down from the top";
    EXPECT_NEAR(middle.z, 32.0, 1e-3);

    // slots as colour: painting changes the solid side
    sceneProject->scene->getComponent<VoxelVolumeComponent>(owner).materialSlots.resize(3);
    voxel::EditOp paint;
    paint.kind = voxel::EditKind::Paint;
    paint.material = 2;
    paint.shape.center = voxel::Vec3d{32.0, 25.0, 32.0};
    paint.shape.radius = 5.0;
    voxel::EditOptions options;
    ASSERT_TRUE(system->edit(owner, std::span<const voxel::EditOp>(&paint, 1), options).ok);
    VoxelSliceImage painted;
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, painted));
    EXPECT_NE(painted.rgba, first.rgba);

    // the grid is optional, and the plane and the axis change the picture
    request.blockGrid = false;
    VoxelSliceImage noGrid;
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, noGrid));
    EXPECT_NE(noGrid.rgba, painted.rgba);
    request.blockGrid = true;
    request.axis = VoxelSliceAxis::Y;
    request.position = 10000;
    VoxelSliceImage top;
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, top));
    EXPECT_EQ(top.position, 63) << "past the end clamps";
    EXPECT_NEAR(top.solidFraction, 0.0, 1e-9);
    request.axis = VoxelSliceAxis::X;
    request.position = -1;
    VoxelSliceImage xPlane;
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, xPlane));
    EXPECT_EQ(xPlane.position, 32) << "below zero is the middle";

    // the terrain's outline is drawn where its height says
    request.axis = VoxelSliceAxis::Z;
    request.position = 32;
    VoxelSliceImage plain;
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, plain));
    request.terrainHeight = [](double, double, double& height) {
        height = 50.0;
        return true;
    };
    VoxelSliceImage outlined;
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, outlined));
    EXPECT_NE(outlined.rgba, plain.rgba);
    const uint8_t* onTerrain = pixel(outlined, outlined.width / 2, outlined.height - 1 - 50 * outlined.scaleNum);
    EXPECT_EQ(onTerrain[1], 232) << "the outline's colour on row 50";

    // a smaller limit downsamples and stays deterministic
    request.terrainHeight = nullptr;
    request.maxWidth = 32;
    VoxelSliceImage small1, small2;
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, small1));
    ASSERT_TRUE(renderVoxelSlice(*asset->store, request, small2));
    EXPECT_EQ(small1.width, 32);
    EXPECT_EQ(small1.rgba, small2.rgba);
}

TEST_F(VoxelPicking, ClickSelectsOwner) {
    addVolume();
    system->finishPendingWork();
    const std::vector<Entity> chunks = system->getGeneratedEntities(owner);
    ASSERT_FALSE(chunks.empty());
    const Entity chunk = chunks.front();
    Scene* scene = sceneProject->scene;

    // a chunk under a click is the volume
    EXPECT_EQ(ScenePicker::pickedEntity(scene, chunk), owner);
    // anything else is itself
    const Entity other = scene->createEntity();
    scene->addComponent<Transform>(other, {});
    EXPECT_EQ(ScenePicker::pickedEntity(scene, other), other);

    // the pick's filter lets a chunk through because of its owner: chunks are not in the Hierarchy
    ScenePicker picker(project.get());
    const PickFilter filter = picker.makeFilter(sceneProject, sceneProject, false);
    ASSERT_TRUE(filter.accept);
    EXPECT_TRUE(filter.accept(chunk));
    EXPECT_FALSE(filter.accept(other)) << "not pickable, not owned by anything pickable";
}
