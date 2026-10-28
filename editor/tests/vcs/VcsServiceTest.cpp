// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// VcsService (docs/specs/S29-version-control.md R5-R8, AC20): repository detection from the
// project folder up, a machine without the tools, the snapshot's meta pairing, and the poller's
// wakes.

#include "VcsTestSupport.h"

#include "vcs/VcsEditorOps.h"

using namespace jasperengine;
using namespace jasperengine::editor::test;

TEST(VcsService, DetectsTheNearestRepositoryAbove) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"game/project.yaml", "name: Game\n"}, {"outside.txt", "o\n"}});
    VcsService service;
    service.open(repo / "game", repo / "game" / ".jasperengine", box.settings(), false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.state() != VcsState::Detecting; }));
    ASSERT_EQ(service.state(), VcsState::Active) << service.stateMessage();
    EXPECT_EQ(service.caps().provider, "git");
    EXPECT_EQ(service.repoRoot(), repo);
    EXPECT_EQ(service.projectKey(), "game");
    EXPECT_NE(service.stateMessage().find("Git "), std::string::npos);
    // status is limited to the project folder
    writeFile(repo / "outside.txt", "changed\n");
    writeFile(repo / "game" / "inside.txt", "new\n");
    const SnapshotPtr snapshot = refreshed(service);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->find("outside.txt"), nullptr);
    EXPECT_NE(snapshot->find("game/inside.txt"), nullptr);
    EXPECT_NE(service.find(repo / "game" / "inside.txt"), nullptr);
    service.close();
    EXPECT_EQ(service.state(), VcsState::Off);
}

TEST(VcsService, NoRepositoryAndProviderNone) {
    VcsSandbox box;
    fs::create_directories(box.path("plain"));
    VcsService service;
    service.open(box.path("plain"), box.path("plain/.jasperengine"), box.settings(), false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.state() != VcsState::Detecting; }));
    EXPECT_EQ(service.state(), VcsState::NoRepository);
    // a repository, but the project says none
    const fs::path repo = box.gitRepo("repo");
    VcsSettings none = box.settings();
    none.provider = ProviderChoice::None;
    service.open(repo, repo / ".jasperengine", none, false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.state() != VcsState::Detecting; }));
    EXPECT_EQ(service.state(), VcsState::Off);
}

TEST(VcsService, NotAvailableWithoutTheTools) {
    // AC20: no git or svn on PATH: the editor still runs, version control says why it is off
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo");
    VcsSettings settings = box.settings();
    settings.usePathOverride = true;
    settings.pathOverride = box.path("empty-bin").string();
    VcsService service;
    service.open(repo, repo / ".jasperengine", settings, false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.state() != VcsState::Detecting; }));
    EXPECT_EQ(service.state(), VcsState::NotAvailable);
    EXPECT_EQ(service.stateMessage().rfind("Version control: not available", 0), 0u) << service.stateMessage();
    EXPECT_FALSE(service.active());
    // every operation answers, none spawns
    const VcsResult result = await(service, [&](VcsService::Done done) { service.lock({"README.md"}, std::move(done)); });
    EXPECT_FALSE(result.ok);
    // a configured path that does not exist says so
    settings.usePathOverride = false;
    settings.gitPath = box.path("no/such/git");
    service.open(repo, repo / ".jasperengine", settings, false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.state() != VcsState::Detecting; }));
    EXPECT_EQ(service.state(), VcsState::NotAvailable);
    EXPECT_NE(service.stateMessage().find("does not exist"), std::string::npos);
}

TEST(VcsService, MetaPairing) {
    // R8: an asset and its .meta are one item; a lone .meta is its own
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"hero.png", "png\n"}, {"hero.png.meta", "guid: a\n"}, {"lone.png", "png\n"},
                                               {"lone.png.meta", "guid: b\n"}});
    writeFile(repo / "hero.png", "png2\n");
    writeFile(repo / "hero.png.meta", "guid: a\nimport: 1\n");
    writeFile(repo / "lone.png.meta", "guid: b\nimport: 2\n");
    VcsService service;
    service.open(repo, repo / ".jasperengine", box.settings(), false);
    const SnapshotPtr snapshot = refreshed(service);
    ASSERT_TRUE(snapshot);
    const std::vector<RepoPath> paired = withChangedMetas(*snapshot, {"hero.png", "lone.png.meta"});
    EXPECT_EQ(paired, (std::vector<RepoPath>{"hero.png", "hero.png.meta", "lone.png.meta"}));
    EXPECT_EQ(metaAssetOf("hero.png.meta"), "hero.png");
    EXPECT_EQ(metaAssetOf("hero.png"), "");
    EXPECT_EQ(metaAssetOf(".meta"), "");
    // committing the asset takes its meta along
    std::string revision;
    const VcsResult committed = await(service, [&](VcsService::Done done) {
        service.commit(paired, "art", false, [done, &revision](const VcsResult& r, const std::string& rev) {
            revision = rev;
            done(r);
        });
    });
    ASSERT_TRUE(committed.ok) << committed.message;
    EXPECT_EQ(box.gitOk(repo, {"show", "--name-only", "--format=", "HEAD"}), "hero.png\nhero.png.meta\nlone.png.meta\n");
}

TEST(VcsService, WakesAfterOperationsAndRefreshes) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo");
    VcsSettings settings = box.settings();
    settings.pollSeconds = 120;   // only wakes poll in this test
    VcsService service;
    service.open(repo, repo / ".jasperengine", settings, false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.snapshot() != nullptr; }));
    const uint64_t polls = service.pollCount();
    writeFile(repo / "new.txt", "n\n");
    const SnapshotPtr snapshot = refreshed(service);
    ASSERT_TRUE(snapshot);
    EXPECT_NE(snapshot->find("new.txt"), nullptr);
    EXPECT_GT(service.pollCount(), polls);
    EXPECT_GT(service.snapshotVersion(), 0u);
}
