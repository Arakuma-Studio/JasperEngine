// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The titleart module in builds (docs/specs/S81-title-art-runtime.md AC14: R51, R52, R59, R67):
// only an enabled Realtime or Glyph Atlas Title Art Renderer needs it, with evidence naming the
// title; Baked titles and scripts naming TitleArt keep the smallest variants; the variants that
// carry it.

#include "players/ModuleUsage.h"
#include "players/PlayerRuntimes.h"
#include "players/ShaderNeutralApi.h"
#include "player/PlayerModules.h"

#include "Scene.h"
#include "component/TitleArtRendererComponent.h"
#include "component/Transform.h"
#include "titleart/TitleArtDoc.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    ModuleUsage usageOf(TitleArtRenderMode mode, bool enabled = true){
        Scene scene;
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.setEntityName(entity, "Logo");
        TitleArtRendererComponent title;
        title.title = AssetRef("assets/titles/missing.titleart");
        title.renderMode = mode;
        title.enabled = enabled;
        scene.addComponent<TitleArtRendererComponent>(entity, std::move(title));
        ModuleScan scan;
        scan.navigationInBuilds = false;
        scan.contents.push_back({"prefab Logo", &scene, {entity}});
        return ModuleUsageDetector::analyze(scan);
    }

}

TEST(TitleArtModuleUsage, BakedTitlesNeedNoModule) {
    EXPECT_FALSE(usageOf(TitleArtRenderMode::BAKED).needs("titleart"));
    EXPECT_FALSE(usageOf(TitleArtRenderMode::REALTIME, false).needs("titleart")) << "a disabled title renders nothing";
}

TEST(TitleArtModuleUsage, RealtimeAndGlyphAtlasTitlesNeedItWithEvidence) {
    const ModuleUsage realtime = usageOf(TitleArtRenderMode::REALTIME);
    ASSERT_TRUE(realtime.needs("titleart"));
    EXPECT_EQ(realtime.firstEvidence("titleart"), "prefab Logo: Title Art Renderer (Realtime) on Logo");
    const ModuleUsage atlas = usageOf(TitleArtRenderMode::GLYPH_ATLAS);
    ASSERT_TRUE(atlas.needs("titleart"));
    EXPECT_NE(atlas.firstEvidence("titleart").find("Glyph Atlas"), std::string::npos);
    // a document that cannot be read has no 3D layer: the sub-options stay off
    EXPECT_FALSE(realtime.titleArt3D);
    EXPECT_FALSE(realtime.titleArtMesher);
}

TEST(TitleArtModuleUsage, ScriptsNamingTitleArtKeepTheSmallestVariant) {
    ModuleScan scan;
    scan.navigationInBuilds = false;
    scan.luaScripts.push_back({"lua/hud.lua",
                               "local t = self.gameObject:getTitleArt()\nif t ~= nil then t.text = 'WIN'; t:setParameter('glow', 1); "
                               "t.onReady:add('x', function() end) end\nlocal other = TitleArt(self.scene, self.entity)\n"});
    const ModuleUsage usage = ModuleUsageDetector::analyze(scan);
    EXPECT_FALSE(usage.needs("titleart")) << "R67: Lua cannot make a Realtime title, components alone decide";
    EXPECT_FALSE(usage.needs("runtimeshaders")) << "TitleArt is reviewed shader neutral";
    EXPECT_TRUE(ShaderNeutralApi::isNeutralClass("TitleArt"));
    EXPECT_TRUE(ShaderNeutralApi::mixedClasses().at("GameObject").count("getTitleArt"));
    EXPECT_EQ(PlayerRuntimes::variantFor(usage.list()).name, "lite2d");
}

TEST(TitleArtModuleUsage, VariantsCarryTheModule) {
    const std::vector<std::string>& all = player::allModules();
    EXPECT_NE(std::find(all.begin(), all.end(), "titleart"), all.end());
    EXPECT_EQ(player::moduleDisplayName("titleart"), "Title Art (Realtime and Glyph Atlas)");
    for (const PlayerVariant& variant : PlayerRuntimes::variants()){
        const bool has = std::find(variant.modules.begin(), variant.modules.end(), "titleart") != variant.modules.end();
        const bool lite = variant.name == "lite2d" || variant.name == "lite3d";
        EXPECT_EQ(has, !lite) << variant.name << ": full, ui and core carry titleart, the lite ones do not (R59)";
    }
    // R59: a Realtime title takes the smallest variant with the module
    EXPECT_EQ(PlayerRuntimes::variantFor({"titleart"}).name, "core");
    EXPECT_EQ(PlayerRuntimes::variantFor({"titleart", "physics2d"}).name, "core");
    // the editor's engine has it
    EXPECT_TRUE(player::isModuleBuilt("titleart"));
}

namespace {

    // a Realtime title of a document with one Dimension 3D plate, `exposedField` (when set) an
    // Exposed number on it
    ModuleUsage usageOf3D(const char* exposedField){
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / ("jasper-titleart-modules-test-" + std::to_string(::getpid()));
        fs::create_directories(dir);
        const fs::path file = dir / (std::string(exposedField ? "dynamic" : "fixed") + ".titleart");
        titleart::Json doc = {{"format", "jasperengine.titleart"}, {"version", 1}, {"canvas", {{"width", 256}, {"height", 128}}},
                              {"layers", titleart::Json::array({{{"id", "00000001"}, {"type", "shape"}, {"name", "Logo"}, {"shape", "rect"},
                                                                {"size", 100}, {"dimension", "3d"}}})}};
        if (exposedField){
            doc["exposed"] = titleart::Json::array({{{"name", "depth"}, {"type", "number"},
                                                     {"targets", titleart::Json::array({{{"layer", "00000001"}, {"field", exposedField}}})}}});
        }
        std::ofstream(file) << doc.dump();
        Scene scene;
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.setEntityName(entity, "Logo");
        TitleArtRendererComponent title;
        title.title = AssetRef(file.string());
        title.renderMode = TitleArtRenderMode::REALTIME;
        scene.addComponent<TitleArtRendererComponent>(entity, std::move(title));
        ModuleScan scan;
        scan.navigationInBuilds = false;
        scan.contents.push_back({"scene Main", &scene, {entity}});
        return ModuleUsageDetector::analyze(scan);
    }

}

TEST(TitleArtModuleUsage, ThreeDSubOptions) {
    // S81 R51, S86 R38: a fixed 3D title needs the 3D renderer but not the mesher (it ships cooked
    // meshes); an Exposed parameter that changes its geometry needs the mesher, and the evidence
    // names the title, the layer and the parameter; one that only turns it does not
    const ModuleUsage fixed = usageOf3D(nullptr);
    EXPECT_TRUE(fixed.titleArt3D);
    EXPECT_FALSE(fixed.titleArtMesher);
    bool named = false;
    for (const ModuleEvidence& e : fixed.evidenceOf("titleart")) named = named || e.what.find("a 3D title") != std::string::npos;
    EXPECT_TRUE(named);
    const ModuleUsage turned = usageOf3D("transform.rotation_y");
    EXPECT_TRUE(turned.titleArt3D);
    EXPECT_FALSE(turned.titleArtMesher);
    const ModuleUsage dynamic = usageOf3D("style_3d.depth");
    EXPECT_TRUE(dynamic.titleArt3D);
    EXPECT_TRUE(dynamic.titleArtMesher);
    std::string why;
    for (const ModuleEvidence& e : dynamic.evidenceOf("titleart"))
        if (e.what.find("meshes at runtime") != std::string::npos) why = e.what;
    EXPECT_NE(why.find("'Logo'"), std::string::npos) << why;
    EXPECT_NE(why.find("depth"), std::string::npos) << why;
    EXPECT_NE(why.find("Title Art Renderer (Realtime) on Logo"), std::string::npos) << why;
}
