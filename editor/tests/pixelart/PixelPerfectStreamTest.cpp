// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC17 (R26, R27): the Pixel Perfect Camera and the Sprite and Tilemap Pixels Per Unit in the
// scene format. No version bump: every key is optional with a default that draws what a scene
// drew before. A scene with the component round-trips, the keys of a later editor under it are
// kept, and a scene without the new keys re-saves byte for byte.

#include "support/EditorTestSupport.h"

#include "App.h"

#include "DocumentFile.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "reflect/Reflect.h"
#include "component/CameraComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    // a scene of one camera GameObject: the components block given (a scene without its
    // settings block gets no Scene to decode into)
    std::string cameraScene(const std::string& components){
        return "formatVersion: 7\n"
               "id: 4141\n"
               "name: PixelPerfect\n"
               "scene: {}\n"
               "sceneType: scene_2d\n"
               "mainCamera: 1001\n"
               "entities:\n"
               "  - type: Entity\n"
               "    entity: 1001\n"
               "    name: Main Camera\n"
               "    components:\n" + components;
    }

    Entity firstEntity(const DecodedScene& decoded){
        return decoded.scene->entities.empty() ? NULL_ENTITY : decoded.scene->entities.front();
    }

}

TEST(PixelPerfectStream, ComponentRoundTrips){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<CameraComponent>(entity, {});
    PixelPerfectCameraComponent authored;
    authored.referenceWidth = 320;
    authored.referenceHeight = 180;
    authored.upscaleMode = PixelUpscaleMode::INTEGER_EXPAND;
    authored.upscaleRenderTexture = false;
    authored.rotationMode = PixelRotation::ON_GRID;
    authored.subpixelSmoothing = true;
    authored.letterboxColor = Vector4(0.1f, 0.2f, 0.3f, 1.0f);
    // runtime values are never written
    authored.layout.scale = 7;
    authored.active = true;
    scene.addComponent<PixelPerfectCameraComponent>(entity, authored);

    const YAML::Node components = Stream::encodeComponents(entity, &scene, scene.getSignature(entity));
    ASSERT_TRUE(components["pixelperfectcamera"].IsDefined());
    EXPECT_FALSE(components["pixelperfectcamera"]["pixelRatio"].IsDefined());
    EXPECT_FALSE(components["pixelperfectcamera"]["active"].IsDefined());

    Scene other;
    const Entity copy = other.createUserEntity();
    Stream::decodeComponents(copy, NULL_ENTITY, &other, YAML::Load(YAML::Dump(components)));
    ASSERT_TRUE(other.findComponent<PixelPerfectCameraComponent>(copy));
    const PixelPerfectCameraComponent& decoded = other.getComponent<PixelPerfectCameraComponent>(copy);
    EXPECT_EQ(decoded.referenceWidth, 320u);
    EXPECT_EQ(decoded.referenceHeight, 180u);
    EXPECT_EQ(decoded.upscaleMode, PixelUpscaleMode::INTEGER_EXPAND);
    EXPECT_FALSE(decoded.upscaleRenderTexture);
    EXPECT_EQ(decoded.rotationMode, PixelRotation::ON_GRID);
    EXPECT_TRUE(decoded.subpixelSmoothing);
    EXPECT_FLOAT_EQ(decoded.letterboxColor.z, 0.3f);
    EXPECT_EQ(decoded.layout.scale, 1);

    // a default component writes only its key (OmitIfDefault)
    Scene plain;
    const Entity plainEntity = plain.createUserEntity();
    plain.addComponent<Transform>(plainEntity, {});
    plain.addComponent<CameraComponent>(plainEntity, {});
    plain.addComponent<PixelPerfectCameraComponent>(plainEntity, {});
    const YAML::Node plainComponents = Stream::encodeComponents(plainEntity, &plain, plain.getSignature(plainEntity));
    ASSERT_TRUE(plainComponents["pixelperfectcamera"].IsDefined());
    EXPECT_FALSE(plainComponents["pixelperfectcamera"]["referenceWidth"].IsDefined());
}

// the keys of a later editor under the component survive a load and a save
TEST(PixelPerfectStream, UnknownKeysSurvive){
    const std::string text = cameraScene(
        "      transform:\n"
        "        position: [0, 0, 1]\n"
        "      camera:\n"
        "        type: camera_ortho\n"
        "      pixelperfectcamera:\n"
        "        referenceWidth: 384\n"
        "        referenceHeight: 216\n"
        "        cropBehaviour: zoom\n");
    DecodedScene decoded = decodeSceneText(text, "pixel.scene");
    const Entity camera = firstEntity(decoded);
    ASSERT_NE(camera, NULL_ENTITY);
    ASSERT_TRUE(decoded.scene->scene->findComponent<PixelPerfectCameraComponent>(camera));
    EXPECT_EQ(decoded.scene->scene->getComponent<PixelPerfectCameraComponent>(camera).referenceWidth, 384u);
    EXPECT_GE(decoded.report.preserved(), 1u);
    const std::string saved = encodeSceneText(decoded);
    EXPECT_NE(saved.find("cropBehaviour: zoom"), std::string::npos) << saved;
    // the spelling of the spec's sketch names the component in MCP and Lua calls; a scene file
    // has the lowercase key of every component
    const reflect::ComponentDesc* aliased = reflect::findByKey("pixelPerfectCamera");
    ASSERT_NE(aliased, nullptr);
    EXPECT_EQ(aliased->type, ComponentType::PixelPerfectCameraComponent);
}

// Sprite and Tilemap Pixels Per Unit and Pixel Rotation: written only away from their defaults
TEST(PixelPerfectStream, SpriteAndTilemapKeys){
    SpriteComponent sprite;
    sprite.width = 16;
    sprite.height = 16;
    YAML::Node node = Stream::encodeSpriteComponent(sprite);
    EXPECT_FALSE(node["pixelsPerUnit"].IsDefined());
    EXPECT_FALSE(node["pixelRotation"].IsDefined());
    sprite.pixelsPerUnit = 16.0f;
    sprite.pixelRotation = PixelRotation::ON_GRID;
    node = Stream::encodeSpriteComponent(sprite);
    const SpriteComponent decoded = Stream::decodeSpriteComponent(YAML::Load(YAML::Dump(node)));
    EXPECT_FLOAT_EQ(decoded.pixelsPerUnit, 16.0f);
    EXPECT_EQ(decoded.pixelRotation, PixelRotation::ON_GRID);

    TilemapComponent tilemap;
    YAML::Node tilemapNode = Stream::encodeTilemapComponent(tilemap);
    EXPECT_FALSE(tilemapNode["pixelsPerUnit"].IsDefined());
    tilemap.pixelsPerUnit = 32.0f;
    tilemapNode = Stream::encodeTilemapComponent(tilemap);
    EXPECT_FLOAT_EQ(Stream::decodeTilemapComponent(YAML::Load(YAML::Dump(tilemapNode))).pixelsPerUnit, 32.0f);
}

// a scene made before S41 (here the data-safety fixture with a sprite-free camera scene, and
// every scene of the reflection parity project in its own test) saves as it was
TEST(PixelPerfectStream, OldSceneSavesAsItWas){
    const std::string text = cameraScene(
        "      transform:\n"
        "        position: [0, 0, 1]\n"
        "      camera:\n"
        "        type: camera_ortho\n");
    DecodedScene decoded = decodeSceneText(text, "old.scene");
    const std::string saved = encodeSceneText(decoded);
    DecodedScene again = decodeSceneText(saved, "old.scene");
    EXPECT_EQ(encodeSceneText(again), saved);
    EXPECT_EQ(saved.find("pixel"), std::string::npos) << saved;
}

// AC17 across the committed fixtures: every scene and project file of tools/fixtures and of the
// editor tests re-saves without S41's keys (they are written only away from their defaults) and
// stably (a second save is the first, byte for byte)
TEST(PixelPerfectStream, FixturesResaveWithoutS41Keys){
    const fs::path roots[] = {fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures", fs::path(JASPER_TEST_FIXTURES_DIR)};
    const char* const s41Keys[] = {"pixelperfectcamera", "pixelsPerUnit", "pixelRotation"};
    size_t scenes = 0, projects = 0;
    for (const fs::path& root : roots){
        ASSERT_TRUE(fs::is_directory(root)) << root;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)){
            if (!entry.is_regular_file())
                continue;
            const fs::path& path = entry.path();
            std::ifstream in(path, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            const std::string source = text.str();
            if (path.extension() == ".scene"){
                SCOPED_TRACE(path.string());
                DecodedScene decoded = decodeSceneFile(path);
                if (!decoded.scene || !decoded.scene->scene)
                    continue;
                const std::string saved = encodeSceneText(decoded);
                for (const char* key : s41Keys){
                    if (source.find(key) == std::string::npos){
                        EXPECT_EQ(saved.find(key), std::string::npos) << key << " written into a scene that had none";
                    }
                }
                DecodedScene again = decodeSceneText(saved, path.filename().string());
                EXPECT_EQ(encodeSceneText(again), saved) << "a second save differs";
                scenes++;
            }else if (path.filename() == "project.yaml"){
                SCOPED_TRACE(path.string());
                YAML::Node node;
                try {
                    node = YAML::Load(source);
                } catch (const YAML::Exception&) {
                    continue;
                }
                if (!node.IsMap())
                    continue;
                Project project;
                Stream::decodeProject(&project, node, Workspace());
                const std::string saved = YAML::Dump(Stream::encodeProject(&project));
                if (source.find("pixelsPerUnit") == std::string::npos){
                    EXPECT_EQ(saved.find("pixelsPerUnit"), std::string::npos) << "graphics.pixelsPerUnit written into a project that had none";
                }
                Project reloaded;
                Stream::decodeProject(&reloaded, YAML::Load(saved), Workspace());
                EXPECT_EQ(YAML::Dump(Stream::encodeProject(&reloaded)), saved) << "a second save differs";
                projects++;
            }
        }
    }
    std::printf("[S41] re-saved %zu fixture scenes and %zu projects\n", scenes, projects);
    EXPECT_GT(scenes, 20u);
    EXPECT_GT(projects, 5u);
}

// S41's shader sources ('Pxs', pixel_snap.glsl, the frame block's last members) are cached under
// v57 or later: a cache of an older version holds programs without them
TEST(PixelPerfectStream, ShaderCacheVersionCoversThePixelSnap){
    const std::string name = editor::App::getUserShaderCacheDir().filename().string();
    ASSERT_EQ(name.rfind('v', 0), 0u) << name;
    EXPECT_GE(std::stoi(name.substr(1)), 57) << name;
}

// S43's palette sources (library:sprite's PALETTE_SWAP and PALETTE_CYCLE, the uber pass's 'Pal'
// bit 19) are cached under v58: a cache of an older version holds programs without them
TEST(PaletteShaders, ShaderCacheVersionCoversThePalettes){
    // (later specs took v59 to v61 after it: a later version covers them too)
    const std::string name = editor::App::getUserShaderCacheDir().filename().string();
    ASSERT_EQ(name.rfind('v', 0), 0u) << name;
    EXPECT_GE(std::stoi(name.substr(1)), 58) << name;
}
