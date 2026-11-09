// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R33, R37: a generated volume opened again holds what it held. A preset's base is rebuilt from the
// file's description; a base no description rebuilds goes in the file whole.

#include "VoxelTestScene.h"

#include "EditorHost.h"
#include "component/TerrainComponent.h"
#include "pool/TextureDataPool.h"
#include "voxel/VoxelGeneration.h"
#include "voxel/VoxelGenerationTask.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::voxeltest;

namespace {

    // An editor's host that counts the main-thread tasks asked of it: each wakes a sleeping editor
    class CountingHost : public EditorHost {
    public:
        std::atomic<int> tasks{0};
        void enqueueMainThreadTask(std::function<void()> task) override {
            tasks++;
            EditorHost::enqueueMainThreadTask(std::move(task));
        }
    };

    class VoxelGenerate : public VoxelSceneTest {
    protected:
        // Generates on the fixture's volume, saves it, and reads the file back as a new asset
        std::shared_ptr<VoxelAsset> generateAndReopen(const VoxelGenerateRequest& request, uint64_t& generated) {
            VoxelGenerateResult result;
            const std::shared_ptr<VoxelGeneration> generation = VoxelGeneration::prepare(sceneProject->scene, owner, request, result);
            EXPECT_TRUE(generation && result.ok) << result.error << ": " << result.message;
            if (!generation) return nullptr;
            generation->run(nullptr, nullptr);
            EXPECT_TRUE(generation->finish(sceneProject->scene, nullptr).ok);
            generated = hash();
            std::string error;
            EXPECT_TRUE(saveVoxelAsset(*asset, &error)) << error;
            VoxelAssets::forget(assetPath);
            return VoxelAssets::acquire(assetPath);
        }
    };

}

TEST_F(VoxelGenerate, ReopenedPresetIsTheSame) {
    addVolume();
    for (const char* preset : {"caves", "overhangs"}) {
        VoxelGenerateRequest request;
        request.source = "preset";
        request.preset = preset;
        request.seed = 7;
        request.sizeScale = 0.75;
        request.keepEdits = false;
        uint64_t generated = 0;
        const std::shared_ptr<VoxelAsset> reopened = generateAndReopen(request, generated);
        ASSERT_NE(reopened, nullptr) << preset;
        ASSERT_NE(reopened->store, nullptr) << preset;
        EXPECT_TRUE(reopened->baseResolved) << preset;
        EXPECT_EQ(voxel::contentHash(*reopened->store), generated) << preset;
        // the next round works on the asset the system holds
        system->update(0.0);
        asset = system->getAsset(owner);
    }
}

TEST_F(VoxelGenerate, BaseNoDescriptionRebuildsIsStoredWhole) {
    addVolume();
    // a blend with a Terrain the volume is not attached to: nothing but the blocks can say what it was
    Scene* scene = sceneProject->scene;
    const Entity terrainEntity = scene->createEntity();
    scene->addComponent<Transform>(terrainEntity, {});
    Transform& transform = scene->getComponent<Transform>(terrainEntity);
    transform.position = Vector3(32.0f, 0.0f, 32.0f);
    transform.worldPosition = transform.position;
    transform.modelMatrix = Matrix4::translateMatrix(transform.position);
    std::vector<unsigned char> heights(64 * 64);
    for (size_t i = 0; i < heights.size(); i++) heights[i] = static_cast<unsigned char>(100 + (i * 7) % 40);
    TerrainComponent terrain;
    terrain.terrainSize = 64.0f;
    terrain.maxHeight = 60.0f;
    terrain.heightMap.setData("voxel-generate-height", TextureData(64, 64, static_cast<unsigned int>(heights.size()), ColorFormat::RED, 1, heights.data()));
    terrain.heightMap.setReleaseDataAfterLoad(false);
    scene->addComponent<TerrainComponent>(terrainEntity, terrain);

    VoxelGenerateRequest request;
    request.source = "terrain";
    request.terrain = terrainEntity;
    request.keepEdits = false;
    const uint64_t before = hash();
    uint64_t generated = 0;
    const std::shared_ptr<VoxelAsset> reopened = generateAndReopen(request, generated);
    EXPECT_NE(generated, before) << "the terrain's shape, not the solid ground it replaced";
    ASSERT_NE(reopened, nullptr);
    ASSERT_NE(reopened->store, nullptr);
    EXPECT_EQ(reopened->generator.kind, "baked");
    EXPECT_EQ(reopened->generator.otherKind, "terrain");
    EXPECT_TRUE(reopened->baseResolved);
    EXPECT_EQ(voxel::contentHash(*reopened->store), generated);
    TextureDataPool::remove("voxel-generate-height");
}

// The panel polls the task in its frames, which an idle editor does not draw: the finished worker asks
// for one
TEST_F(VoxelGenerate, FinishedTaskWakesAnIdleEditor) {
    addVolume();
    system->finishPendingWork();
    CountingHost host;
    EditorHost& previous = getEditorHost();
    setEditorHost(&host);
    VoxelGenerationTask task;
    VoxelGenerateRequest request;
    request.source = "preset";
    request.preset = "caves";
    request.seed = 3;
    std::string error;
    ASSERT_TRUE(task.start(project.get(), sceneProject->id, owner, request, false, error)) << error;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (host.tasks.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_GT(host.tasks.load(), 0);
    VoxelGenerateResult result;
    while (!task.poll(result) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(result.ok) << result.error;
    setEditorHost(&previous);
}
