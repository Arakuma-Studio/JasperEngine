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

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

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
