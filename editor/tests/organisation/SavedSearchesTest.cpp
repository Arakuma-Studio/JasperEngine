// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Saved searches (S25 R26, AC19): the structured editor's rows and the R20 grammar say the same
// thing, and organisation.yaml / project.yaml keep them.

#include "organisation/Organisation.h"
#include "spritenormals/SpriteNormalsField.h"
#include "organisation/SavedSearchRows.h"
#include "search/AssetQuery.h"
#include "search/FakeAssetIndex.h"

#include <gtest/gtest.h>

#include "yaml-cpp/yaml.h"

using namespace jasperengine::editor;
using jasperengine::editor::test::FakeAssetIndex;

namespace {
    using Field = SavedSearchRows::Field;
}

TEST(SavedSearches, StructuredRoundTrip) {
    const SavedSearchRows cases[] = {
        {false, {{Field::Type, "texture"}, {Field::Label, "Hero"}}},
        {false, {{Field::Name, "boss room"}, {Field::Extension, "png"}, {Field::Path, "my folder/"}}},
        {false, {{Field::Regex, "^tile_\\d+$"}}},
        {true, {{Field::Type, "model"}, {Field::Label, "Boss Fight"}, {Field::Name, "hero"}}},
        {false, {}},
    };
    for (const SavedSearchRows& rows : cases) {
        const std::string query = rows.toQuery();
        SavedSearchRows back;
        ASSERT_TRUE(SavedSearchRows::fromQuery(query, back)) << query;
        EXPECT_EQ(back, rows) << query;
        // the text the rows make parses to the same query
        EXPECT_EQ(AssetQuery::parse(query).toString(), query);
    }
    EXPECT_EQ((SavedSearchRows{false, {{Field::Type, "texture"}, {Field::Label, "Hero"}}}).toQuery(), "t:texture l:Hero");
    EXPECT_EQ((SavedSearchRows{true, {{Field::Type, "model"}, {Field::Label, "Boss Fight"}}}).toQuery(), "t:model OR l:\"Boss Fight\"");

    // what the rows cannot say opens as text
    SavedSearchRows rows;
    EXPECT_FALSE(SavedSearchRows::fromQuery("ref:textures/hero.png", rows));
    EXPECT_FALSE(SavedSearchRows::fromQuery("t:texture l:Hero OR t:model", rows));
    EXPECT_FALSE(SavedSearchRows::fromQuery("two words", rows));
    EXPECT_TRUE(SavedSearchRows::fromQuery("", rows));
    EXPECT_TRUE(rows.rows.empty());

    // rows and typed text select the same assets
    FakeAssetIndex index;
    index.add("textures/hero.png", "texture", {"Hero"});
    index.add("textures/boss.png", "texture");
    index.add("models/hero.gltf", "model", {"Boss Fight"});
    const SavedSearchRows any{true, {{Field::Type, "texture"}, {Field::Label, "boss fight"}}};
    EXPECT_EQ(index.paths(AssetQuery::parse(any.toQuery()).evaluate(index)).size(), 3u);
    const SavedSearchRows all{false, {{Field::Type, "texture"}, {Field::Label, "hero"}}};
    EXPECT_EQ(index.paths(AssetQuery::parse(all.toQuery()).evaluate(index)), std::vector<std::string>{"textures/hero.png"});
}

TEST(SavedSearches, OrganisationFileRoundTrip) {
    Organisation organisation;
    organisation.setUserSearches({{"Big Textures", "t:texture l:Hero", "textures"}, {"Scripts", "t:script", ""}});
    Bookmark asset;
    asset.kind = Bookmark::Kind::Asset;
    asset.guid = "4b1e0000000000000000000000000000";
    asset.path = "textures/hero.png";
    asset.name = "hero.png";
    Bookmark object;
    object.kind = Bookmark::Kind::GameObject;
    object.guid = "9c1d0000000000000000000000000000";
    object.path = "Main Scene.scene";
    object.entity = 42;
    object.entityPath = "Player/Gun";
    object.name = "Gun";
    Bookmark folder;
    folder.kind = Bookmark::Kind::Folder;
    folder.path = "textures";
    folder.name = "textures";
    organisation.setBookmarks({asset, object, folder});
    organisation.noteRecent("command:edit.duplicate");
    organisation.noteRecent("asset:4b1e");
    organisation.noteRecent("command:edit.duplicate");
    EXPECT_EQ(organisation.recents(), (std::vector<std::string>{"command:edit.duplicate", "asset:4b1e"}));

    Organisation back;
    std::string error;
    ASSERT_TRUE(back.parse(organisation.serialize(), &error)) << error;
    EXPECT_EQ(back.userSearches(), organisation.userSearches());
    EXPECT_EQ(back.bookmarks(), organisation.bookmarks());
    EXPECT_EQ(back.recents(), organisation.recents());
    EXPECT_EQ(back.findSearch("Big Textures")->folder, "textures");
    // the built-in Favorites are found by name and are read-only
    const SavedSearch* materials = back.findSearch("All Materials");
    ASSERT_NE(materials, nullptr);
    EXPECT_TRUE(materials->builtin);
    EXPECT_EQ(Organisation::builtinSearches().size(), 5u);

    // project.yaml's list
    YAML::Node list;
    Organisation::encodeSearches({{"Shared", "l:Boss", ""}}, list);
    const std::vector<SavedSearch> shared = Organisation::decodeSearches(YAML::Load(YAML::Dump(list)), true);
    ASSERT_EQ(shared.size(), 1u);
    EXPECT_EQ(shared[0].name, "Shared");
    EXPECT_EQ(shared[0].query, "l:Boss");
    EXPECT_TRUE(shared[0].project);

    // recents are capped
    for (int i = 0; i < 30; i++) organisation.noteRecent("command:" + std::to_string(i));
    EXPECT_EQ(organisation.recents().size(), Organisation::MAX_RECENTS);
    EXPECT_EQ(organisation.recents().front(), "command:29");

    // a newer or broken file is refused
    EXPECT_FALSE(back.parse("[1, 2]", &error));
}
