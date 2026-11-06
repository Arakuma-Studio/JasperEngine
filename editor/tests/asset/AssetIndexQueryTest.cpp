// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "asset/AssetIndex.h"
#include "TempDir.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(AssetIndex, ReferenceQueriesKeepPathOrderPrefixesAndOverlayLimits) {
    jasperengine::test::TempDir project;
    project.write("project.yaml", "name: Reference Queries\nscenes: []\n");
    project.write("assets/textures/picture.png", "texture");
    const std::string first = "assets/interface-with-a-long-folder-name/alpha-page.html";
    const std::string second = "assets/interface-with-a-long-folder-name/bravo-page.html";
    const std::string sibling = "assets/interface-with-a-long-folder-name/charlie-page.html";
    project.write(first, "<img src=\"../textures/picture.png\">\n");
    project.write(second, "<img src=\"../textures/picture.png\">\n");
    project.write(sibling, "<img src=\"../textures-copy/picture.png\">\n");

    AssetDatabase database;
    database.setReadOnly(true);
    database.setBackgroundHashing(false);
    database.open(project.path(), "assets");
    AssetIndex index;
    index.open(&database, project.path(), {"assets", "scripts"});
    index.waitUntilReady();
    const Guid texture = database.getAssetGuid("assets/textures/picture.png");
    ASSERT_FALSE(texture.isNull());

    const auto references = index.referencesTo(texture, "assets/textures/picture.png", false, 100);
    ASSERT_EQ(references.size(), 2u);
    EXPECT_EQ(index.referrerPath(references[0].referrer), first);
    EXPECT_EQ(index.referrerPath(references[1].referrer), second);
    EXPECT_EQ(index.referencesTo(Guid(), "assets/textures", false, 100), references);
    EXPECT_TRUE(index.referencesTo(texture, "assets/textures/picture.png", false, 0).empty());
    EXPECT_EQ(index.referencesTo(texture, "assets/textures/picture.png", false, 1),
              std::vector<DepEdge>{references[0]});

    const Guid unsaved(123, 456);
    const Guid replaced = database.getAssetGuid(first);
    DepEdge added = references[0];
    added.referrer = unsaved;
    added.field = "unsaved/reference";
    index.setOverlayProvider([&](std::unordered_map<Guid, std::vector<DepEdge>>& memory) {
        memory[replaced] = {};
        memory[unsaved] = {added};
    });
    EXPECT_EQ(index.referencesTo(texture, "assets/textures/picture.png", false, 100),
              (std::vector<DepEdge>{added, references[1]}));
    EXPECT_EQ(index.referencesTo(texture, "assets/textures/picture.png", false, 1),
              std::vector<DepEdge>{added});
}
