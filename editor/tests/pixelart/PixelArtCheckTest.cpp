// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S42 AC6, AC7 (R9-R14): each of S42's rules on the data of tools/fixtures/pixel-art-bad (one seeded
// defect per rule) gives its status, measured and expected values, and the same rule on the clean
// data passes; removing the seeded defect turns the rule PASS (the red control per rule). The
// rules are pure functions of the input (D3): the fixture's files are read into the same input
// by test_check_pixel_art.py, which compares the CLI's JSON with check_pixel_art's.

#include <gtest/gtest.h>

#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelArtRules.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;
using Json = nlohmann::json;

namespace {

    PixelArtDrawable sprite(const std::string& entity, const std::string& texture, Vector3 position = Vector3(16, 16, 0)){
        PixelArtDrawable d;
        d.kind = DrawableKind::Sprite;
        d.scene = "scenes/Main.scene";
        d.entity = entity;
        d.texture = texture;
        d.textureField = "mesh.submeshes[0].material.baseColorTexture";
        d.minFilter = TextureFilter::NEAREST;
        d.magFilter = TextureFilter::NEAREST;
        d.hasTransform = true;
        d.position = position;
        d.worldPosition = position;
        d.width = 16;
        d.height = 16;
        return d;
    }

    PixelArtTexture texture(const std::string& path){
        PixelArtTexture t;
        t.path = path;
        t.exists = true;
        t.hasSettings = true;
        t.settings.type = TextureImportType::Sprite;
        t.settings.pixelArt = true;
        t.width = 16;
        t.height = 16;
        t.pixelArt = true;
        return t;
    }

    // tools/fixtures/pixel-art: Pixel Art Mode, 320 x 180 at integer scales, a Pixel Perfect
    // Camera, point-filtered lossless sprites on whole pixels
    PixelArtCheckInput cleanInput(){
        PixelArtCheckInput input;
        input.project.modeEnabled = true;
        input.project.canvasWidth = 320;
        input.project.canvasHeight = 180;
        input.project.scaling = Scaling::INTEGER;
        input.project.pixelsPerUnit = 1;
        input.project.defaultFilterMode = TextureFilterMode::Point;
        input.project.windowWidth = 1280;
        input.project.windowHeight = 720;
        input.project.screens = PixelArtSettings().screens();
        PixelArtScene scene;
        scene.path = "scenes/Main.scene";
        scene.name = "Main";
        scene.is2D = true;
        scene.pixelArt = true;
        scene.drawsSprites = true;
        scene.camera.exists = true;
        scene.camera.entity = "/Main Camera";
        scene.camera.hasComponent = true;
        scene.drawables.push_back(sprite("/Hero", "sprites/hero.png"));
        input.scenes.push_back(scene);
        input.textures["sprites/hero.png"] = texture("sprites/hero.png");
        return input;
    }

    const Finding* findingOf(const CheckResult& result, const std::string& rule){
        for (const Finding& finding : result.findings)
            if (finding.rule == rule)
                return &finding;
        return nullptr;
    }

    RuleStatus statusOf(const PixelArtCheckInput& input, const std::string& rule){
        const CheckResult result = run(input, {rule});
        return result.rules.empty() ? RuleStatus::Pass : result.rules.front().status;
    }

}

TEST(PixelArtCheck, CleanDataPassesEveryRule){
    const CheckResult result = run(cleanInput());
    EXPECT_EQ(result.verdict, Verdict::Clean) << result.toJson().dump(1);
    EXPECT_TRUE(result.findings.empty());
    for (const RuleResult& rule : result.rules)
        EXPECT_EQ(rule.status, RuleStatus::Pass) << rule.id;
    // a rule with nothing in scope is PASS with checked 0
    for (const RuleResult& rule : result.rules)
        if (rule.id == "palette") {
            EXPECT_EQ(rule.checked, 0);
        }
}

TEST(PixelArtCheck, PixelArtMode){
    PixelArtCheckInput input = cleanInput();
    input.project.modeEnabled = false;
    input.project.scaling = Scaling::LETTERBOX;
    input.project.canvasWidth = 1280;
    input.project.canvasHeight = 720;
    input.project.defaultFilterMode = TextureFilterMode::Bilinear;
    const CheckResult result = run(input, {"pixel_art_mode"});
    const Finding* f = findingOf(result, "pixel_art_mode");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn);
    EXPECT_EQ(f->measured["pixel_art.enabled"], false);
    EXPECT_EQ(f->measured["graphics.scaling_mode"], "letterbox");
    EXPECT_EQ(f->measured["graphics.canvas"], "1280x720");
    ASSERT_EQ(f->options.size(), 1u);
    // the preset's project part: the mode, 320 x 180, integer, point, a 4x window (already 1280 x 720)
    std::map<std::string, Json> to;
    for (const FixChange& change : f->options.front().changes) to[change.field] = change.to;
    EXPECT_EQ(to["pixel_art.enabled"], true);
    EXPECT_EQ(to["graphics.canvas_width"], 320);
    EXPECT_EQ(to["graphics.canvas_height"], 180);
    EXPECT_EQ(to["graphics.scaling_mode"], "integer");
    EXPECT_EQ(to["graphics.default_filter_mode"], "point");
    EXPECT_FALSE(to.count("player.window_width")) << "1280 x 720 is 4 x 320 x 180 already";
    EXPECT_EQ(statusOf(cleanInput(), "pixel_art_mode"), RuleStatus::Pass);
    // with no pixel-art content and the mode off there is nothing to check
    PixelArtCheckInput empty;
    empty.project = input.project;
    const CheckResult none = run(empty, {"pixel_art_mode"});
    EXPECT_EQ(none.rules.front().checked, 0);
}

TEST(PixelArtCheck, IntegerScale){
    // a scene with fixed resolution 640 x 360 and letterbox (fit) scaling of a 320 x 180 canvas
    PixelArtCheckInput input = cleanInput();
    input.project.scaling = Scaling::LETTERBOX;
    PixelArtScene& scene = input.scenes.front();
    scene.camera.hasComponent = false;
    scene.fixedResolution = true;
    scene.fixedWidth = 640;
    scene.fixedHeight = 360;
    const CheckResult result = run(input, {"integer_scale"});
    const Finding* f = findingOf(result, "integer_scale");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Fail);
    const Json& laptop = f->measured["screens"]["1366x768"];
    EXPECT_EQ(laptop["integer"], false);
    EXPECT_NEAR(laptop["scale"].get<double>(), 2.1333, 0.001) << laptop.dump();
    EXPECT_EQ(f->measured["screens"]["1920x1080"]["integer"], true) << "1920x1080 is 3x";
    ASSERT_FALSE(f->options.empty());
    const FixChange& add = f->options.front().changes.front();
    EXPECT_EQ(add.field, "pixelperfectcamera");
    EXPECT_EQ(add.to["referenceWidth"], 640) << "the reference is the fixed resolution";
    // red control: with the Pixel Perfect Camera every screen is a whole scale
    EXPECT_EQ(statusOf(cleanInput(), "integer_scale"), RuleStatus::Pass);
    // a scene with integer scaling but no Pixel Perfect Camera warns (nothing snaps)
    PixelArtCheckInput noCamera = cleanInput();
    noCamera.scenes.front().camera.hasComponent = false;
    EXPECT_EQ(statusOf(noCamera, "integer_scale"), RuleStatus::Warn);
    // a Stretch Fill camera warns
    PixelArtCheckInput stretch = cleanInput();
    stretch.scenes.front().camera.component.upscaleMode = PixelUpscaleMode::STRETCH_FILL;
    EXPECT_EQ(statusOf(stretch, "integer_scale"), RuleStatus::Warn);
}

TEST(PixelArtCheck, Filtering){
    PixelArtCheckInput input = cleanInput();
    input.scenes.front().drawables.front().minFilter = TextureFilter::LINEAR;
    input.scenes.front().drawables.front().magFilter = TextureFilter::LINEAR;
    const CheckResult result = run(input, {"filtering"});
    const Finding* f = findingOf(result, "filtering");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Fail);
    EXPECT_EQ(f->key, "filtering:scenes/Main.scene#/Hero:mesh.submeshes[0].material.baseColorTexture");
    EXPECT_EQ(f->measured, (Json{{"min_filter", "linear"}, {"mag_filter", "linear"}}));
    EXPECT_EQ(f->expected, (Json{{"min_filter", "nearest"}, {"mag_filter", "nearest"}}));
    EXPECT_EQ(f->options.front().changes.front().to, (Json{{"min_filter", "nearest"}, {"mag_filter", "nearest"}}));
    EXPECT_EQ(statusOf(cleanInput(), "filtering"), RuleStatus::Pass);
}

TEST(PixelArtCheck, MipmapsCompression){
    PixelArtCheckInput input = cleanInput();
    PixelArtTexture& t = input.textures["sprites/hero.png"];
    t.settings = TextureImportSettings();
    t.settings.type = TextureImportType::Default;   // BC7 with mipmaps
    t.settings.quality = TextureCookQuality::High;
    t.width = t.height = 64;
    const CheckResult result = run(input, {"mipmaps_compression"});
    const Finding* f = findingOf(result, "mipmaps_compression");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Fail);
    EXPECT_EQ(f->measured["mipmaps"], true);
    EXPECT_EQ(f->measured["format"]["desktop"], "BC7") << f->measured.dump();
    EXPECT_GT(f->measured["vram_bytes"].get<int64_t>(), 0);
    const Json to = f->options.front().changes.front().to;
    EXPECT_EQ(to["pixelArt"], true);
    EXPECT_FALSE(to.contains("quality")) << "the contradicting field is cleared";
    // the fix's settings resolve lossless
    t.settings.pixelArt = true;
    t.settings.quality.reset();
    t.settings.type = TextureImportType::Sprite;
    EXPECT_EQ(statusOf(input, "mipmaps_compression"), RuleStatus::Pass);
    // a max size that clamps the art warns (uneven pixels)
    PixelArtCheckInput clamped = cleanInput();
    clamped.textures["sprites/hero.png"].settings.maxSize = 32;
    clamped.textures["sprites/hero.png"].width = 64;
    EXPECT_EQ(statusOf(clamped, "mipmaps_compression"), RuleStatus::Warn);
    // a lossy .aseprite sheet
    PixelArtCheckInput sheet = cleanInput();
    PixelArtTexture ase;
    ase.path = "sprites/hero.aseprite";
    ase.aseprite = true;
    ase.asepriteCompression = "project";
    ase.pixelArt = true;
    sheet.textures[ase.path] = ase;
    const CheckResult sheetResult = run(sheet, {"mipmaps_compression"});
    ASSERT_TRUE(findingOf(sheetResult, "mipmaps_compression"));
    EXPECT_EQ(findingOf(sheetResult, "mipmaps_compression")->options.front().changes.front().field,
              "import.asepriteImporter.texture.compression");
}

TEST(PixelArtCheck, TexelSize){
    PixelArtCheckInput input = cleanInput();
    PixelArtDrawable& d = input.scenes.front().drawables.front();
    d.scale = d.worldScale = Vector3(1.5f, 1.5f, 1.0f);
    const CheckResult result = run(input, {"texel_size"});
    const Finding* f = findingOf(result, "texel_size");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Fail);
    EXPECT_EQ(f->measured, (Json{{"x", 1.5}, {"y", 1.5}}));
    EXPECT_EQ(f->options.front().changes.front().field, "transform.scale");
    EXPECT_EQ(f->options.front().changes.front().to, (Json::array({1, 1, 1})));
    // a PPU-32 sprite in a PPU-16 project: half-size pixels, fixed by the project's PPU
    PixelArtCheckInput ppu = cleanInput();
    ppu.project.pixelsPerUnit = 16;
    ppu.scenes.front().drawables.front().pixelsPerUnit = 32;
    const CheckResult ppuResult = run(ppu, {"texel_size"});
    const Finding* g = findingOf(ppuResult, "texel_size");
    ASSERT_TRUE(g);
    EXPECT_EQ(g->status, RuleStatus::Fail);
    EXPECT_EQ(g->measured, (Json{{"x", 0.5}, {"y", 0.5}}));
    EXPECT_EQ(g->options.front().changes.front().field, "sprite.pixelsPerUnit");
    EXPECT_EQ(g->options.front().changes.front().to, 0);
    // deliberate big pixels: 2 x 2 warns
    PixelArtCheckInput big = cleanInput();
    big.scenes.front().drawables.front().worldScale = Vector3(2, 2, 1);
    EXPECT_EQ(statusOf(big, "texel_size"), RuleStatus::Warn);
    EXPECT_EQ(statusOf(cleanInput(), "texel_size"), RuleStatus::Pass);
}

TEST(PixelArtCheck, OffGrid){
    PixelArtCheckInput input = cleanInput();
    PixelArtDrawable& d = input.scenes.front().drawables.front();
    d.position = d.worldPosition = Vector3(10.3f, 20.0f, 0.0f);
    const CheckResult result = run(input, {"off_grid"});
    const Finding* f = findingOf(result, "off_grid");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn);
    EXPECT_NEAR(f->measured["anchor"][0].get<double>(), 10.3, 1e-4);
    EXPECT_NEAR(f->measured["fraction"][0].get<double>(), 0.3, 1e-4);
    const Json to = f->options.front().changes.front().to;
    EXPECT_NEAR(to[0].get<double>(), 10.0, 1e-5) << "S41's snap of 10.3 is 10";
    EXPECT_NEAR(to[1].get<double>(), 20.0, 1e-5);
    EXPECT_EQ(statusOf(cleanInput(), "off_grid"), RuleStatus::Pass);
    // a CENTER pivot of an odd size sits on half pixels by design: its anchor is the quad's corner
    PixelArtCheckInput centred = cleanInput();
    PixelArtDrawable& c = centred.scenes.front().drawables.front();
    c.width = c.height = 15;
    c.pivot = Vector2(7.5f, 7.5f);
    c.position = c.worldPosition = Vector3(7.5f, 7.5f, 0.0f);
    EXPECT_EQ(statusOf(centred, "off_grid"), RuleStatus::Pass);
}

TEST(PixelArtCheck, Rotation){
    PixelArtCheckInput input = cleanInput();
    PixelArtDrawable& d = input.scenes.front().drawables.front();
    d.rotation = d.worldRotation = Quaternion(0.0f, 0.0f, 37.0f);
    const CheckResult result = run(input, {"rotation"});
    const Finding* f = findingOf(result, "rotation");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn);
    EXPECT_NEAR(f->measured["z"].get<double>(), 37.0, 1e-3);
    ASSERT_EQ(f->options.size(), 2u) << "round, or Rotate On Grid with the render texture";
    EXPECT_NEAR(f->options[0].changes.front().to["euler"][2].get<double>(), 0.0, 1e-3);
    EXPECT_EQ(f->options[1].id, "on_grid");
    EXPECT_EQ(f->options[1].changes.front().to, "on_grid");
    // 90-degree steps pass, and so does Rotate On Grid with the render texture
    PixelArtCheckInput quarter = cleanInput();
    quarter.scenes.front().drawables.front().worldRotation = Quaternion(0.0f, 0.0f, 90.0f);
    EXPECT_EQ(statusOf(quarter, "rotation"), RuleStatus::Pass);
    input.scenes.front().drawables.front().pixelRotation = PixelRotation::ON_GRID;
    EXPECT_EQ(statusOf(input, "rotation"), RuleStatus::Pass);
    EXPECT_EQ(statusOf(cleanInput(), "rotation"), RuleStatus::Pass);
    // a rolled camera warns
    PixelArtCheckInput rolled = cleanInput();
    rolled.scenes.front().camera.roll = 15.0f;
    EXPECT_EQ(statusOf(rolled, "rotation"), RuleStatus::Warn);
}

TEST(PixelArtCheck, FontAntialiasing){
    PixelArtCheckInput input = cleanInput();
    PixelArtDrawable text;
    text.kind = DrawableKind::Text;
    text.scene = "scenes/Main.scene";
    text.entity = "/Label";
    text.fontSize = 20;
    text.hasTransform = true;
    input.scenes.front().drawables.push_back(text);
    const CheckResult result = run(input, {"font_antialiasing"});
    const Finding* f = findingOf(result, "font_antialiasing");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn);
    EXPECT_EQ(f->measured["kind"], "vector") << "before S51 every font is a vector font";
    EXPECT_TRUE(f->options.empty());
    EXPECT_FALSE(f->needsPerson.empty());
    EXPECT_EQ(result.verdict, Verdict::NeedsPerson);
    // S51's analysis plugs in: a bitmap font at a whole multiple of its native size passes
    setFontAnalyzer([](const std::filesystem::path&, const std::string& font, unsigned int size){
        PixelArtFontInfo info;
        info.font = font;
        info.kind = "bitmap";
        info.size = size;
        info.nativeSize = 10;
        info.antialiased = false;
        info.partialAlphaShare = 0.0f;
        return info;
    });
    EXPECT_EQ(statusOf(input, "font_antialiasing"), RuleStatus::Pass);
    input.scenes.front().drawables.back().fontSize = 15;
    const CheckResult off = run(input, {"font_antialiasing"});
    ASSERT_TRUE(findingOf(off, "font_antialiasing"));
    EXPECT_EQ(findingOf(off, "font_antialiasing")->options.front().changes.front().to, 20) << "the nearest whole multiple";
    setFontAnalyzer(nullptr);
    EXPECT_EQ(statusOf(cleanInput(), "font_antialiasing"), RuleStatus::Pass);
}

TEST(PixelArtCheck, ParticlesGrid){
    PixelArtCheckInput input = cleanInput();
    PixelArtDrawable particles;
    particles.kind = DrawableKind::Particles;
    particles.scene = "scenes/Main.scene";
    particles.entity = "/Sparks";
    particles.texture = "sprites/spark.png";
    particles.particleRenderMode = "stretched_billboard";
    particles.particleSizes = {4.0f};
    particles.hasTransform = true;
    input.scenes.front().drawables.push_back(particles);
    const CheckResult result = run(input, {"particles_grid"});
    const Finding* f = findingOf(result, "particles_grid");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn);
    EXPECT_EQ(f->measured["render_mode"], "stretched_billboard");
    // a linear default filter fails, fixed by the project's Default Filter Mode
    input.scenes.front().drawables.back().particleRenderMode = "billboard";
    input.project.defaultFilterMode = TextureFilterMode::Bilinear;
    const CheckResult linear = run(input, {"particles_grid"});
    ASSERT_TRUE(findingOf(linear, "particles_grid"));
    EXPECT_EQ(findingOf(linear, "particles_grid")->status, RuleStatus::Fail);
    EXPECT_EQ(findingOf(linear, "particles_grid")->options.front().changes.front().field, "graphics.default_filter_mode");
    // S49: without its Pixel Art module the effect is off the grid, and the fix adds it
    input.project.defaultFilterMode = TextureFilterMode::Point;
    const CheckResult noModule = run(input, {"particles_grid"});
    const Finding* m = findingOf(noModule, "particles_grid");
    ASSERT_TRUE(m);
    EXPECT_EQ(m->status, RuleStatus::Warn);
    EXPECT_TRUE(m->measured["pixel_art_module"].is_null());
    ASSERT_FALSE(m->options.empty()) << m->needsPerson;
    const FixChange& add = m->options.front().changes.back();
    EXPECT_EQ(add.field, "particlesystem.modules.pixelArt");
    EXPECT_EQ(add.to, (Json{{"snap", true}, {"pixelSizes", true}, {"fade", "dither"}, {"enabled", true}}));
    // the module with an alpha fade (partial alpha) warns; its fix only changes the fade
    PixelArtDrawable& sparks = input.scenes.front().drawables.back();
    sparks.particlePixelArt = sparks.particleSnap = sparks.particlePixelSizes = sparks.particleIntegerSize = true;
    sparks.particleFade = "alpha";
    const CheckResult alphaResult = run(input, {"particles_grid"});
    ASSERT_TRUE(findingOf(alphaResult, "particles_grid"));
    EXPECT_EQ(findingOf(alphaResult, "particles_grid")->options.front().changes.back().to, (Json{{"fade", "dither"}}));
    // red control: billboard, whole-pixel sizes, point filtering, a snapping camera, the module dithering
    sparks.particleFade = "dither";
    EXPECT_EQ(statusOf(input, "particles_grid"), RuleStatus::Pass);
}

TEST(PixelArtCheck, AntiAliasing){
    PixelArtCheckInput input = cleanInput();
    input.scenes.front().fxaa = true;
    const CheckResult result = run(input, {"anti_aliasing"});
    const Finding* f = findingOf(result, "anti_aliasing");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn);
    EXPECT_EQ(f->measured["fxaa"], true);
    EXPECT_EQ(f->options.front().changes.front().field, "post_processing.fxaa_enabled");
    EXPECT_EQ(statusOf(cleanInput(), "anti_aliasing"), RuleStatus::Pass);
}

TEST(PixelArtCheck, Palette){
    // the off-palette pixels need the image: the fixture's sprite is measured by the MCP test;
    // here a palette the project names makes the rule run (checked > 0) and a missing file fails
    PixelArtCheckInput input = cleanInput();
    input.project.defaultPalette = "palettes/game.palette";
    input.palettes["palettes/game.palette"] = {{0, 0, 0, 255}, {255, 255, 255, 255}};
    input.assetsRoot = "/nonexistent-s42-assets";
    const CheckResult result = run(input, {"palette"});
    EXPECT_EQ(result.rules.front().checked, 1);
    const Finding* f = findingOf(result, "palette");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Fail);
    // red control: no Default Palette, nothing to measure
    EXPECT_EQ(statusOf(cleanInput(), "palette"), RuleStatus::Pass);
}

TEST(PixelArtCheck, SpriteVariants){
    PixelArtCheckInput input = cleanInput();
    std::set<std::string> variants;
    for (int i = 0; i < 30; i++) variants.insert("mesh:variant" + std::to_string(i));
    input.scenes.front().spriteVariants["library:sprite [A, B]"] = variants;
    input.scenes.front().spriteVariants["library:sprite"] = {"mesh:variant0"};
    const CheckResult result = run(input, {"sprite_variants"});
    const Finding* f = findingOf(result, "sprite_variants");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn);
    EXPECT_EQ(f->measured["variants"], 30);
    EXPECT_EQ(f->measured["by_material"][0]["material"], "library:sprite [A, B]") << "the material adding the most first";
    EXPECT_TRUE(f->options.empty());
    // red control: 24 variants are within the budget
    input.scenes.front().spriteVariants["library:sprite [A, B]"].clear();
    for (int i = 0; i < 24; i++) input.scenes.front().spriteVariants["library:sprite [A, B]"].insert("mesh:variant" + std::to_string(i));
    EXPECT_EQ(statusOf(input, "sprite_variants"), RuleStatus::Pass);
}

// R11-R14: ids in rule order, verdicts from fixes, selections, determinism
TEST(PixelArtCheck, Deterministic){
    PixelArtCheckInput input = cleanInput();
    input.scenes.front().drawables.front().minFilter = TextureFilter::LINEAR;
    input.scenes.front().drawables.push_back(sprite("/B", "sprites/hero.png", Vector3(1.25f, 3.0f, 0.0f)));
    input.scenes.front().fxaa = true;
    const std::string a = run(input).toJson().dump();
    const std::string b = run(input).toJson().dump();
    EXPECT_EQ(a, b);
    const CheckResult result = run(input);
    ASSERT_GE(result.findings.size(), 3u);
    EXPECT_EQ(result.findings[0].id, "F1");
    EXPECT_EQ(result.findings[0].rule, "filtering") << "rule order: filtering before off_grid before anti_aliasing";
    EXPECT_EQ(result.verdict, Verdict::Fixable) << "every finding has a fix";
}

TEST(PixelArtCheck, FixSelections){
    PixelArtCheckInput input = cleanInput();
    PixelArtDrawable& d = input.scenes.front().drawables.front();
    d.minFilter = TextureFilter::LINEAR;
    d.worldRotation = d.rotation = Quaternion(0.0f, 0.0f, 37.0f);
    const CheckResult result = run(input);
    const FixPlan all = plan(result, {"all"});
    EXPECT_EQ(all.fixes.size(), result.findings.size());
    const FixPlan byRule = plan(result, {"rotation"});
    ASSERT_EQ(byRule.fixes.size(), 1u);
    EXPECT_EQ(byRule.fixes.front().second.id, "default");
    const Finding* rotation = findingOf(result, "rotation");
    const FixPlan second = plan(result, {rotation->key + "@on_grid"});
    ASSERT_EQ(second.fixes.size(), 1u);
    EXPECT_EQ(second.fixes.front().second.id, "on_grid");
    const FixPlan byId = plan(result, {"F1"});
    ASSERT_EQ(byId.fixes.size(), 1u);
    EXPECT_EQ(byId.fixes.front().first, result.findings.front().key);
    const FixPlan unknown = plan(result, {"filtering:scenes/Main.scene#/Nobody:mesh"});
    EXPECT_EQ(unknown.unknown.size(), 1u);
    const FixPlan badOption = plan(result, {rotation->key + "@sideways"});
    EXPECT_EQ(badOption.badOptions.size(), 1u);
}

// D9: a later spec's rule (S46's cleanup_outputs) runs in R10's table position once registered
TEST(PixelArtCheck, RegisteredRulesRunInTheirPlace){
    EXPECT_FALSE(isKnownRule("cleanup_outputs"));
    registerRule("cleanup_outputs", [](const PixelArtCheckInput& input, std::vector<Finding>& findings){
        RuleResult result;
        result.checked = 1;
        Finding finding;
        finding.key = "cleanup_outputs:sprites/hero.png";
        finding.status = RuleStatus::Warn;
        finding.target = {{"kind", "texture"}, {"path", "sprites/hero.png"}};
        finding.message = "stale";
        finding.needsPerson = "forced";
        (void)input;
        findings.push_back(finding);
        return result;
    });
    EXPECT_TRUE(isKnownRule("cleanup_outputs"));
    const CheckResult result = run(cleanInput());
    ASSERT_EQ(result.rules.back().id, "cleanup_outputs");
    EXPECT_EQ(result.rules.back().status, RuleStatus::Warn);
    EXPECT_EQ(result.findings.back().rule, "cleanup_outputs");
    unregisterRule("cleanup_outputs");
    EXPECT_FALSE(isKnownRule("cleanup_outputs"));
}

TEST(PixelArtCheck, RotationFixKeepsFlips){
    // a sprite flipped about Y and turned 37 degrees: only Z of its Euler angles moves
    PixelArtCheckInput input = cleanInput();
    PixelArtDrawable& d = input.scenes.front().drawables.front();
    d.rotation = d.worldRotation = Quaternion(0.0f, 180.0f, 37.0f);
    const CheckResult result = run(input, {"rotation"});
    const Finding* f = findingOf(result, "rotation");
    ASSERT_TRUE(f);
    const FixChange& change = f->options.front().changes.front();
    const Json& from = change.from["euler"];
    const Json& to = change.to["euler"];
    EXPECT_NEAR(to[0].get<double>(), from[0].get<double>(), 1e-3) << change.to.dump();
    EXPECT_NEAR(to[1].get<double>(), from[1].get<double>(), 1e-3) << change.to.dump();
    // the rotation it writes is drawn on a 90-degree step and stays flipped
    Quaternion fixed(to[0].get<float>(), to[1].get<float>(), to[2].get<float>());
    PixelArtCheckInput after = cleanInput();
    after.scenes.front().drawables.front().rotation = after.scenes.front().drawables.front().worldRotation = fixed;
    EXPECT_EQ(statusOf(after, "rotation"), RuleStatus::Pass) << change.to.dump();
    const Vector3 normal = fixed.normalize() * Vector3(0, 0, 1);
    EXPECT_LT(normal.z, -0.99f) << "still facing away: the flip is kept";
}

TEST(PixelArtCheck, MipmapsCompressionReadsTheProjectsTextureSettings){
    PixelArtCheckInput input = cleanInput();
    PixelArtTexture& t = input.textures["sprites/hero.png"];
    t.width = 600;
    t.height = 300;
    EXPECT_EQ(statusOf(input, "mipmaps_compression"), RuleStatus::Pass) << "2048 by default: kept whole";
    // Project Settings > Textures caps every platform at 256: the art is resized (uneven pixels)
    input.project.textureSettings.defaultMaxSize = {256, 256, 256, 256};
    const CheckResult result = run(input, {"mipmaps_compression"});
    const Finding* f = findingOf(result, "mipmaps_compression");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn);
    EXPECT_EQ(f->measured["resized_to"], Json::array({256, 128}));
    // the fix gives the texture a max size that keeps it whole
    EXPECT_EQ(f->options.front().changes.front().to["maxSize"], 1024) << f->options.front().changes.front().to.dump();
}

TEST(PixelArtCheck, ParticlesWithoutAPixelPerfectCameraAreFixable){
    PixelArtCheckInput input = cleanInput();
    PixelArtScene& scene = input.scenes.front();
    scene.camera = PixelArtCamera();          // no camera of its own
    PixelArtDrawable particles;
    particles.kind = DrawableKind::Particles;
    particles.scene = scene.path;
    particles.entity = "/Sparks";
    particles.particleRenderMode = "billboard";
    particles.particleSizes = {4.0f};
    particles.hasTransform = true;
    scene.drawables.push_back(particles);
    const CheckResult result = run(input, {"integer_scale", "particles_grid"});
    const Finding* f = findingOf(result, "particles_grid");
    ASSERT_TRUE(f);
    EXPECT_TRUE(f->needsPerson.empty()) << f->needsPerson;
    ASSERT_FALSE(f->options.empty());
    // the same change as integer_scale's, so fix: ["all"] adds one camera
    const Finding* camera = findingOf(result, "integer_scale");
    ASSERT_TRUE(camera);
    const FixChange& a = f->options.front().changes.front();
    const FixChange& b = camera->options.front().changes.front();
    EXPECT_EQ(a.target, b.target);
    EXPECT_EQ(a.field, "camera");
    EXPECT_EQ(a.field, b.field);
    EXPECT_EQ(a.to, b.to);
}

TEST(PixelArtCheck, RenderScale){
    // S56's default Device Tiers: Low draws at 0.5, Medium at 0.85, stretched back bilinear
    PixelArtCheckInput input = cleanInput();
    for (const char* group : {"desktop", "mobile", "web"}){
        input.project.renderScaleCaps.push_back({group, "low", 0.5f});
        input.project.renderScaleCaps.push_back({group, "medium", 0.85f});
    }
    const CheckResult result = run(input, {"render_scale"});
    const Finding* f = findingOf(result, "render_scale");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, RuleStatus::Warn) << "under a Pixel Perfect Camera the caps draw none of the art";
    EXPECT_EQ(f->measured["device_tiers"]["desktop.low"], 0.5);
    ASSERT_EQ(f->options.front().changes.size(), 6u);
    const FixChange& low = f->options.front().changes.front();
    EXPECT_EQ(low.target, "project");
    EXPECT_EQ(low.field, "quality.device_tiers.desktop.low");
    EXPECT_EQ(low.from, (Json{{"render_scale", 0.5}}));
    EXPECT_EQ(low.to, (Json{{"render_scale", 1}}));
    // a pixel-art scene without one: those devices blur it
    PixelArtCheckInput bare = input;
    bare.scenes.front().camera.hasComponent = false;
    EXPECT_EQ(statusOf(bare, "render_scale"), RuleStatus::Fail);
    // the scene's own render scale or dynamic resolution
    PixelArtCheckInput scaled = cleanInput();
    scaled.scenes.front().camera.hasComponent = false;
    scaled.scenes.front().renderScale = 0.75f;
    scaled.scenes.front().dynamicResolution = true;
    const CheckResult own = run(scaled, {"render_scale"});
    const Finding* sceneFinding = findingOf(own, "render_scale");
    ASSERT_TRUE(sceneFinding);
    EXPECT_EQ(sceneFinding->status, RuleStatus::Fail);
    ASSERT_EQ(sceneFinding->options.front().changes.size(), 2u);
    EXPECT_EQ(sceneFinding->options.front().changes[0].field, "post_processing.render_scale");
    EXPECT_EQ(sceneFinding->options.front().changes[1].field, "post_processing.dynamic_resolution_enabled");
    // red control: every tier at full resolution
    EXPECT_EQ(statusOf(cleanInput(), "render_scale"), RuleStatus::Pass);
    // the preset's project part raises the caps too (pixel_art_mode's fix)
    PixelArtCheckInput off = input;
    off.project.modeEnabled = false;
    off.project.scaling = Scaling::LETTERBOX;
    const CheckResult modeResult = run(off, {"pixel_art_mode"});
    const Finding* mode = findingOf(modeResult, "pixel_art_mode");
    ASSERT_TRUE(mode);
    size_t caps = 0;
    for (const FixChange& change : mode->options.front().changes)
        caps += change.field.rfind("quality.device_tiers.", 0) == 0 ? 1 : 0;
    EXPECT_EQ(caps, 6u);
}
