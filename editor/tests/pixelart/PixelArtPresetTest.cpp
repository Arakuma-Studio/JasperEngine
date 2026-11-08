// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S42 AC9 (R2, R18, R23): the Pixel Art preset's plan, as Project Settings' preview lists it, is the
// pixel_art_mode fix followed by check_pixel_art {fix: ["all"]} on the project Pixel Art Mode then
// sees: the same changes, by kind then target.

#include <gtest/gtest.h>

#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelArtPreset.h"

#include <set>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;

namespace {

    // the 2d template as the validator reads it: 1280 x 720 letterbox, a sprite with a bilinear
    // reference to a default (BC7, mipmapped) texture, no saved camera
    PixelArtCheckInput templateInput(bool modeOn){
        PixelArtCheckInput input;
        input.project.modeEnabled = modeOn;
        input.project.canvasWidth = modeOn ? 320 : 1280;
        input.project.canvasHeight = modeOn ? 180 : 720;
        input.project.scaling = modeOn ? Scaling::INTEGER : Scaling::LETTERBOX;
        input.project.defaultFilterMode = modeOn ? TextureFilterMode::Point : TextureFilterMode::Bilinear;
        input.project.windowWidth = 1280;
        input.project.windowHeight = 720;
        input.project.screens = PixelArtSettings().screens();
        PixelArtScene scene;
        scene.path = "Main.scene";
        scene.name = "Main";
        scene.is2D = true;
        scene.pixelArt = true;
        scene.drawsSprites = true;
        PixelArtDrawable player;
        player.kind = DrawableKind::Sprite;
        player.scene = "Main.scene";
        player.entity = "/Player";
        player.texture = "sprites/player.png";
        player.textureField = "mesh.submeshes[0].material.baseColorTexture";
        player.hasTransform = true;
        player.position = player.worldPosition = Vector3(608, 328, 0);
        player.width = player.height = 64;
        scene.drawables.push_back(player);
        input.scenes.push_back(scene);
        PixelArtTexture texture;
        texture.path = "sprites/player.png";
        texture.exists = true;
        texture.hasSettings = true;
        texture.settings.type = TextureImportType::Default;
        texture.settings.inferredFrom = "no references";
        texture.width = texture.height = 64;
        texture.pixelArt = true;
        input.textures[texture.path] = texture;
        return input;
    }

    using Change = std::tuple<std::string, std::string, std::string>;
    std::set<Change> changeSet(const std::vector<FixChange>& changes){
        std::set<Change> out;
        for (const FixChange& change : changes)
            out.insert({change.target, change.field, change.to.dump()});
        return out;
    }

}

TEST(PixelArtPreset, PlanMatchesFixes){
    // the preset: the project as it is, its scenes read as Pixel Art Mode would
    PixelArtCheckInput before = templateInput(false);
    before.project.assumeMode = true;
    const std::vector<FixChange> preset = planPixelArtPreset(before, PixelArtPresetOptions());

    // the agent's route: pixel_art_mode, then everything on the project the mode now sees
    const CheckResult first = run(templateInput(false), {"pixel_art_mode"});
    const FixPlan modeFix = plan(first, {"pixel_art_mode"});
    ASSERT_EQ(modeFix.fixes.size(), 1u);
    std::vector<FixChange> agent = modeFix.fixes.front().second.changes;
    const CheckResult second = run(templateInput(true));
    const FixPlan all = plan(second, {"all"});
    for (const auto& [key, option] : all.fixes)
        agent.insert(agent.end(), option.changes.begin(), option.changes.end());

    EXPECT_EQ(changeSet(preset), changeSet(agent));
    // what AC9 expects of it
    std::set<std::string> fields;
    for (const FixChange& change : preset) fields.insert(change.field);
    for (const char* field : {"pixel_art.enabled", "graphics.canvas_width", "graphics.canvas_height", "graphics.scaling_mode",
                              "graphics.default_filter_mode", "camera", "import.textureImporter",
                              "mesh.submeshes[0].material.baseColorTexture"})
        EXPECT_TRUE(fields.count(field)) << field;
    EXPECT_FALSE(fields.count("player.window_width")) << "1280 x 720 is already 4 x 320 x 180";
    // deterministic, project settings first
    EXPECT_EQ(preset.front().target, "project");
    EXPECT_EQ(changeSet(preset), changeSet(planPixelArtPreset(before, PixelArtPresetOptions())));

    // the options: no cameras, no textures, another base
    PixelArtPresetOptions options;
    options.addCameras = false;
    options.convertTextures = false;
    options.pointReferences = false;
    options.baseWidth = 384;
    options.baseHeight = 216;
    options.windowScale = 3;
    const std::vector<FixChange> small = planPixelArtPreset(before, options);
    for (const FixChange& change : small) EXPECT_EQ(change.target, "project");
    std::map<std::string, nlohmann::json> to;
    for (const FixChange& change : small) to[change.field] = change.to;
    EXPECT_EQ(to["graphics.canvas_width"], 384);
    EXPECT_EQ(to["player.window_width"], 1152);
    const PresetCounts counts = countPresetChanges(preset);
    EXPECT_EQ(counts.scenes, 1);
    EXPECT_EQ(counts.textures, 1);
    EXPECT_EQ(counts.references, 1);
}
