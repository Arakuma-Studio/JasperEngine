// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A Project (a copy of the prefab-runtime fixture) with a terrain whose maps are real files: what the
// tests of the editor's side of the terrain engine share (docs/specs/T1-terrain-authoring.md)

#include "Project.h"
#include "command/CommandHandle.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/TerrainEdit.h"
#include "pool/TextureDataPool.h"
#include "support/TestTempDir.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/OverlayMapStore.h"
#include "terrain/TerrainMapPersistence.h"
#include "terrain/TerrainOps.h"
#include "texture/TextureData.h"
#include "util/TerrainMapUtils.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace terrainprojecttest {

    using namespace jasperengine;
    using namespace jasperengine::editor;
    namespace fs = std::filesystem;

    inline constexpr terrain::MapId HEIGHT_MAP{terrain::MapKind::Height, 0};
    inline constexpr terrain::MapId BLEND_MAP{terrain::MapKind::Blend, 0};
    inline constexpr terrain::MapId DENSITY_MAP{terrain::MapKind::Density, 0};

    class TerrainProjectFixture : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"terrain-project"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        Entity terrainEntity = NULL_ENTITY;
        std::vector<std::string> madeIds;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) {
                    project->loadScene(candidate.filepath, false, false, true);
                }
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            terrainEntity = addTerrain();
            terrain().foliageLayers.resize(1);
        }

        void TearDown() override {
            TerrainMapPersistence::get().flushAll();
            if (project) {
                for (SceneProject& candidate : project->getScenes()) {
                    CommandHandle::remove(candidate.id);
                }
                project->releaseScenes();
            }
            project.reset();
            for (const std::string& id : madeIds) {
                TextureDataPool::remove(id);
            }
        }

        Entity addTerrain() {
            Scene* s = scene->scene;
            const Entity entity = s->createEntity();
            s->addComponent<Transform>(entity, {});
            TerrainComponent component;
            component.terrainSize = 128.0f;
            component.maxHeight = 20.0f;
            component.needUpdateTerrain = false;
            component.needUpdateFoliage = false;
            component.heightMapLoaded = true;
            s->addComponent<TerrainComponent>(entity, component);
            // listed as the editor lists every GameObject: what a save writes and the map
            // collector keeps
            scene->entities.push_back(entity);
            return entity;
        }

        TerrainComponent& terrain() { return scene->scene->getComponent<TerrainComponent>(terrainEntity); }
        CommandHistory* history() { return CommandHandle::get(scene->id); }

        // A map bound to a file of this terrain's own, its pixels set by value(byteIndex)
        template<typename F>
        void giveFileMap(const TerrainMapRef& ref, int size, ColorFormat format, int channels, F&& value, Entity entity = NULL_ENTITY) {
            const Entity owner = entity == NULL_ENTITY ? terrainEntity : entity;
            TerrainComponent& component = scene->scene->getComponent<TerrainComponent>(owner);
            TerrainMapUtils::ensureMapSlot(component, ref);
            Texture* texture = TerrainMapUtils::findTexture(component, ref);
            ASSERT_NE(texture, nullptr);
            std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * channels * TextureData::getBytesPerChannel(format));
            for (size_t i = 0; i < pixels.size(); i++) {
                pixels[i] = static_cast<unsigned char>(value(i));
            }
            const std::string path = TerrainMapUtils::makeEditableTexturePath(project.get(), scene->id, owner, ref);
            ASSERT_TRUE(TerrainMapUtils::setFileBackedTextureData(project.get(), *texture, path, size, size, format, channels, pixels));
            madeIds.push_back(texture->getId());
        }

        void giveHeight16(int size, unsigned char fill = 0x40) {
            giveFileMap(TerrainMapRef(TerrainMapTarget::HeightMap), size, ColorFormat::RED16, 1, [&](size_t) { return fill; });
        }

        std::vector<unsigned char> bytesOf(terrain::MapId map) {
            Texture* texture = terrainMapTexture(terrain(), map);
            EXPECT_NE(texture, nullptr);
            EXPECT_TRUE(TerrainMapUtils::hasLoadedData(*texture));
            TextureData& data = texture->getData();
            const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
            return std::vector<unsigned char>(pixels, pixels + data.getSize());
        }

        fs::path fileOf(terrain::MapId map) {
            return project->resolveAssetPath(terrainMapTexture(terrain(), map)->getPath(0));
        }

        // The whole pending state on disk, as a forced flush leaves it
        void settleFiles() {
            EXPECT_TRUE(TerrainMapPersistence::get().flushAll());
        }
    };

    inline terrain::Op raiseOp(float delta) {
        terrain::Op op;
        op.type = terrain::OpType::Raise;
        op.delta = delta;
        op.shape.type = terrain::ShapeType::Circle;
        op.shape.center = Vector2(0.0f, 0.0f);
        op.shape.radius = 10.0f;
        return op;
    }

    inline terrain::TerrainOpResult runOn(terrain::TerrainMapStore& store, const terrain::Op& op) {
        terrain::OpOptions options;
        options.inPlace = true;
        return terrain::applyOps(store, std::span<const terrain::Op>(&op, 1), options);
    }

}
