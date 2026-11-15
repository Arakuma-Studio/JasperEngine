// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a build ships (docs/specs/S98-editor-tooling-gaps.md REQ-8 to REQ-12, REQ-50): the globs of
// Always Include and Exclude From Build, the string literals a Lua or C++ source holds, the closure
// Referenced Only keeps, the project.yaml keys (absent: Ship All Assets, nothing written) and the
// undo step of the window and the Project window.

#include "asset/FileOpsTestProject.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "asset/TextReferences.h"
#include "command/type/AssetStrippingSettingsCmd.h"
#include "export/AssetStripping.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(AssetStripping, GlobsMatchFilesFoldersAndWildcards) {
    EXPECT_TRUE(buildglob::matches("assets/ui/logo.png", "assets/ui/logo.png"));
    EXPECT_FALSE(buildglob::matches("assets/ui/logo.png", "assets/ui/logo.png.bak"));
    // a glob without wildcards names a folder's contents too
    EXPECT_TRUE(buildglob::matches("assets/levels", "assets/levels/one/map.png"));
    EXPECT_TRUE(buildglob::matches("assets/levels/", "assets/levels/map.png"));
    EXPECT_FALSE(buildglob::matches("assets/levels", "assets/levels2/map.png"));
    // * stays in one folder, ** crosses them, ? is one character
    EXPECT_TRUE(buildglob::matches("assets/*.png", "assets/a.png"));
    EXPECT_FALSE(buildglob::matches("assets/*.png", "assets/sub/a.png"));
    EXPECT_TRUE(buildglob::matches("assets/**/*.png", "assets/a.png"));
    EXPECT_TRUE(buildglob::matches("assets/**/*.png", "assets/x/y/a.png"));
    EXPECT_TRUE(buildglob::matches("assets/dev/**", "assets/dev/a/b.ogg"));
    EXPECT_TRUE(buildglob::matches("assets/level?.json", "assets/level7.json"));
    EXPECT_FALSE(buildglob::matches("assets/level?.json", "assets/level17.json"));
    EXPECT_EQ(buildglob::forSelection("assets/levels", true), "assets/levels/**");
    EXPECT_EQ(buildglob::forSelection("assets/a.png", false), "assets/a.png");

    std::string error;
    EXPECT_TRUE(buildglob::isValid("assets/**", error));
    EXPECT_FALSE(buildglob::isValid("", error));
    EXPECT_FALSE(buildglob::isValid("/abs/path", error));
    EXPECT_FALSE(buildglob::isValid("../outside/**", error));
    EXPECT_FALSE(buildglob::isValid("assets\\a.png", error));
    EXPECT_NE(error.find("'/'"), std::string::npos) << error;
}

TEST(AssetStripping, StringLiteralsOfLuaAndCppSkipComments) {
    const std::string lua = "local t = Texture(\"ui/logo.png\") -- \"ui/commented.png\"\n"
                            "local s = 'sounds/hit.ogg'\n"
                            "--[[ 'ui/block.png' ]]\n"
                            "local l = [[data/long.json]]\n"
                            "local e = \"a\\\"b.png\"\n";
    EXPECT_EQ(TextReferences::scanStringLiterals(lua, true),
              (std::vector<std::string>{"ui/logo.png", "sounds/hit.ogg", "data/long.json", "a\\\"b.png"}));
    const std::string cpp = "// \"ui/commented.png\"\nauto t = load(\"ui/logo.png\"); /* \"x.png\" */ char c = 'q';\n";
    EXPECT_EQ(TextReferences::scanStringLiterals(cpp, false), (std::vector<std::string>{"ui/logo.png", "q"}));
}

TEST(AssetStripping, BuildGlobToggles) {
    AssetStrippingSettings settings;
    settings = withBuildGlob(settings, true, "assets/dev/**", true);
    settings = withBuildGlob(settings, true, "assets/dev/**", true);
    EXPECT_EQ(settings.exclude, std::vector<std::string>{"assets/dev/**"});
    settings = withBuildGlob(settings, false, "assets/levels/**", true);
    EXPECT_EQ(settings.alwaysInclude, std::vector<std::string>{"assets/levels/**"});
    settings = withBuildGlob(settings, true, "assets/dev/**", false);
    EXPECT_TRUE(settings.exclude.empty());
}

namespace {

    // the prefab-runtime fixture (assets dir "assets", Lua dir "scripts") with textures a scene,
    // a Lua literal, an HTML document, project.yaml or nothing names
    class AssetStrippingProjectTest : public editor::test::FileOpsProjectTest {
    protected:
        void prepare() override {
            dir.write("assets/textures/used_by_lua.png", "png");
            dir.write("assets/textures/unused.png", "png-unused");
            dir.write("assets/textures/from_html.png", "png");
            dir.write("assets/textures/icon.png", "png");
            dir.write("assets/textures/computed_1.png", "png");
            dir.write("assets/dev/debug.png", "png");
            dir.write("assets/ui/menu.html", "<html><body><img src=\"../textures/from_html.png\"></body></html>\n");
            dir.write("assets/data/levels.json", "{\"next\": \"textures/icon.png\"}\n");
            dir.write("scripts/Loader.lua", "local t = Texture('textures/used_by_lua.png')\nlocal n = 'textures/computed_' .. 1\n");
            std::string yaml = dir.read("project.yaml");
            yaml += "windowIcon: textures/icon.png\n";
            dir.write("project.yaml", yaml);
        }
    };

}

TEST_F(AssetStrippingProjectTest, ShipAllLeavesOutOnlyExclusions) {
    AssetStrippingSettings settings;
    settings.exclude = {"assets/dev/**"};
    project->setAssetStripping(settings);
    const AssetStrippingPlan plan = AssetStrippingPlan::make(*project);
    EXPECT_FALSE(plan.referencedOnly());
    EXPECT_TRUE(plan.referenced().empty()) << "Ship All Assets runs no closure (REQ-43)";
    EXPECT_EQ(plan.leftOutReason("assets/textures/unused.png"), "");
    EXPECT_EQ(plan.leftOutReason("assets/dev/debug.png"), "excluded: assets/dev/**");
}

TEST_F(AssetStrippingProjectTest, ReferencedOnlyKeepsWhatTheBuildNames) {
    project->getAssetIndex().waitUntilReady();
    AssetStrippingSettings settings;
    settings.mode = AssetStrippingMode::ReferencedOnly;
    settings.alwaysInclude = {"assets/ui/**", "assets/dev/**"};
    settings.exclude = {"assets/dev/**"};
    project->setAssetStripping(settings);
    const AssetStrippingPlan plan = AssetStrippingPlan::make(*project);
    ASSERT_TRUE(plan.referencedOnly());
    // a Lua literal, an Always Include document's image, project.yaml's icon, a data file's literal
    EXPECT_EQ(plan.leftOutReason("assets/textures/used_by_lua.png"), "");
    EXPECT_EQ(plan.leftOutReason("assets/ui/menu.html"), "");
    EXPECT_EQ(plan.leftOutReason("assets/textures/from_html.png"), "");
    EXPECT_EQ(plan.leftOutReason("assets/textures/icon.png"), "");
    // the scene's prefabs
    EXPECT_EQ(plan.leftOutReason("prefabs/Enemy.prefab"), "");
    EXPECT_EQ(plan.leftOutReason("prefabs/Bullet.prefab"), "");
    // nothing names these: a computed path needs Always Include
    EXPECT_EQ(plan.leftOutReason("assets/textures/unused.png"), "unreferenced");
    EXPECT_EQ(plan.leftOutReason("assets/textures/computed_1.png"), "unreferenced");
    // Exclude wins over Always Include
    EXPECT_EQ(plan.leftOutReason("assets/dev/debug.png"), "excluded: assets/dev/**");
    // Lua sources and data files always ship
    EXPECT_EQ(plan.leftOutReason("scripts/Loader.lua"), "");
    EXPECT_EQ(plan.leftOutReason("assets/data/levels.json"), "");
}

// A scene never saved ships as the editor holds it (S98 REQ-4) and has no file the index read:
// what it names ships
TEST_F(AssetStrippingProjectTest, ReferencedOnlyKeepsWhatANeverSavedSceneNames) {
    project->getAssetIndex().waitUntilReady();
    AssetStrippingSettings settings;
    settings.mode = AssetStrippingMode::ReferencedOnly;
    project->setAssetStripping(settings);
    if (SceneProject* current = project->getSelectedScene()) current->isModified = false;
    project->createNewScene("Unsaved", SceneType::SCENE_3D);
    SceneProject* unsaved = project->getScene(project->getSelectedSceneId());
    ASSERT_NE(unsaved, nullptr);
    ASSERT_TRUE(unsaved->filepath.empty());
    Scene* scene = unsaved->scene;
    const Entity entity = scene->createUserEntity();
    scene->addComponent<Transform>(entity, {});
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].material.baseColorTexture.setPath("textures/unused.png");
    scene->addComponent<MeshComponent>(entity, mesh);
    unsaved->entities.push_back(entity);
    EXPECT_EQ(AssetStrippingPlan::make(*project).leftOutReason("assets/textures/unused.png"), "");
    EXPECT_EQ(AssetStrippingPlan::make(*project).leftOutReason("assets/textures/computed_1.png"), "unreferenced");
}

TEST_F(AssetStrippingProjectTest, SettingsAreOneUndoStep) {
    AssetStrippingSettings settings;
    settings.mode = AssetStrippingMode::ReferencedOnly;
    settings.alwaysInclude = {"assets/levels/**"};
    ASSERT_TRUE(history().addCommand(new AssetStrippingSettingsCmd(project.get(), settings)));
    EXPECT_EQ(project->getAssetStripping(), settings);
    EXPECT_NE(text("project.yaml").find("assetStripping: referencedOnly"), std::string::npos) << text("project.yaml");
    ASSERT_TRUE(history().undo());
    EXPECT_EQ(project->getAssetStripping(), AssetStrippingSettings());
    EXPECT_EQ(text("project.yaml").find("assetStripping"), std::string::npos) << text("project.yaml");
}

namespace {

    class StreamProjectSettingsTest : public editor::test::FileOpsProjectTest {
    protected:
        bool withKeys = false;
        void prepare() override {
            if (!withKeys) return;
            std::string yaml = dir.read("project.yaml");
            yaml += "export:\n  assetStripping: referencedOnly\n  alwaysInclude:\n    - assets/levels/**\n  exclude:\n    - assets/dev/**\n";
            dir.write("project.yaml", yaml);
        }
    };

    class StreamProjectSettingsWithKeysTest : public StreamProjectSettingsTest {
    protected:
        void SetUp() override {
            withKeys = true;
            StreamProjectSettingsTest::SetUp();
        }
    };

}

// REQ-50, AC35: an old project.yaml means Ship All Assets with empty lists, and saving it writes none of the keys
TEST_F(StreamProjectSettingsTest, NewBuildKeysDefault) {
    EXPECT_EQ(project->getAssetStripping(), AssetStrippingSettings());
    EXPECT_EQ(project->getAssetStripping().mode, AssetStrippingMode::ShipAll);
    ASSERT_TRUE(project->saveProjectFile());
    const std::string yaml = text("project.yaml");
    EXPECT_EQ(yaml.find("assetStripping"), std::string::npos) << yaml;
    EXPECT_EQ(yaml.find("alwaysInclude"), std::string::npos) << yaml;
    EXPECT_EQ(yaml.find("exclude:"), std::string::npos) << yaml;
}

TEST_F(StreamProjectSettingsWithKeysTest, BuildKeysRoundTrip) {
    const AssetStrippingSettings& settings = project->getAssetStripping();
    EXPECT_EQ(settings.mode, AssetStrippingMode::ReferencedOnly);
    EXPECT_EQ(settings.alwaysInclude, std::vector<std::string>{"assets/levels/**"});
    EXPECT_EQ(settings.exclude, std::vector<std::string>{"assets/dev/**"});
    ASSERT_TRUE(project->saveProjectFile());
    const std::string yaml = text("project.yaml");
    EXPECT_NE(yaml.find("assetStripping: referencedOnly"), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("assets/levels/**"), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("assets/dev/**"), std::string::npos) << yaml;
}
