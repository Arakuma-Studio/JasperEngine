// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S77 R27, R28, D4: a model's "<model>.clips" sidecar sets its clips' Wrap Mode and Root Motion
// where buildModelSubClip makes them (what the editor and a player both load), no sidecar or no
// entry means today's clip byte for byte, unknown keys survive a write, and the pool reads a
// changed sidecar again once the model is invalidated.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelClipSettings.h"
#include "animation/ModelSubClips.h"
#include "pool/AnimationClipPool.h"

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace jasperengine;

namespace clipsidecartest {

    std::string heroGltf(){
        std::ifstream file(std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models") + "/hero_animated.gltf",
                           std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // the clip as it plays, written out: equal text is an equal clip
    std::string built(const std::string& reference){
        ClipData data;
        std::vector<ClipWarning> warnings;
        std::string error;
        EXPECT_TRUE(anim::buildModelSubClip(reference, data, warnings, &error)) << error;
        return anim::writeClip(data);
    }

    class ModelClipSettingsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            model = "models/sidecar" + std::to_string(run++) + "/hero_animated.gltf";
            animtest::writeAsset(model, heroGltf());
            AnimationClipPool::clear();
        }
        void TearDown() override {
            AnimationClipPool::clear();
            anim::dropModelClipSettings("");
        }

        void writeSidecar(const std::string& text){
            animtest::writeAsset(anim::modelClipSettingsPath(model), text);
            anim::dropModelClipSettings(model);
        }

        std::string model;
    };

}

using clipsidecartest::ModelClipSettingsTest;

TEST(ModelClipSettings, ParseAndWriteKeepUnknownKeys){
    const std::string text = R"({
  "version": 1,
  "clips": {
    "Walk": {
      "wrap": "loop",
      "root_motion": {"bone": "mixamorig:Hips", "bake_rotation": true, "bake_position_y": false, "bake_position_xz": false, "future": 3},
      "note": {"by": "artist"}
    },
    "Idle": {"wrap": "ping_pong"}
  },
  "tool": "something else"
})";
    anim::ModelClipSettings settings;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(anim::parseModelClipSettings(text, settings, warnings, &error)) << error;
    EXPECT_TRUE(warnings.empty());
    ASSERT_NE(settings.find("Walk"), nullptr);
    const anim::ModelClipSetting& walk = *settings.find("Walk");
    EXPECT_EQ(walk.wrap, ClipWrapMode::Loop);
    ASSERT_TRUE(walk.rootMotion.has_value());
    EXPECT_EQ(walk.rootMotion->bone, "mixamorig:Hips");
    EXPECT_TRUE(walk.rootMotion->bakeRotation);
    EXPECT_FALSE(walk.rootMotion->bakePositionY);
    EXPECT_EQ(settings.find("Idle")->wrap, ClipWrapMode::PingPong);
    EXPECT_FALSE(settings.find("Idle")->rootMotion.has_value());
    EXPECT_EQ(settings.find("Run"), nullptr);

    const std::string written = anim::writeModelClipSettings(settings);
    anim::ModelClipSettings again;
    ASSERT_TRUE(anim::parseModelClipSettings(written, again, warnings, &error)) << error;
    EXPECT_EQ(anim::writeModelClipSettings(again), written) << "deterministic";
    EXPECT_NE(written.find("\"future\": 3"), std::string::npos) << written;
    EXPECT_NE(written.find("\"note\""), std::string::npos) << written;
    EXPECT_NE(written.find("\"tool\": \"something else\""), std::string::npos) << written;
    EXPECT_TRUE(*again.find("Walk") == walk);
    EXPECT_EQ(written.back(), '\n');
}

TEST(ModelClipSettings, BadValuesAreWarningsAndANewerVersionFails){
    anim::ModelClipSettings settings;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(anim::parseModelClipSettings(R"({"version": 1, "clips": {"Walk": {"wrap": "default"}, "Run": {"wrap": "sometimes",
        "root_motion": {"bake_rotation": "yes"}}}})", settings, warnings, &error));
    EXPECT_EQ(warnings.size(), 3u);
    EXPECT_EQ(settings.find("Walk"), nullptr) << "Default is a player entry's value, not a clip's";
    ASSERT_NE(settings.find("Run"), nullptr);
    EXPECT_FALSE(settings.find("Run")->wrap.has_value());
    EXPECT_FALSE(settings.find("Run")->rootMotion->bakeRotation) << "the default kept";

    EXPECT_FALSE(anim::parseModelClipSettings(R"({"version": 2, "clips": {}})", settings, warnings, &error));
    EXPECT_NE(error.find("version 2"), std::string::npos) << error;
    EXPECT_FALSE(anim::parseModelClipSettings("[1, 2]", settings, warnings, &error));
    EXPECT_FALSE(anim::parseModelClipSettings("{not json", settings, warnings, &error));
}

TEST_F(ModelClipSettingsTest, NoSidecarOrNoEntryIsTodaysClipByteForByte){
    const std::string walk = clipsidecartest::built(model + "#Walk");
    const std::string idle = clipsidecartest::built(model + "#Idle");
    ClipData data;
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(anim::buildModelSubClip(model + "#Walk", data, warnings, nullptr));
    EXPECT_EQ(data.wrap, ClipWrapMode::Once);
    EXPECT_TRUE(data.unknownKeys.empty());

    writeSidecar(R"({"version": 1, "clips": {"Idle": {"wrap": "loop"}}})");
    EXPECT_EQ(clipsidecartest::built(model + "#Walk"), walk) << "a clip with no entry";
    EXPECT_NE(clipsidecartest::built(model + "#Idle"), idle);

    writeSidecar(R"({"version": 1, "clips": {}})");
    EXPECT_EQ(clipsidecartest::built(model + "#Idle"), idle) << "an empty sidecar";

    writeSidecar("{broken");
    EXPECT_EQ(clipsidecartest::built(model + "#Walk"), walk) << "a sidecar that does not parse";
    const std::vector<std::string> logged = animtest::takeWarnings();
    EXPECT_TRUE(std::any_of(logged.begin(), logged.end(), [](const std::string& line){ return line.find(".clips") != std::string::npos; }));
}

TEST_F(ModelClipSettingsTest, TheSidecarSetsWrapAndRootMotion){
    writeSidecar(R"({"version": 1, "clips": {"Walk": {"wrap": "loop", "root_motion": {"bone": "mixamorig:Hips",
        "bake_rotation": true, "bake_position_y": true, "bake_position_xz": false}}, "Wave 2": {"wrap": "clamp_forever"}}})");
    ClipData data;
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(anim::buildModelSubClip(model + "#Walk", data, warnings, nullptr));
    EXPECT_EQ(data.wrap, ClipWrapMode::Loop);
    const anim::ClipRootMotion rootMotion = anim::readClipRootMotion(data.unknownKeys);
    EXPECT_TRUE(rootMotion.present);
    EXPECT_EQ(rootMotion.bone, "mixamorig:Hips");
    EXPECT_TRUE(rootMotion.bakeRotation);
    // by index too: the entry is the clip's name
    ASSERT_TRUE(anim::buildModelSubClip(model + "#4", data, warnings, nullptr));
    EXPECT_EQ(data.wrap, ClipWrapMode::ClampForever);
    ASSERT_TRUE(anim::buildModelSubClip(model + "#Wave", data, warnings, nullptr));
    EXPECT_EQ(data.wrap, ClipWrapMode::Once) << "the first Wave has no entry";
}

TEST_F(ModelClipSettingsTest, ThePoolReadsAChangedSidecarOnceTheModelIsInvalidated){
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(model + "#Walk");
    ASSERT_TRUE(walk);
    EXPECT_EQ(walk->getWrap(), ClipWrapMode::Once);

    animtest::writeAsset(anim::modelClipSettingsPath(model), R"({"version": 1, "clips": {"Walk": {"wrap": "loop"}}})");
    EXPECT_EQ(AnimationClipPool::getOrLoad(model + "#Walk")->getWrap(), ClipWrapMode::Once) << "kept until the model is invalidated";
    const uint64_t revision = AnimationClipPool::revision();
    AnimationClipPool::invalidateModel(model);
    EXPECT_NE(AnimationClipPool::revision(), revision) << "the players and Animators rebind";
    std::shared_ptr<const AnimationClip> looping = AnimationClipPool::getOrLoad(model + "#Walk");
    ASSERT_TRUE(looping);
    EXPECT_EQ(looping->getWrap(), ClipWrapMode::Loop);
    EXPECT_FLOAT_EQ(anim::wrapClipTime(1.25f * walk->getLength(), walk->getLength(), looping->getWrap()), 0.25f * walk->getLength())
        << "it plays on past its end";
}
