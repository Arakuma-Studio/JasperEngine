// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_FOLIAGE_SUPPORT_H
#define TESTS_FOLIAGE_SUPPORT_H

// A Terrain with foliage in memory (T5's tests). Its meshes are paths that do not load, since
// placement does not wait for them.

#include "Engine.h"
#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/TerrainEdit.h"
#include "subsystem/FoliageSystem.h"
#include "terrain/FoliageRules.h"
#include "terrain/TerrainDeterministic.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace foliagetest {

    using namespace jasperengine;
    using namespace jasperengine::terrain;

    constexpr int MAP = 64;
    constexpr float WORLD = 128.0f;

    inline void giveMap(Texture& texture, const std::string& id, int bits, const std::function<float(int, int)>& value) {
        const int bytesPerChannel = bits / 8;
        const size_t bytes = static_cast<size_t>(MAP) * MAP * bytesPerChannel;
        unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes));
        for (int y = 0; y < MAP; y++) {
            for (int x = 0; x < MAP; x++) {
                const float normalized = value(x, y);
                const size_t at = (static_cast<size_t>(y) * MAP + x) * bytesPerChannel;
                if (bits == 16) {
                    const uint32_t stored = static_cast<uint32_t>(normalized * 65535.0f + 0.5f);
                    raw[at] = static_cast<unsigned char>(stored & 0xFF);
                    raw[at + 1] = static_cast<unsigned char>(stored >> 8);
                } else {
                    raw[at] = static_cast<unsigned char>(normalized * 255.0f + 0.5f);
                }
            }
        }
        TextureData data(MAP, MAP, static_cast<unsigned int>(bytes), bits == 16 ? ColorFormat::RED16 : ColorFormat::RED, 1, raw);
        data.setDataOwned(false);
        texture.setData(id, data);
        texture.getData().setDataOwned(true);
    }

    // a unique id per map: the texture pool keeps them by it
    inline int nextFixture = 0;

    struct FoliageFixture {
        std::unique_ptr<Scene> scene;
        Camera* camera = nullptr;
        Entity entity = NULL_ENTITY;
        FoliageSystem* foliage = nullptr;
        std::string name;

        explicit FoliageFixture(float drawDistance = 400.0f, float cameraX = 0.0f, float cameraZ = 0.0f) {
            name = "t5-core-" + std::to_string(nextFixture++);
            scene = std::make_unique<Scene>();
            camera = new Camera(scene.get());
            scene->setCamera(camera->getEntity());
            setCamera(cameraX, cameraZ);

            entity = scene->createEntity();
            scene->addComponent<Transform>(entity);
            scene->addComponent<TerrainComponent>(entity);
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
            terrain.terrainSize = WORLD;
            terrain.maxHeight = 20.0f;
            giveMap(terrain.heightMap, name + "-height", 16, [](int x, int y) {
                return 0.25f + 0.1f * detHash01(x / 8, y / 8, 3u);
            });
            terrain.heightMapLoaded = true;
            terrain.needUpdateTerrain = false;

            TerrainFoliageLayer layer;
            layer.meshPath = AssetRef("t5-no-such-mesh.obj");
            layer.density = 0.5f;
            layer.maxSlope = 90.0f;
            layer.drawDistance = drawDistance;
            layer.seed = 7;
            giveMap(layer.densityMap, name + "-density", 8, [](int, int) { return 1.0f; });
            terrain.foliageLayers.push_back(layer);

            foliage = scene->getSystem<FoliageSystem>().get();
        }

        ~FoliageFixture() {
            delete camera;
        }

        TerrainComponent& terrain() { return scene->getComponent<TerrainComponent>(entity); }

        void setCamera(float x, float z) {
            Transform& transform = scene->getComponent<Transform>(camera->getEntity());
            transform.worldPosition = Vector3(x, 10.0f, z);
        }

        // what a frame does with a settled heightmap
        void frame(double dt = 0.016) {
            terrain().heightMapLoaded = true;
            terrain().needUpdateTerrain = false;
            foliage->update(dt);
        }

        void settle(int frames = 2000) {
            for (int i = 0; i < frames; i++) {
                frame();
                if (!foliage->hasPendingUpdates()) return;
                foliage->waitForJobs();
            }
            FAIL() << "foliage did not settle";
        }
    };

    inline TreeRecord treeAt(float x, float z, uint16_t prototype, uint8_t flags = TREE_SNAP_TO_TERRAIN) {
        TreeRecord record{};
        record.u = x / WORLD + 0.5f;
        record.v = z / WORLD + 0.5f;
        record.scale = 1.0f;
        record.rotation[3] = 32767;
        record.prototypeId = prototype;
        record.heightRatio = 128;
        record.flags = flags;
        std::memset(record.color, 255, 4);
        return record;
    }

    // Trees through the store, as a painted Terrain's are
    inline void withTrees(FoliageFixture& fixture, std::initializer_list<std::pair<float, float>> points) {
        TerrainTreePrototype prototype;
        prototype.id = 5;
        prototype.meshPath = AssetRef("t5-no-such-tree.obj");
        fixture.terrain().treePrototypes.push_back(prototype);
        fixture.frame();
        std::shared_ptr<TreeStore> store = fixture.foliage->getOrCreateTreeStore(fixture.entity);
        ASSERT_NE(store, nullptr);
        std::vector<TreeRecord> added;
        for (const auto& point : points) added.push_back(treeAt(point.first, point.second, 5));
        TreeDelta delta;
        store->apply({}, added, {}, &delta);
        fixture.foliage->treesChanged(fixture.entity, delta);
        fixture.frame();
    }


}

#endif // TESTS_FOLIAGE_SUPPORT_H
