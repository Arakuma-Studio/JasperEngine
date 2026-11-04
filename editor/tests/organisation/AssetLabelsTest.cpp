// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Asset labels (S25 R22, R23; AC17): where they are stored (the meta sidecar's labels: section,
// an in-file document's top-level key), that S04's data survives, that saving an open scene
// keeps them, and that one edit is one undo step.

#include "organisation/AssetLabels.h"
#include "Project.h"
#include "organisation/LabelOperations.h"
#include "organisation/OrganisationCommands.h"
#include "organisation/OrganisationTestProject.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include "yaml-cpp/yaml.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

TEST(AssetLabels, NormalizesSortsAndFormats) {
    std::string label;
    std::string why;
    EXPECT_TRUE(AssetLabels::normalizeLabel("  Boss Fight ", label, &why));
    EXPECT_EQ(label, "Boss Fight");
    EXPECT_FALSE(AssetLabels::normalizeLabel("", label, &why));
    EXPECT_FALSE(AssetLabels::normalizeLabel("bad/char", label, &why));
    EXPECT_FALSE(AssetLabels::normalizeLabel(std::string(33, 'a'), label, &why));
    EXPECT_TRUE(AssetLabels::normalizeLabel("v1.2_final-cut", label, &why));
    EXPECT_TRUE(AssetLabels::sameLabel("Hero", "hERO"));
    EXPECT_EQ(AssetLabels::sortedUnique({"hero", "Boss Fight", "Hero", "audio"}), (std::vector<std::string>{"audio", "Boss Fight", "hero"}));
    EXPECT_EQ(AssetLabels::formatList({"Boss Fight", "Hero"}), "[Boss Fight, Hero]");
    EXPECT_EQ(AssetLabels::parseList("[Boss Fight, Hero]"), (std::vector<std::string>{"Boss Fight", "Hero"}));
    EXPECT_EQ(AssetLabels::parseList("[]"), std::vector<std::string>{});
    // colours: a stable default of the palette, or #RRGGBB
    EXPECT_EQ(AssetLabels::defaultPalette().size(), 9u);
    EXPECT_EQ(AssetLabels::paletteColorFor("Hero"), AssetLabels::paletteColorFor("hero"));
    EXPECT_TRUE(AssetLabels::isColor(AssetLabels::paletteColorFor("Hero")));
    EXPECT_TRUE(AssetLabels::isColor("#a1B2c3"));
    EXPECT_FALSE(AssetLabels::isColor("#12345"));
    EXPECT_FALSE(AssetLabels::isColor("red"));
}

TEST(AssetLabels, InFileLineEditKeepsTheDocument) {
    const std::string scene = "# a comment\nguid: 0123456789abcdef0123456789abcdef\nformerGuids:\n  - fedcba9876543210fedcba9876543210\nname: Main\nentities: []\n";
    const std::string labelled = AssetLabels::withInFileLabels(scene, {"Boss Fight", "Hero"});
    EXPECT_EQ(labelled, "# a comment\nguid: 0123456789abcdef0123456789abcdef\nformerGuids:\n  - fedcba9876543210fedcba9876543210\n"
                        "labels: [Boss Fight, Hero]\nname: Main\nentities: []\n");
    EXPECT_EQ(AssetLabels::readInFileLabels(labelled), (std::vector<std::string>{"Boss Fight", "Hero"}));
    // replaced, then removed: the original back byte for byte
    const std::string replaced = AssetLabels::withInFileLabels(labelled, {"Music"});
    EXPECT_EQ(AssetLabels::readInFileLabels(replaced), std::vector<std::string>{"Music"});
    EXPECT_EQ(AssetLabels::withInFileLabels(replaced, {}), scene);
    // a block list written by hand is read and replaced too
    const std::string block = "guid: 0123456789abcdef0123456789abcdef\nlabels:\n  - One\n  - Two\nname: X\n";
    EXPECT_EQ(AssetLabels::readInFileLabels(block), (std::vector<std::string>{"One", "Two"}));
    EXPECT_EQ(AssetLabels::withInFileLabels(block, {"Three"}), "guid: 0123456789abcdef0123456789abcdef\nlabels: [Three]\nname: X\n");
    // CRLF documents stay CRLF
    const std::string crlf = "guid: 0123456789abcdef0123456789abcdef\r\nname: X\r\n";
    EXPECT_EQ(AssetLabels::withInFileLabels(crlf, {"A"}), "guid: 0123456789abcdef0123456789abcdef\r\nlabels: [A]\r\nname: X\r\n");
}

class AssetLabelsProjectTest : public OrganisationProjectTest {};

TEST_F(AssetLabelsProjectTest, MetaAndInFile) {
    const Guid hero = guidOf("assets/textures/hero.png");
    const Guid enemy = guidOf("prefabs/Enemy.prefab");
    ASSERT_FALSE(hero.isNull());
    ASSERT_FALSE(enemy.isNull());
    const std::string heroMetaBefore = text("assets/textures/hero.png.meta");
    const std::string enemyBefore = text("prefabs/Enemy.prefab");
    ASSERT_FALSE(heroMetaBefore.empty());

    auto* command = new SetAssetLabelsCmd(project.get(), "Set Asset Labels", {{hero, {}}, {enemy, {}}},
                                          {{hero, {"Hero", "Boss Fight"}}, {enemy, {"Hero"}}});
    ASSERT_TRUE(history().addCommand(command));
    ASSERT_NE(history().top(), nullptr);
    EXPECT_EQ(history().top()->name(), "Set Asset Labels");

    // the sidecar's labels: section, with S04's identity and import data kept
    const YAML::Node meta = YAML::Load(text("assets/textures/hero.png.meta"));
    ASSERT_TRUE(meta["labels"]) << text("assets/textures/hero.png.meta");
    EXPECT_EQ(meta["labels"].as<std::vector<std::string>>(), (std::vector<std::string>{"Boss Fight", "Hero"}));
    const YAML::Node metaBefore = YAML::Load(heroMetaBefore);
    for (const auto& entry : metaBefore) {
        const std::string key = entry.first.as<std::string>();
        EXPECT_TRUE(meta[key]) << key << " was lost";
        EXPECT_EQ(YAML::Dump(meta[key]), YAML::Dump(entry.second)) << key;
    }
    // the prefab's own top-level key, the rest of the document unchanged
    const std::string enemyAfter = text("prefabs/Enemy.prefab");
    EXPECT_EQ(AssetLabels::readInFileLabels(enemyAfter), std::vector<std::string>{"Hero"});
    EXPECT_EQ(AssetLabels::withInFileLabels(enemyAfter, {}), enemyBefore);
    EXPECT_EQ(project->getAssetLabels().labelsOf(enemy), std::vector<std::string>{"Hero"});

    // read back from disk by a fresh store (the background load)
    {
        Project reopened;
        reopened.setSkipProjectLock(true);
        ASSERT_TRUE(reopened.loadProject(dir.path(), false));
        reopened.getAssetLabels().waitUntilReady();
        reopened.getAssetLabels().update();
        EXPECT_EQ(reopened.getAssetLabels().labelsOf(hero), (std::vector<std::string>{"Boss Fight", "Hero"}));
        EXPECT_EQ(reopened.getAssetLabels().labelsOf(enemy), std::vector<std::string>{"Hero"});
        closeProject(reopened);
    }

    // one undo puts both files back
    ASSERT_TRUE(history().undo());
    EXPECT_EQ(text("prefabs/Enemy.prefab"), enemyBefore);
    EXPECT_FALSE(YAML::Load(text("assets/textures/hero.png.meta"))["labels"]);
    EXPECT_TRUE(project->getAssetLabels().labelsOf(hero).empty());
    ASSERT_TRUE(history().redo());
    EXPECT_EQ(project->getAssetLabels().labelsOf(hero), (std::vector<std::string>{"Boss Fight", "Hero"}));
}

TEST_F(AssetLabelsProjectTest, SavingAnOpenSceneKeepsItsLabels) {
    SceneProject* scene = nullptr;
    for (SceneProject& candidate : project->getScenes()) {
        if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
    }
    for (SceneProject& candidate : project->getScenes()) {
        if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
    }
    ASSERT_NE(scene, nullptr);
    const std::string relative = project->normalizeToProjectRelative(scene->filepath).generic_string();
    const Guid guid = guidOf(relative);
    ASSERT_FALSE(guid.isNull()) << relative;
    scene->isModified = false;
    ASSERT_TRUE(history().addCommand(new SetAssetLabelsCmd(project.get(), "Set Asset Labels", {{guid, {}}}, {{guid, {"Level"}}})));
    // a label edit does not make the scene modified
    EXPECT_FALSE(scene->isModified);
    EXPECT_EQ(AssetLabels::readInFileLabels(text(relative)), std::vector<std::string>{"Level"});

    scene->isModified = true;
    std::string error;
    ASSERT_TRUE(project->saveSceneNow(scene->id, {}, &error)) << error;
    EXPECT_EQ(AssetLabels::readInFileLabels(text(relative)), std::vector<std::string>{"Level"});
}

TEST_F(AssetLabelsProjectTest, RenameDeleteAndColorAreOneStepEach) {
    const Guid hero = guidOf("assets/textures/hero.png");
    const Guid boss = guidOf("assets/textures/boss.png");
    ASSERT_TRUE(history().addCommand(new SetAssetLabelsCmd(project.get(), "Set Asset Labels", {{hero, {}}, {boss, {}}},
                                                           {{hero, {"Hero"}}, {boss, {"hero", "Boss"}}})));
    const size_t steps = history().appliedCount();

    LabelOperations::Result renamed = LabelOperations::rename(project.get(), "HERO", "Protagonist");
    ASSERT_TRUE(renamed.ok) << renamed.error;
    EXPECT_EQ(renamed.assets, 2u);
    EXPECT_GE(renamed.filesWritten.size(), 2u);
    EXPECT_EQ(history().appliedCount(), steps + 1);
    EXPECT_EQ(history().top()->name(), "Rename Label");
    EXPECT_EQ(project->getAssetLabels().labelsOf(boss), (std::vector<std::string>{"Boss", "Protagonist"}));

    LabelOperations::Result colored = LabelOperations::setColor(project.get(), "Boss", "#112233");
    ASSERT_TRUE(colored.ok) << colored.error;
    EXPECT_EQ(project->getAssetLabels().colorOf("boss"), "#112233");
    EXPECT_FALSE(LabelOperations::setColor(project.get(), "Boss", "blue").ok);

    LabelOperations::Result removed = LabelOperations::remove(project.get(), "protagonist");
    ASSERT_TRUE(removed.ok) << removed.error;
    EXPECT_TRUE(project->getAssetLabels().labelsOf(hero).empty());
    EXPECT_FALSE(LabelOperations::remove(project.get(), "Nobody").ok);

    // each undoes in one step
    ASSERT_TRUE(history().undo());
    EXPECT_EQ(project->getAssetLabels().labelsOf(hero), std::vector<std::string>{"Protagonist"});
    ASSERT_TRUE(history().undo());
    EXPECT_NE(project->getAssetLabels().colorOf("boss"), "#112233");
    ASSERT_TRUE(history().undo());
    EXPECT_EQ(project->getAssetLabels().labelsOf(boss), (std::vector<std::string>{"Boss", "hero"}));
}
