// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A loaded project with a Voxel Volume in its scene: what the T6 editor tests drive.

#include "Project.h"
#include "System.h"
#include "command/CommandHandle.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"
#include "support/TestTempDir.h"
#include "voxel/VoxelAssetFile.h"
#include "voxel/VoxelEdit.h"
#include "voxel/VoxelGenerator.h"
#include "voxel/VoxelSample.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace jasperengine {
    // TEST-ONLY seam (System.h befriends it): detach / restore the instance
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

namespace jasperengine::editor::voxeltest {

    namespace fs = std::filesystem;

    inline voxel::VolumeDesc smallDesc() {
        voxel::VolumeDesc desc;
        desc.dimensions = {64, 64, 64};
        desc.voxelSize = 1.0f;
        desc.levels = 2;
        return desc;
    }

    inline voxel::GeneratorDesc groundGenerator() {
        voxel::GeneratorDesc generator;
        generator.kind = "solid_ground";
        generator.height = 32.0;
        return generator;
    }

    // the edit of `index`: spheres apart, each touching its own blocks; past 36 a layer deeper, so
    // every index changes something
    inline std::vector<voxel::EditOp> digAt(int index) {
        voxel::EditOp op;
        op.kind = voxel::EditKind::Dig;
        op.shape.kind = voxel::ShapeKind::Sphere;
        op.shape.center = voxel::Vec3d{8.0 + (index % 6) * 8.0, 32.0 - (index / 36) * 6.0, 8.0 + (index / 6 % 6) * 8.0};
        op.shape.radius = 3.0 + (index % 3);
        return {op};
    }

    // what the editor's Platform says of a project: relative asset paths are below its assets folder
    class ProjectAssetsSystem : public System {
    public:
        std::string assets;
        std::string getAssetPath() override { return assets; }
        int getScreenWidth() override { return 1280; }
        int getScreenHeight() override { return 720; }
    };

    class VoxelSceneTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"voxel-scene"};
        ProjectAssetsSystem assetsSystem;
        System* previousSystem = nullptr;
        std::unique_ptr<Project> project;
        SceneProject* sceneProject = nullptr;
        std::shared_ptr<VoxelSystem> system;
        std::shared_ptr<VoxelAsset> asset;
        Entity owner = NULL_ENTITY;
        const std::string assetPath = "voxels/cave.voxelvolume";

        void SetUp() override {
            // a level below the scratch directory: a project directly in the temp directory is the
            // editor's own temporary one, which Auto Save skips
            const fs::path projectDir = dir.path() / "project";
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", projectDir,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(projectDir, false));
            assetsSystem.assets = project->getAssetsPath().string();
            previousSystem = SystemTestAccess::swap(&assetsSystem);
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") sceneProject = &candidate;
            }
            ASSERT_NE(sceneProject, nullptr);
            project->setSelectedSceneId(sceneProject->id);
            system = sceneProject->scene->getSystem<VoxelSystem>();
            ASSERT_NE(system, nullptr);
        }

        void TearDown() override {
            system.reset();
            asset.reset();
            if (project) {
                for (SceneProject& candidate : project->getScenes()) CommandHandle::remove(candidate.id);
                project->releaseScenes();
            }
            project.reset();
            SystemTestAccess::swap(previousSystem);
        }

        fs::path assetFile() const { return project->getAssetsPath() / assetPath; }

        // the volume, saved, on a new GameObject, and known to the system
        void addVolume() {
            asset = VoxelAssets::create(assetPath, smallDesc(), groundGenerator(), 1);
            AssetRef ref;
            std::string error;
            ASSERT_TRUE(saveNewVoxelAsset(*project, *asset, ref, &error)) << error;
            Scene* scene = sceneProject->scene;
            owner = scene->createEntity();
            scene->addComponent<Transform>(owner, {});
            scene->setEntityName(owner, "Cave");
            VoxelVolumeComponent component;
            component.volumeData = ref;
            component.collision = false;
            component.navigationUpdate = VoxelNavigationUpdate::Off;
            scene->addComponent<VoxelVolumeComponent>(owner, component);
            sceneProject->entities.push_back(owner);
            system->update(0.0);
            ASSERT_EQ(system->getAsset(owner), asset);
        }

        std::vector<voxel::SlotChange> edit(int index) {
            const std::vector<voxel::EditOp> ops = digAt(index);
            voxel::EditOptions options;
            options.captureDelta = true;
            const voxel::EditResult result = system->edit(owner, ops, options);
            EXPECT_TRUE(result.ok);
            return result.delta;
        }

        uint64_t hash() { return voxel::contentHash(*asset->store); }
    };

}
