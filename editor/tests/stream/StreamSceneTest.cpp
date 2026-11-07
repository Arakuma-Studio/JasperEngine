// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Scenes through Stream: the scene settings, entities with their components, and every
// .scene file of editor/tests/fixtures/scenes (copies of the tools/fixtures projects' scenes),
// which must load without an engine error and save back to the same YAML.

#include "component/MeshComponent.h"
#include "Project.h"
#include "component/LightComponent.h"
#include "component/Transform.h"
#include "Stream.h"

#include "LogCapture.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    std::string dumpScene(Scene* scene){
        return YAML::Dump(Stream::encodeScene(scene));
    }

    // a SceneProject (an editor scene) loaded from YAML, as Project::loadScene does it
    struct LoadedScene {
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();
        SceneProject project;

        explicit LoadedScene(const YAML::Node& node){
            project.scene = scene.get();
            Stream::decodeSceneProject(&project, node, true);
            Stream::decodeSceneProjectEntities(nullptr, &project, node);
        }
        ~LoadedScene(){
            project.scene = nullptr;
        }

        std::string dump(){
            return YAML::Dump(Stream::encodeSceneProject(nullptr, &project));
        }
    };

    std::vector<std::string> fixtureScenes(){
        std::vector<std::string> names;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(JASPER_TEST_SOURCE_DIR) / "fixtures" / "scenes", error)){
            if (entry.path().extension() == ".scene")
                names.push_back(entry.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

}

TEST(StreamScene, DefaultSceneSettingsRoundTrip){
    Scene scene;
    Scene read;
    Stream::decodeScene(&read, YAML::Load(dumpScene(&scene)));
    EXPECT_EQ(dumpScene(&read), dumpScene(&scene));
}

TEST(StreamScene, ChangedSceneSettingsRoundTrip){
    Scene scene;
    scene.setBackgroundColor(0.1f, 0.2f, 0.3f, 1.0f);
    scene.setLightState(LightState::ON);
    scene.setGravity3D(0.0f, -3.5f, 0.0f);
    Scene read;
    Stream::decodeScene(&read, YAML::Load(dumpScene(&scene)));
    EXPECT_EQ(read.getLightState(), LightState::ON);
    EXPECT_EQ(read.getGravity3D(), Vector3(0, -3.5f, 0));
    EXPECT_EQ(dumpScene(&read), dumpScene(&scene));
}

// water planar reflections (water-reflections spec 6.4): written only away from their defaults,
// read back; a scene without them takes the defaults
TEST(StreamScene, PlanarReflectionSettings){
    Scene plain;
    const std::string plainYaml = dumpScene(&plain);
    EXPECT_EQ(plainYaml.find("planarReflection"), std::string::npos) << "defaults are not written";
    EXPECT_EQ(plainYaml.find("maxPlanarReflections"), std::string::npos);

    Scene scene;
    scene.setPlanarReflectionResolution(PlanarReflectionResolution::QUARTER);
    scene.setPlanarReflectionLayers(0x5u);
    scene.setPlanarReflectionShadows(false);
    scene.setPlanarReflectionFarClip(120.0f);
    scene.setPlanarReflectionClipOffset(-0.02f);
    scene.setMaxPlanarReflections(3);
    Scene read;
    Stream::decodeScene(&read, YAML::Load(dumpScene(&scene)));
    EXPECT_EQ(read.getPlanarReflectionResolution(), PlanarReflectionResolution::QUARTER);
    EXPECT_EQ(read.getPlanarReflectionLayers(), 0x5u);
    EXPECT_FALSE(read.getPlanarReflectionShadows());
    EXPECT_FLOAT_EQ(read.getPlanarReflectionFarClip(), 120.0f);
    EXPECT_FLOAT_EQ(read.getPlanarReflectionClipOffset(), -0.02f);
    EXPECT_EQ(read.getMaxPlanarReflections(), 3);
    EXPECT_EQ(dumpScene(&read), dumpScene(&scene));

    // an old scene (none of the keys) over a changed one: the defaults
    Stream::decodeScene(&read, YAML::Load(plainYaml));
    const SceneSettings defaults;
    EXPECT_EQ(read.getPlanarReflectionResolution(), defaults.planarReflectionResolution);
    EXPECT_EQ(read.getPlanarReflectionLayers(), defaults.planarReflectionLayers);
    EXPECT_EQ(read.getPlanarReflectionShadows(), defaults.planarReflectionShadows);
    EXPECT_EQ(read.getMaxPlanarReflections(), defaults.maxPlanarReflections);

    // the setters keep values in range
    scene.setMaxPlanarReflections(9);
    EXPECT_EQ(scene.getMaxPlanarReflections(), 4);
    scene.setPlanarReflectionFarClip(-5.0f);
    EXPECT_FLOAT_EQ(scene.getPlanarReflectionFarClip(), 0.0f);
}

TEST(StreamScene, DecodeSceneWithoutASceneMakesOne){
    Scene source;
    std::unique_ptr<Scene> made(Stream::decodeScene(nullptr, Stream::encodeScene(&source)));
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(dumpScene(made.get()), dumpScene(&source));
}

// A Play snapshot put back (Stream::decodeEntity's keepSameMeshes, docs/specs/S71-mesh-update-flags.md)
namespace stream_scene_snapshot_test {

    struct Played {
        Scene scene;
        Entity entity = NULL_ENTITY;
        YAML::Node snapshot;

        Played() {
            entity = scene.createUserEntity();
            scene.setEntityName(entity, "Box");
            scene.addComponent<Transform>(entity, Transform{});
            MeshComponent mesh;
            mesh.worldAABB = AABB(Vector3(-1, -1, -1), Vector3(1, 1, 1));
            scene.addComponent<MeshComponent>(entity, mesh);
            snapshot = YAML::Load(YAML::Dump(Stream::encodeEntity(entity, &scene)));
            // what a Play leaves: moved, its bounds derived from that, and already loaded
            scene.getComponent<Transform>(entity).position = Vector3(5, 5, 5);
            scene.getComponent<MeshComponent>(entity).worldAABB = AABB(Vector3(4, 4, 4), Vector3(6, 6, 6));
            scene.getComponent<MeshComponent>(entity).needReload = false;
        }

        void restore(bool keepSameMeshes) {
            Stream::decodeEntity(snapshot, &scene, nullptr, nullptr, nullptr, NULL_ENTITY, false, true, nullptr, keepSameMeshes);
        }
    };

}

TEST(StreamScene, SnapshotKeepsAMeshThatOnlyMoved){
    stream_scene_snapshot_test::Played played;
    played.restore(true);
    const MeshComponent& mesh = played.scene.getComponent<MeshComponent>(played.entity);
    EXPECT_EQ(played.scene.getComponent<Transform>(played.entity).position, Vector3(0, 0, 0));
    EXPECT_TRUE(played.scene.getComponent<Transform>(played.entity).needUpdate);
    EXPECT_FALSE(mesh.needReload);
    EXPECT_EQ(YAML::Dump(Stream::encodeAABB(mesh.worldAABB)), YAML::Dump(played.snapshot["components"]["mesh"]["worldAABB"]));
}

TEST(StreamScene, SnapshotDecodesAMeshAPlayChanged){
    stream_scene_snapshot_test::Played played;
    played.scene.getComponent<MeshComponent>(played.entity).castShadows = false;
    played.restore(true);
    const MeshComponent& mesh = played.scene.getComponent<MeshComponent>(played.entity);
    EXPECT_TRUE(mesh.castShadows);
    EXPECT_TRUE(mesh.needReload);
    EXPECT_EQ(YAML::Dump(Stream::encodeAABB(mesh.worldAABB)), YAML::Dump(played.snapshot["components"]["mesh"]["worldAABB"]));
}

// what the file held that this editor does not read stays, with the decode that keeps it
TEST(StreamScene, SnapshotDecodesAMeshWithKeysItDoesNotEncode){
    stream_scene_snapshot_test::Played played;
    played.snapshot["components"]["mesh"]["fromALaterEditor"] = 1;
    played.restore(true);
    EXPECT_TRUE(played.scene.getComponent<MeshComponent>(played.entity).needReload);
    EXPECT_TRUE(Stream::encodeEntity(played.entity, &played.scene)["components"]["mesh"]["fromALaterEditor"]);
    EXPECT_EQ(Stream::encodeEntity(played.entity, &played.scene)["components"]["mesh"]["fromALaterEditor"].as<int>(), 1);
}

// undo, prefab propagation and every other decode over a live Mesh: as ever
TEST(StreamScene, OtherDecodesReloadTheMeshTheyDecode){
    stream_scene_snapshot_test::Played played;
    played.restore(false);
    EXPECT_TRUE(played.scene.getComponent<MeshComponent>(played.entity).needReload);
}

TEST(StreamScene, EntityWithComponentsRoundTrips){
    Scene scene;
    const Entity entity = scene.createEntity();
    scene.setEntityName(entity, "Lamp");
    scene.addComponent<Transform>(entity, Transform{});
    LightComponent light;
    light.type = LightType::POINT;
    light.color = Vector3(1, 0.8f, 0.6f);
    scene.addComponent<LightComponent>(entity, light);
    const YAML::Node first = Stream::encodeEntity(entity, &scene);

    Scene other;
    const std::vector<Entity> read = Stream::decodeEntity(YAML::Load(YAML::Dump(first)), &other);
    ASSERT_EQ(read.size(), 1u);
    EXPECT_EQ(read[0], entity);
    EXPECT_EQ(other.getEntityName(entity), "Lamp");
    EXPECT_EQ(other.getComponent<LightComponent>(entity).type, LightType::POINT);
    EXPECT_EQ(YAML::Dump(Stream::encodeEntity(read[0], &other)), YAML::Dump(first));
}

TEST(StreamScene, ChildrenAreWrittenUnderTheirParent){
    Scene scene;
    const Entity parent = scene.createEntity();
    const Entity child = scene.createEntity();
    scene.addComponent<Transform>(parent, Transform{});
    scene.addComponent<Transform>(child, Transform{});
    scene.addEntityChild(parent, child, false);
    const YAML::Node node = Stream::encodeEntity(parent, &scene);
    ASSERT_TRUE(node["children"]);
    EXPECT_EQ(node["children"].size(), 1u);

    Scene other;
    const std::vector<Entity> read = Stream::decodeEntity(node, &other);
    EXPECT_EQ(read.size(), 2u);
    EXPECT_EQ(other.getComponent<Transform>(child).parent, parent);
}

TEST(StreamScene, DecodingATakenIdMakesANewEntity){
    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity, Transform{});
    const YAML::Node node = Stream::encodeEntity(entity, &scene);
    const std::vector<Entity> copy = Stream::decodeEntity(node, &scene);
    ASSERT_EQ(copy.size(), 1u);
    EXPECT_NE(copy[0], entity);
    EXPECT_TRUE(scene.isEntityCreated(copy[0]));
}

TEST(StreamScene, SceneProjectRoundTrips){
    Scene scene;
    SceneProject project;
    project.id = 7;
    project.name = "Level";
    project.scene = &scene;
    project.sceneType = SceneType::SCENE_2D;
    const Entity entity = scene.createEntity();
    scene.setEntityName(entity, "Player");
    scene.addComponent<Transform>(entity, Transform{});
    project.entities.push_back(entity);
    const std::string first = YAML::Dump(Stream::encodeSceneProject(nullptr, &project));
    project.scene = nullptr;

    LoadedScene loaded(YAML::Load(first));
    EXPECT_EQ(loaded.project.id, 7u);
    EXPECT_EQ(loaded.project.name, "Level");
    EXPECT_EQ(loaded.project.sceneType, SceneType::SCENE_2D);
    EXPECT_EQ(loaded.project.entities, std::vector<Entity>{entity});
    EXPECT_EQ(loaded.dump(), first);
}

class StreamFixtureScene : public ::testing::TestWithParam<std::string> {};

TEST_P(StreamFixtureScene, LoadsWithoutErrorsAndSavesTheSameYaml){
    const std::filesystem::path path = std::filesystem::path(JASPER_TEST_SOURCE_DIR) / "fixtures" / "scenes" / GetParam();
    const YAML::Node file = YAML::LoadFile(path.string());
    LoadedScene first(file);
    EXPECT_FALSE(first.project.entities.empty());
    EXPECT_NO_ENGINE_ERRORS();
    const std::string saved = first.dump();

    LoadedScene second(YAML::Load(saved));
    EXPECT_EQ(second.dump(), saved);
    EXPECT_EQ(second.project.entities, first.project.entities);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, StreamFixtureScene, ::testing::ValuesIn(fixtureScenes()),
    [](const ::testing::TestParamInfo<std::string>& info){
        std::string name = info.param.substr(0, info.param.size() - 6);
        for (char& c : name){
            if (!std::isalnum(static_cast<unsigned char>(c)))
                c = '_';
        }
        return name;
    });
