// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A scene's start in every build is its cooked load (S98 REQ-39): 2,000 GameObjects (meshes,
// sprites, lights, bodies, Lua and C++ scripts) load in at most 60 ms on a Release host. The
// budget is printed, and enforced where JASPER_TEST_PERF_SLACK says the machine is one
// (editor::test::perfSlack). The measurement against the generated create_<scene>() it replaced
// is in docs/specs/S98-editor-tooling-gaps.md, Progress.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "Scene.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/ScriptComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "cook/SceneCooker.h"
#include "serialization/CookedScene.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // REQ-39's fixture, as the MCP tools make one (create_entity cube, sprite, point_light, rigidbody;
    // attach_script): 700 meshes, 400 sprites, 200 lights, 300 bodies, 250 Lua and 150 C++ scripts
    std::vector<Entity> twoThousand(Scene& scene) {
        std::vector<Entity> entities;
        auto make = [&](const std::string& name, int n) {
            const Entity entity = scene.createUserEntity();
            scene.setEntityName(entity, name);
            Transform transform;
            transform.position = Vector3(static_cast<float>(n % 50), static_cast<float>((n / 50) % 40), 0.0f);
            scene.addComponent<Transform>(entity, transform);
            entities.push_back(entity);
            return entity;
        };
        int n = 0;
        for (int i = 0; i < 700; i++, n++) scene.addComponent<MeshComponent>(make("cube" + std::to_string(i), n), MeshComponent{});
        for (int i = 0; i < 400; i++, n++) {
            const Entity entity = make("sprite" + std::to_string(i), n);
            scene.addComponent<MeshComponent>(entity, MeshComponent{});
            SpriteComponent sprite;
            sprite.width = 32;
            sprite.height = 32;
            scene.addComponent<SpriteComponent>(entity, sprite);
        }
        for (int i = 0; i < 200; i++, n++) {
            LightComponent light;
            light.type = LightType::POINT;
            light.range = 8.0f;
            scene.addComponent<LightComponent>(make("light" + std::to_string(i), n), light);
        }
        for (int i = 0; i < 300; i++, n++) {
            const Entity entity = make("body" + std::to_string(i), n);
            scene.addComponent<MeshComponent>(entity, MeshComponent{});
            RigidbodyComponent body;
            body.colliders.resize(1);
            scene.addComponent<RigidbodyComponent>(entity, body);
        }
        for (int i = 0; i < 400; i++, n++) {
            const Entity entity = make((i < 250 ? "lua" : "cpp") + std::to_string(i), n);
            scene.addComponent<MeshComponent>(entity, MeshComponent{});
            ScriptComponent scripts;
            ScriptEntry entry;
            entry.type = i < 250 ? ScriptType::LUA : ScriptType::CPP;
            entry.path = i < 250 ? "Spin.lua" : "scripts/Mover.h";
            entry.className = i < 250 ? "Spin" : "Mover";
            ScriptProperty speed;
            speed.name = "speed";
            speed.type = ScriptPropertyType::Float;
            speed.value = 1.0f;
            entry.properties.push_back(speed);
            scripts.scripts.push_back(entry);
            scene.addComponent<ScriptComponent>(entity, scripts);
        }
        return entities;
    }

}

TEST(CookedSceneBench, LoadsTwoThousandGameObjects) {
    Scene source;
    const std::vector<Entity> entities = twoThousand(source);
    ASSERT_EQ(entities.size(), 2000u);
    const std::vector<uint8_t> bytes = editor::cook::SceneCooker::cookScene(&source, 42, entities, NULL_ENTITY, SceneResources{});

    std::vector<double> samples;
    for (int round = 0; round < 7; round++) {
        Scene loaded;
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = CookedSceneLoader::loadScene(&loaded, std::span<const uint8_t>(bytes.data(), bytes.size()), &error);
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        ASSERT_TRUE(ok) << error;
        ASSERT_EQ(loaded.getEntityList().size(), source.getEntityList().size());
    }
    std::sort(samples.begin(), samples.end());
    const double best = samples.front();
    constexpr double budgetMs = 60.0;
    std::printf("[ bench ] cooked load of 2,000 GameObjects (%zu bytes): best %.2f ms, median %.2f ms (budget %.1f)\n", bytes.size(), best,
                samples[samples.size() / 2], budgetMs);
    const double slack = editor::test::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(best, budgetMs * slack);
    }
}
