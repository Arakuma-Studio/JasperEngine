// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A loaded project with primitive meshes in its scene: what the V1 editor tests drive
// (docs/specs/V1-vertex-colors.md). Each test gets its own copy of tools/fixtures/prefab-runtime.

#include "Project.h"
#include "System.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/VertexColorsComponent.h"
#include "object/Primitive.h"
#include "subsystem/VertexColorScene.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"
#include "vertexcolor/VertexColorEditing.h"
#include "vertexcolor/VertexColorProject.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

namespace jasperengine::editor::vertexcolortest {

    namespace fs = std::filesystem;
    namespace vc = jasperengine::vertexcolor;
    namespace vce = jasperengine::editor::vertexcolor;

    class VertexColorSceneTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"vertex-colors-scene"};
        std::unique_ptr<Project> project;
        SceneProject* sceneProject = nullptr;
        uint32_t sceneId = 0;
        CommandHistory* history = nullptr;
        // the objects own their entities: kept until the test ends (the scene goes first)
        std::vector<std::unique_ptr<EntityHandle>> objects;

        void SetUp() override {
            // a level below the scratch directory: one directly in the temp directory is the editor's
            // own temporary project, which Auto Save skips
            const fs::path projectDir = dir.path() / "project";
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", projectDir,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            test::useProjectPaths(project.get());
            ASSERT_TRUE(project->loadProject(projectDir, false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") sceneProject = &candidate;
            }
            ASSERT_NE(sceneProject, nullptr);
            sceneId = sceneProject->id;
            project->setSelectedSceneId(sceneId);
            history = CommandHandle::get(sceneId);
            history->clear();
        }

        void TearDown() override {
            releaseVertexColorFiles();
            if (project) test::closeProject(*project);
            // useProjectPaths registered it to be released when the tests end: it lives until then
            (void)project.release();
            objects.clear();
        }

        Scene* scene() const { return sceneProject->scene; }

        Entity track(std::unique_ptr<EntityHandle> object, const std::string& name) {
            const Entity entity = object->getEntity();
            scene()->setEntityName(entity, name);
            sceneProject->entities.push_back(entity);
            objects.push_back(std::move(object));
            return entity;
        }

        Entity addBox(const std::string& name = "Box") {
            auto box = std::make_unique<Primitive>(scene());
            box->createBox(1.0f, 1.0f, 1.0f);
            return track(std::move(box), name);
        }

        Entity addSphere(const std::string& name = "Ball") {
            auto sphere = std::make_unique<Primitive>(scene());
            sphere->createSphere(1.0f, 48, 24);
            return track(std::move(sphere), name);
        }

        Entity addGrid(const std::string& name, unsigned int segments) {
            auto grid = std::make_unique<Primitive>(scene());
            grid->createGrid(2.0f, 2.0f, segments);
            return track(std::move(grid), name);
        }

        vc::TargetKey firstKey(Entity entity) {
            const std::vector<vce::TargetInfo> targets = vce::listTargets(project.get(), sceneId, entity);
            EXPECT_FALSE(targets.empty());
            return targets.empty() ? vc::TargetKey() : targets.front().key;
        }

        static vc::Op fillAll(const Vector4& value, vc::ChannelMask channels = vc::CHANNELS_RGBA) {
            vc::Op op;
            op.type = vc::OpType::Fill;
            op.target.kind = vc::TargetKind::All;
            op.value = value;
            op.channels = channels;
            return op;
        }

        vce::EditResult edit(Entity entity, std::vector<vc::Op> ops, const std::string& name = "Apply Color", bool dryRun = false) {
            vce::EditRequest request;
            request.targets.push_back({entity, firstKey(entity)});
            request.ops = std::move(ops);
            request.undoName = name;
            request.dryRun = dryRun;
            return vce::applyEdit(project.get(), sceneId, request);
        }

        uint64_t hashOf(Entity entity) {
            const vc::TargetData* data = vce::targetData(project.get(), sceneId, entity, firstKey(entity));
            return data ? vc::hashTarget(*data) : 0;
        }

        VertexColorsComponent* colorsOf(Entity entity) { return scene()->findComponent<VertexColorsComponent>(entity); }

        size_t steps() const { return history->appliedCount(); }
    };

}
