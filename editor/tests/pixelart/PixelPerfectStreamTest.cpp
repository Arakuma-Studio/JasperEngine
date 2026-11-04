// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC17 (R26, R27): the Pixel Perfect Camera and the Sprite and Tilemap Pixels Per Unit in the
// scene format. No version bump: every key is optional with a default that draws what a scene
// drew before. A scene with the component round-trips, the keys of a later editor under it are
// kept, and a scene without the new keys re-saves byte for byte.

#include "support/EditorTestSupport.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "component/CameraComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    // a scene of one camera GameObject: the components block given
    std::string cameraScene(const std::string& components){
        return "formatVersion: 7\n"
               "id: 4141\n"
               "name: PixelPerfect\n"
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
    // and the spelling of the spec's sketch reads as the key
    DecodedScene aliased = decodeSceneText(cameraScene(
        "      transform:\n"
        "        position: [0, 0, 1]\n"
        "      camera:\n"
        "        type: camera_ortho\n"
        "      pixelPerfectCamera:\n"
        "        referenceWidth: 256\n"), "alias.scene");
    const Entity aliasedCamera = firstEntity(aliased);
    ASSERT_NE(aliasedCamera, NULL_ENTITY);
    ASSERT_TRUE(aliased.scene->scene->findComponent<PixelPerfectCameraComponent>(aliasedCamera));
    EXPECT_EQ(aliased.scene->scene->getComponent<PixelPerfectCameraComponent>(aliasedCamera).referenceWidth, 256u);
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
