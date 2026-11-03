// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Bookmarks (S25 R28, AC20): an asset bookmark follows its GUID through an S04 move; a
// GameObject bookmark resolves by its entity id, then its EntityPath, else it is Missing.

#include "organisation/Bookmarks.h"
#include "organisation/OrganisationCommands.h"
#include "organisation/OrganisationTestProject.h"

#include "asset/AssetOps.h"

#include <set>

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

TEST(Bookmarks, ResolvesGameObjectsByIdThenPath) {
    Bookmark bookmark;
    bookmark.kind = Bookmark::Kind::GameObject;
    bookmark.guid = "scene-guid";
    bookmark.path = "old/Main.scene";
    bookmark.entity = 42;
    bookmark.entityPath = "Player/Gun";
    bool sceneOpen = true;
    std::set<uint64_t> entities = {42};
    BookmarkLookups lookups;
    lookups.pathOfGuid = [](const std::string& guid) { return guid == "scene-guid" ? std::string("levels/Main.scene") : std::string(); };
    lookups.exists = [](const std::string&, bool) { return false; };
    lookups.openScene = [&](const std::string& path) -> uint32_t { return sceneOpen && path == "levels/Main.scene" ? 7u : 0u; };
    lookups.hasEntity = [&](uint32_t scene, uint64_t entity) { return scene == 7 && entities.count(entity) > 0; };
    lookups.findByPath = [&](uint32_t scene, const std::string& path) -> uint64_t { return scene == 7 && path == "Player/Gun" ? 99u : 0u; };

    BookmarkTarget target = Bookmarks::resolve(bookmark, lookups);
    EXPECT_TRUE(target.found);
    EXPECT_EQ(target.path, "levels/Main.scene");
    EXPECT_EQ(target.sceneId, 7u);
    EXPECT_EQ(target.entity, 42u);
    // the id is gone: the path finds it
    entities.clear();
    target = Bookmarks::resolve(bookmark, lookups);
    EXPECT_TRUE(target.found);
    EXPECT_EQ(target.entity, 99u);
    // neither: Missing
    bookmark.entityPath = "Player/Sword";
    target = Bookmarks::resolve(bookmark, lookups);
    EXPECT_FALSE(target.found);
    EXPECT_FALSE(target.sceneClosed);
    // the scene is not open: it says so (opening it resolves)
    sceneOpen = false;
    target = Bookmarks::resolve(bookmark, lookups);
    EXPECT_FALSE(target.found);
    EXPECT_TRUE(target.sceneClosed);
    // an unknown GUID falls back to the path hint, when that exists
    Bookmark asset;
    asset.kind = Bookmark::Kind::Asset;
    asset.guid = "unknown";
    asset.path = "textures/hero.png";
    lookups.exists = [](const std::string& path, bool folder) { return !folder && path == "textures/hero.png"; };
    target = Bookmarks::resolve(asset, lookups);
    EXPECT_TRUE(target.found);
    EXPECT_EQ(target.path, "textures/hero.png");
    asset.path = "gone.png";
    EXPECT_FALSE(Bookmarks::resolve(asset, lookups).found);
}

class BookmarksProjectTest : public OrganisationProjectTest {};

TEST_F(BookmarksProjectTest, ResolveAfterMove) {
    const Bookmark bookmark = Bookmarks::forPath(project.get(), "assets/textures/hero.png");
    EXPECT_EQ(bookmark.kind, Bookmark::Kind::Asset);
    ASSERT_FALSE(bookmark.guid.empty());
    EXPECT_EQ(bookmark.name, "hero.png");
    const Bookmark folder = Bookmarks::forPath(project.get(), "assets/textures");
    EXPECT_EQ(folder.kind, Bookmark::Kind::Folder);
    const Bookmark scene = Bookmarks::forPath(project.get(), "Main Scene.scene");
    EXPECT_EQ(scene.kind, Bookmark::Kind::Scene);

    std::filesystem::create_directories(dir.path() / "assets/art");
    const AssetOps::Result moved = AssetOps::move(*project, dir.path() / "assets/textures/hero.png", dir.path() / "assets/art/hero.png");
    ASSERT_TRUE(moved.ok) << moved.error;
    const BookmarkTarget target = Bookmarks::resolve(bookmark, Bookmarks::projectLookups(project.get()));
    EXPECT_TRUE(target.found);
    EXPECT_EQ(target.path, "assets/art/hero.png");
}

TEST_F(BookmarksProjectTest, EditsAreUndoSteps) {
    const Bookmark a = Bookmarks::forPath(project.get(), "assets/textures/hero.png");
    const Bookmark b = Bookmarks::forPath(project.get(), "assets/textures/boss.png");
    ASSERT_TRUE(history().addCommand(new SetBookmarksCmd(project.get(), "Add Bookmark", {a})));
    ASSERT_TRUE(history().addCommand(new SetBookmarksCmd(project.get(), "Add Bookmark", {a, b})));
    ASSERT_TRUE(history().addCommand(new SetBookmarksCmd(project.get(), "Move Bookmark", {b, a})));
    EXPECT_EQ(project->getOrganisation().bookmarks(), (std::vector<Bookmark>{b, a}));
    ASSERT_TRUE(history().addCommand(new SetBookmarksCmd(project.get(), "Remove Bookmark", {b})));
    // organisation.yaml has them
    Organisation reread;
    const std::string file = project->getOrganisation().file().lexically_relative(dir.path()).generic_string();
    EXPECT_EQ(file, ".jasperengine/user/organisation.yaml");
    ASSERT_TRUE(reread.parse(text(file)));
    EXPECT_EQ(reread.bookmarks(), std::vector<Bookmark>{b});
    // undo re-adds the removed one where it was
    ASSERT_TRUE(history().undo());
    EXPECT_EQ(project->getOrganisation().bookmarks(), (std::vector<Bookmark>{b, a}));
    // an unchanged list is no step
    const size_t steps = history().appliedCount();
    history().addCommand(new SetBookmarksCmd(project.get(), "Move Bookmark", {b, a}));
    EXPECT_EQ(history().appliedCount(), steps);
}
