// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BENCH_FOLIAGEBENCHSCENE_H
#define BENCH_FOLIAGEBENCHSCENE_H

// T5 R44's benchmark scene, made by code with no assets: je-bench-foliage's numbers and
// FoliageBudget's counts come from the same one. The meshes are paths that do not load: what is
// measured is placement, culling and the store, and counts do not depend on them.

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "math/Plane.h"
#include "render/FoliageQuality.h"
#include "subsystem/FoliageSystem.h"
#include "terrain/FoliageTypes.h"
#include "terrain/TerrainDeterministic.h"
#include "terrain/TreeStore.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace foliagebench {

    using namespace jasperengine;
    using namespace jasperengine::terrain;

    inline constexpr float WORLD = 1024.0f;
    inline constexpr int HEIGHT_MAP = 1024;
    inline constexpr int DENSITY_MAP = 512;
    inline constexpr int TREES = 50000;
    inline constexpr int FRAMES = 600;
    inline constexpr double FRAME_SECONDS = 1.0 / 60.0;

    inline void giveMap(Texture& texture, const std::string& id, int size, int bits, const std::function<float(int, int)>& value) {
        const int bytesPerChannel = bits / 8;
        const size_t bytes = static_cast<size_t>(size) * size * bytesPerChannel;
        unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes));
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                const float normalized = value(x, y);
                const size_t at = (static_cast<size_t>(y) * size + x) * bytesPerChannel;
                if (bits == 16) {
                    const uint32_t stored = static_cast<uint32_t>(normalized * 65535.0f + 0.5f);
                    raw[at] = static_cast<unsigned char>(stored & 0xFF);
                    raw[at + 1] = static_cast<unsigned char>(stored >> 8);
                } else {
                    raw[at] = static_cast<unsigned char>(normalized * 255.0f + 0.5f);
                }
            }
        }
        TextureData data(size, size, static_cast<unsigned int>(bytes), bits == 16 ? ColorFormat::RED16 : ColorFormat::RED, 1, raw);
        data.setDataOwned(false);
        texture.setData(id, data);
        texture.getData().setDataOwned(true);
    }

    // hash fractal: the height of R44
    inline float fractalHeight(int x, int y) {
        float value = 0.0f;
        float amplitude = 0.5f;
        int cell = 128;
        for (int octave = 0; octave < 5; octave++) {
            const float fx = static_cast<float>(x % cell) / static_cast<float>(cell);
            const float fy = static_cast<float>(y % cell) / static_cast<float>(cell);
            const int cx = x / cell;
            const int cy = y / cell;
            const float a = detHash01(cx, cy, 11u + static_cast<uint32_t>(octave));
            const float b = detHash01(cx + 1, cy, 11u + static_cast<uint32_t>(octave));
            const float c = detHash01(cx, cy + 1, 11u + static_cast<uint32_t>(octave));
            const float d = detHash01(cx + 1, cy + 1, 11u + static_cast<uint32_t>(octave));
            value += amplitude * (a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy);
            amplitude *= 0.5f;
            cell /= 2;
        }
        return value;
    }

    struct Scene1 {
        std::unique_ptr<Scene> scene;
        Camera* camera = nullptr;
        Entity entity = NULL_ENTITY;
        FoliageSystem* foliage = nullptr;

        explicit Scene1(const std::string& name) {
            scene = std::make_unique<Scene>();
            camera = new Camera(scene.get());
            scene->setCamera(camera->getEntity());
            setPose(0);

            entity = scene->createEntity();
            scene->addComponent<Transform>(entity);
            scene->addComponent<TerrainComponent>(entity);
            TerrainComponent& terrain = component();
            terrain.terrainSize = WORLD;
            terrain.maxHeight = 60.0f;
            terrain.detailDistance = 120.0f;
            terrain.treeDistance = 1000.0f;
            terrain.billboardStart = 80.0f;
            giveMap(terrain.heightMap, name + "-height", HEIGHT_MAP, 16, fractalHeight);
            terrain.heightMapLoaded = true;
            terrain.needUpdateTerrain = false;

            const float densities[4] = {4.0f, 2.0f, 1.0f, 0.5f};
            for (int i = 0; i < 4; i++) {
                TerrainFoliageLayer layer;
                layer.meshPath = AssetRef("t5-bench-detail-" + std::to_string(i) + ".obj");
                layer.density = densities[i];
                layer.maxSlope = 60.0f;
                layer.drawDistance = 120.0f;
                layer.seed = static_cast<unsigned int>(100 + i);
                giveMap(layer.densityMap, name + "-density-" + std::to_string(i), DENSITY_MAP, 8, [i](int x, int y) {
                    return 0.5f + 0.5f * detHash01(x / 16, y / 16, 40u + static_cast<uint32_t>(i));
                });
                terrain.foliageLayers.push_back(layer);
            }
            for (unsigned int i = 0; i < 3; i++) {
                TerrainTreePrototype prototype;
                prototype.id = i + 1;
                prototype.meshPath = AssetRef("t5-bench-tree-" + std::to_string(i) + ".obj");
                terrain.treePrototypes.push_back(prototype);
            }
            foliage = scene->getSystem<FoliageSystem>().get();
            frame(0.0);
            plant();
        }

        ~Scene1() { delete camera; }

        TerrainComponent& component() { return scene->getComponent<TerrainComponent>(entity); }

        void frame(double dt) {
            component().heightMapLoaded = true;
            component().needUpdateTerrain = false;
            foliage->update(dt);
        }

        void plant() {
            std::shared_ptr<TreeStore> store = foliage->getOrCreateTreeStore(entity);
            std::vector<TreeRecord> added;
            added.reserve(TREES);
            for (int i = 0; i < TREES; i++) {
                TreeRecord record{};
                record.u = detHash01(i, 0, 71u);
                record.v = detHash01(i, 1, 71u);
                record.scale = 1.0f;
                record.rotation[3] = 32767;
                record.prototypeId = static_cast<uint16_t>(1 + i % 3);
                record.heightRatio = 128;
                record.flags = TREE_SNAP_TO_TERRAIN;
                std::memset(record.color, 255, 4);
                added.push_back(record);
            }
            TreeDelta delta;
            store->apply({}, added, {}, &delta);
            foliage->treesChanged(entity, delta);
        }

        void settle() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
            do {
                frame(FRAME_SECONDS);
                foliage->waitForJobs();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (foliage->hasPendingUpdates() && std::chrono::steady_clock::now() < deadline);
        }

        // R44's path: 5 units a second, turning 30 degrees a second
        void setPose(int index) {
            const double seconds = index * FRAME_SECONDS;
            const float yaw = static_cast<float>(seconds * 30.0 * 3.14159265358979 / 180.0);
            Transform& transform = scene->getComponent<Transform>(camera->getEntity());
            transform.worldPosition = Vector3(static_cast<float>(seconds * 5.0), 40.0f, 0.0f);
            forward = Vector3(std::sin(yaw), -0.1f, std::cos(yaw));
        }

        Vector3 forward = Vector3(0.0f, -0.1f, 1.0f);

        // A 60 degree frustum of the camera's pose
        FoliageViewRequest view(Plane planes[6]) {
            Transform& transform = scene->getComponent<Transform>(camera->getEntity());
            const Vector3 eye = transform.worldPosition;
            const Vector3 f = forward.normalized();
            const Vector3 right = Vector3(0.0f, 1.0f, 0.0f).crossProduct(f).normalized();
            const Vector3 up = f.crossProduct(right);
            const float tangent = std::tan(30.0f * 3.14159265f / 180.0f);
            const float aspect = 16.0f / 9.0f;
            const Vector3 sides[4] = {
                (right * -1.0f + f * (tangent * aspect)),
                (right + f * (tangent * aspect)),
                (up * -1.0f + f * tangent),
                (up + f * tangent)
            };
            for (int i = 0; i < 4; i++) {
                const Vector3 normal = sides[i].normalized();
                planes[i] = Plane(normal.x, normal.y, normal.z, -normal.dotProduct(eye));
            }
            planes[4] = Plane(f.x, f.y, f.z, -f.dotProduct(eye));
            planes[5] = Plane(-f.x, -f.y, -f.z, f.dotProduct(eye) + 1200.0f);

            FoliageViewRequest request;
            request.planes = planes;
            request.farClip = 1200.0f;
            request.eye = eye;
            request.main = true;
            return request;
        }
    };

}

#endif // BENCH_FOLIAGEBENCHSCENE_H
