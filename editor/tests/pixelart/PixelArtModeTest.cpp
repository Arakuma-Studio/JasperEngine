// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S42 AC14, AC15 (R1, R16, R27, R28): Pixel Art Mode registers its import and Play hooks only while
// it is on (zero cost when off), project.yaml's pixelArt block and graphics.defaultFilterMode are
// written only away from their defaults, and every committed fixture project re-saves without them.

#include <gtest/gtest.h>

#include "Engine.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "pixelart/PixelArtMode.h"
#include "pixelart/PixelArtSettings.h"
#include "support/EditorTestSupport.h"
#include "texture/Texture.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;
namespace fs = std::filesystem;

TEST(PixelArtMode, OffRegistersNothing){
    Project project;
    PixelArtMode& mode = PixelArtMode::get();
    mode.projectSettingsChanged(&project);
    EXPECT_FALSE(mode.importHookRegistered());
    EXPECT_FALSE(mode.playHookRegistered());

    PixelArtSettings on = project.getPixelArtSettings();
    on.enabled = true;
    project.setPixelArtSettings(on);
    EXPECT_TRUE(mode.importHookRegistered());
    EXPECT_TRUE(mode.playHookRegistered());
    // the switches turn each hook off on their own
    on.checkOnImport = false;
    project.setPixelArtSettings(on);
    EXPECT_FALSE(mode.importHookRegistered());
    EXPECT_TRUE(mode.playHookRegistered());
    // off: nothing registered, whatever the switches say
    on.enabled = false;
    on.checkOnImport = true;
    project.setPixelArtSettings(on);
    EXPECT_FALSE(mode.importHookRegistered());
    EXPECT_FALSE(mode.playHookRegistered());
    // turning the mode on changes no other setting (D1)
    const int canvas = (int)project.getCanvasWidth();
    const Scaling scaling = project.getScalingMode();
    on.enabled = true;
    project.setPixelArtSettings(on);
    EXPECT_EQ((int)project.getCanvasWidth(), canvas);
    EXPECT_EQ(project.getScalingMode(), scaling);
    EXPECT_EQ(project.getDefaultFilterMode(), TextureFilterMode::Bilinear);
    on.enabled = false;
    project.setPixelArtSettings(on);
}

TEST(PixelArtMode, ProjectYamlHoldsOnlyWhatDiffers){
    EXPECT_EQ(encodePixelArtSettings(PixelArtSettings()).size(), 0u);
    PixelArtSettings settings;
    settings.enabled = true;
    settings.checkOnPlay = false;
    settings.checkScreens = {"1280x720", "640x480"};
    settings.upscaleMode = PixelUpscaleMode::INTEGER_EXPAND;
    const YAML::Node node = encodePixelArtSettings(settings);
    EXPECT_TRUE(node["enabled"].as<bool>());
    EXPECT_FALSE(node["checkOnPlay"].as<bool>());
    EXPECT_FALSE(node["checkOnImport"]);
    EXPECT_FALSE(node["backdrops"]);
    PixelArtSettings decoded;
    decodePixelArtSettings(YAML::Load(YAML::Dump(node)), decoded);
    EXPECT_TRUE(decoded == settings);
    // values that do not parse keep their defaults
    PixelArtSettings bad;
    decodePixelArtSettings(YAML::Load("{checkScreens: [12x, 800x600], backdrops: [red], upscaleMode: sideways}"), bad);
    EXPECT_EQ(bad.checkScreens, std::vector<std::string>({"800x600"}));
    EXPECT_EQ(bad.backdrops, std::vector<std::string>());
    EXPECT_EQ(bad.upscaleMode, PixelUpscaleMode::INTEGER);

    // through the project codec: the block and graphics.defaultFilterMode appear only when set
    Project project;
    std::string saved = YAML::Dump(Stream::encodeProject(&project));
    EXPECT_EQ(saved.find("pixelArt"), std::string::npos);
    EXPECT_EQ(saved.find("defaultFilterMode"), std::string::npos);
    project.setPixelArtSettings(settings);
    project.setDefaultFilterMode(TextureFilterMode::Point);
    saved = YAML::Dump(Stream::encodeProject(&project));
    Project reloaded;
    Stream::decodeProject(&reloaded, YAML::Load(saved), Workspace());
    EXPECT_TRUE(reloaded.getPixelArtSettings() == settings);
    EXPECT_EQ(reloaded.getDefaultFilterMode(), TextureFilterMode::Point);
    PixelArtSettings off = reloaded.getPixelArtSettings();
    off.enabled = false;
    reloaded.setPixelArtSettings(off);
    project.setPixelArtSettings(PixelArtSettings());
}

// R28: old projects load and save byte-identically: no fixture gains S42's keys
TEST(PixelArtMode, FixturesResaveWithoutS42Keys){
    const fs::path roots[] = {fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures", fs::path(JASPER_TEST_FIXTURES_DIR)};
    size_t projects = 0;
    for (const fs::path& root : roots){
        ASSERT_TRUE(fs::is_directory(root)) << root;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)){
            if (!entry.is_regular_file() || entry.path().filename() != "project.yaml")
                continue;
            SCOPED_TRACE(entry.path().string());
            std::ifstream in(entry.path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            const std::string source = text.str();
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
            if (source.find("pixelArt") == std::string::npos) {
                EXPECT_EQ(saved.find("pixelArt"), std::string::npos) << "pixelArt written into a project that had none";
            }
            if (source.find("defaultFilterMode") == std::string::npos) {
                EXPECT_EQ(saved.find("defaultFilterMode"), std::string::npos);
            }
            Project reloaded;
            Stream::decodeProject(&reloaded, YAML::Load(saved), Workspace());
            EXPECT_EQ(YAML::Dump(Stream::encodeProject(&reloaded)), saved) << "a second save differs";
            projects++;
            PixelArtSettings off = reloaded.getPixelArtSettings();
            off.enabled = false;
            reloaded.setPixelArtSettings(off);
            off = project.getPixelArtSettings();
            off.enabled = false;
            project.setPixelArtSettings(off);
        }
    }
    std::printf("[S42] re-saved %zu fixture projects\n", projects);
    EXPECT_GT(projects, 5u);
}

TEST(PixelArtMode, SavedReferencesStayBilinearWhilePlaying){
    // Play applies Default Filter Mode point: textures the game makes start NEAREST (R3)...
    Engine::setDefaultTextureFilter(TextureFilter::NEAREST);
    EXPECT_EQ(Texture().getMinFilter(), TextureFilter::NEAREST);
    // ...but what a scene's data reads and writes keeps its own defaults: a Text's font atlas
    // sampler (keys left out when they are the defaults) decodes bilinear, and a bilinear one
    // still leaves its keys out
    Texture atlas;
    Stream::decodeAtlasSampler(YAML::Node(YAML::NodeType::Map), atlas);
    EXPECT_EQ(atlas.getMinFilter(), TextureFilter::LINEAR);
    EXPECT_EQ(atlas.getMagFilter(), TextureFilter::LINEAR);
    const YAML::Node encoded = Stream::encodeAtlasSampler(atlas);
    EXPECT_FALSE(encoded["minFilter"]) << YAML::Dump(encoded);
    // a reference without filter keys (a camera link) decodes bilinear too
    const Texture linked = Stream::decodeTexture(YAML::Load("{textureType: 2D, source: camera, camera: 7}"));
    EXPECT_EQ(linked.getMinFilter(), TextureFilter::LINEAR);
    EXPECT_EQ(Stream::savedTexture().getMagFilter(), TextureFilter::LINEAR);
    Engine::setDefaultTextureFilter(TextureFilter::LINEAR);
}

// S48 R38: in a Pixel Art Mode project, ticking library:sprite's Outline picks the 4-neighbour pixel
// style; a chosen style, another shader, Outline off or the mode off leave the features alone
TEST(PixelArtMode, OutlinePicksThePixelStyle){
    std::vector<std::string> features = {"FLASH", "OUTLINE"};
    PixelArtMode::pickPixelOutline(features, "library:sprite", true);
    EXPECT_EQ(features, (std::vector<std::string>{"FLASH", "OUTLINE", "OUTLINE_STYLE_PIXEL4"}));

    std::vector<std::string> off = {"FLASH", "OUTLINE"};
    PixelArtMode::pickPixelOutline(off, "library:sprite", false);
    EXPECT_EQ(off, (std::vector<std::string>{"FLASH", "OUTLINE"}));

    std::vector<std::string> chosen = {"OUTLINE", "OUTLINE_STYLE_PIXEL8"};
    PixelArtMode::pickPixelOutline(chosen, "library:sprite", true);
    EXPECT_EQ(chosen, (std::vector<std::string>{"OUTLINE", "OUTLINE_STYLE_PIXEL8"}));

    std::vector<std::string> smooth = {"OUTLINE", "OUTLINE_STYLE_SMOOTH"};
    PixelArtMode::pickPixelOutline(smooth, "library:sprite", true);
    EXPECT_EQ(smooth, (std::vector<std::string>{"OUTLINE", "OUTLINE_STYLE_SMOOTH"}));

    std::vector<std::string> noOutline = {"FLASH"};
    PixelArtMode::pickPixelOutline(noOutline, "library:sprite", true);
    EXPECT_EQ(noOutline, (std::vector<std::string>{"FLASH"}));

    std::vector<std::string> otherShader = {"OUTLINE"};
    PixelArtMode::pickPixelOutline(otherShader, "library:lit", true);
    EXPECT_EQ(otherShader, (std::vector<std::string>{"OUTLINE"}));
}
