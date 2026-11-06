// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The code editor's baselines (docs/superpowers/specs/2026-11-04-code-editor-git-design.md 6.2):
// git HEAD and svn BASE content, the states of files with nothing to compare against, renames,
// LFS pointers and binaries, the memo that reads a file once per HEAD, and svn's HEAD meaning the
// baseline (BASE) in VcsService's reads.

#include "VcsTestSupport.h"

#include "vcs/TextBaselines.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor::test;

namespace {

    // Updates the cache until `file` leaves Pending (and None), pumping the service
    const TextBaseline& settle(VcsService& service, TextBaselines& baselines, const std::vector<fs::path>& files, const fs::path& file) {
        pumpUntil(service, [&]() {
            baselines.update(service, files);
            const BaselineState state = baselines.get(file).state;
            return state != BaselineState::Pending && state != BaselineState::None;
        });
        return baselines.get(file);
    }

    std::string fileUrl(const fs::path& path) {
        std::string generic = genericUtf8(path);
        if (!generic.empty() && generic[0] != '/') generic = "/" + generic;
        return "file://" + generic;
    }

}

TEST(TextBaselines, GitStates) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"a.lua", "local a = 1\nreturn a\n"},
                                               {"old.lua", "print('old')\n"},
                                               {".gitignore", "ignored.lua\n"}});
    // an LFS pointer and a binary, committed as such
    writeFile(repo / "pointer.lua", "version https://git-lfs.github.com/spec/v1\noid sha256:00\nsize 3\n");
    writeFile(repo / "binary.lua", std::string("a\0b", 3));
    box.gitOk(repo, {"add", "-A"});
    box.gitOk(repo, {"commit", "-q", "-m", "more"});

    VcsService service;
    service.open(repo, repo / ".jasperengine", box.settings(), false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.snapshot() != nullptr; })) << service.stateMessage();
    EXPECT_EQ(service.baselineRevision(), "HEAD");

    // edits, an untracked, an added, an ignored file and a staged rename
    writeFile(repo / "a.lua", "local a = 2\r\nreturn a\r\n");
    writeFile(repo / "untracked.lua", "u\n");
    writeFile(repo / "added.lua", "n\n");
    writeFile(repo / "ignored.lua", "i\n");
    box.gitOk(repo, {"add", "added.lua"});
    box.gitOk(repo, {"mv", "old.lua", "renamed.lua"});
    ASSERT_TRUE(refreshed(service));
    // the ignored set is read on its own cadence
    service.requestIgnoredRefresh();
    ASSERT_TRUE(refreshed(service));

    TextBaselines baselines;
    const std::vector<fs::path> files = {repo / "a.lua", repo / "untracked.lua", repo / "added.lua", repo / "ignored.lua",
                                         repo / "renamed.lua", repo / "pointer.lua", repo / "binary.lua", box.path("elsewhere.lua")};
    const TextBaseline& a = settle(service, baselines, files, files[0]);
    ASSERT_EQ(a.state, BaselineState::Ready) << a.error;
    EXPECT_EQ(a.revision, "HEAD");
    ASSERT_TRUE(a.lines);
    EXPECT_EQ(*a.lines, (std::vector<std::string>{"local a = 1", "return a", ""}));

    EXPECT_EQ(baselines.get(files[1]).state, BaselineState::Untracked);
    EXPECT_EQ(baselines.get(files[2]).state, BaselineState::NewFile);
    EXPECT_EQ(baselines.get(files[3]).state, BaselineState::Ignored);
    const TextBaseline& renamed = settle(service, baselines, files, files[4]);
    ASSERT_EQ(renamed.state, BaselineState::Ready) << renamed.error;
    EXPECT_EQ(*renamed.lines, (std::vector<std::string>{"print('old')", ""}));
    EXPECT_EQ(settle(service, baselines, files, files[5]).state, BaselineState::LfsPointer);
    EXPECT_EQ(settle(service, baselines, files, files[6]).state, BaselineState::Binary);
    EXPECT_EQ(baselines.get(files[7]).state, BaselineState::OutsideRepository);

    // a poll that changes nothing reads nothing again: the version stays
    const uint64_t version = baselines.get(files[0]).version;
    ASSERT_TRUE(refreshed(service));
    baselines.update(service, files);
    EXPECT_EQ(baselines.get(files[0]).version, version);

    // a commit moves HEAD: read again; the same text keeps the version
    box.gitOk(repo, {"commit", "-q", "-m", "rename", "--", "old.lua", "renamed.lua"});
    ASSERT_TRUE(refreshed(service));
    settle(service, baselines, files, files[0]);
    pumpUntil(service, [&]() { baselines.update(service, files); return false; }, std::chrono::milliseconds(500));
    EXPECT_EQ(baselines.get(files[0]).version, version);

    // committing a.lua changes its baseline
    box.gitOk(repo, {"commit", "-q", "-m", "a", "--", "a.lua"});
    ASSERT_TRUE(refreshed(service));
    pumpUntil(service, [&]() {
        baselines.update(service, files);
        const TextBaseline& now = baselines.get(files[0]);
        return now.lines && (*now.lines)[0] == "local a = 2";
    });
    EXPECT_EQ((*baselines.get(files[0]).lines)[0], "local a = 2");
    EXPECT_NE(baselines.get(files[0]).version, version);

    // a file closed is forgotten; version control off clears everything
    baselines.update(service, std::vector<fs::path>{files[0]});
    EXPECT_EQ(baselines.get(files[1]).state, BaselineState::None);
    service.close();
    baselines.update(service, files);
    EXPECT_EQ(baselines.get(files[0]).state, BaselineState::None);
}

#ifndef _WIN32
// The baseline reads, counted through a git that logs each blob read: one per file and HEAD
// however often the code editor asks, none once version control is off (spec AC7)
TEST(TextBaselines, ProviderCallsCounted) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"a.lua", "a\n"}, {"b.lua", "b\n"}});
    const fs::path reads = box.path("blob-reads");
    const fs::path wrapper = box.path("git-counting.sh");
    writeFile(wrapper, "#!/bin/sh\nfor arg in \"$@\"; do\n  if [ \"$arg\" = cat-file ]; then echo read >> '" + reads.string() +
                           "'; fi\ndone\nexec '" + box.git.string() + "' \"$@\"\n");
    fs::permissions(wrapper, fs::perms::owner_all, fs::perm_options::replace);
    auto blobReads = [&]() {
        const std::string text = fs::exists(reads) ? readFile(reads) : std::string();
        return std::count(text.begin(), text.end(), '\n');
    };
    VcsSettings settings = box.settings();
    settings.gitPath = wrapper;
    VcsService service;
    service.open(repo, repo / ".jasperengine", settings, false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.snapshot() != nullptr; })) << service.stateMessage();

    TextBaselines baselines;
    const std::vector<fs::path> files = {repo / "a.lua", repo / "b.lua"};
    ASSERT_EQ(settle(service, baselines, files, files[0]).state, BaselineState::Ready);
    ASSERT_EQ(settle(service, baselines, files, files[1]).state, BaselineState::Ready);
    EXPECT_EQ(blobReads(), 2);
    ASSERT_TRUE(refreshed(service));
    pumpUntil(service, [&]() { baselines.update(service, files); return false; }, std::chrono::milliseconds(300));
    EXPECT_EQ(blobReads(), 2);

    service.close();
    for (int i = 0; i < 10; i++) baselines.update(service, files);
    EXPECT_EQ(blobReads(), 2);
    EXPECT_EQ(baselines.get(files[0]).state, BaselineState::None);
}
#endif

TEST(TextBaselines, Classify) {
    EXPECT_EQ(TextBaselines::classify("text\n"), BaselineState::Ready);
    EXPECT_EQ(TextBaselines::classify(std::string("a\0b", 3)), BaselineState::Binary);
    EXPECT_EQ(TextBaselines::classify("version https://git-lfs.github.com/spec/v1\noid x\n"), BaselineState::LfsPointer);
    EXPECT_EQ(TextBaselines::classify(std::string(TextBaselines::kMaxBytes + 1, 'a')), BaselineState::TooLarge);
    EXPECT_STREQ(baselineStateName(BaselineState::NewFile), "new_file");
    EXPECT_STREQ(baselineStateLabel(BaselineState::Untracked), "Untracked");
}

// Subversion's baseline is BASE, the revision checked out: another user's newer commit is not a
// change of this working copy (S29 D5). VcsService reads BASE for "HEAD" (the scene diff, diff_file
// and the text diff viewer default to it).
TEST(TextBaselines, SvnBaselineIsBase) {
    VcsSandbox box;
    ASSERT_FALSE(box.svn.empty()) << "svn is in the build image";
    const fs::path repo = box.path("svnrepo");
    ASSERT_TRUE(box.run(box.svnadmin, box.dir.path(), {"create", repo.string()}).ok());
    const fs::path alice = box.path("alice");
    const fs::path bob = box.path("bob");
    box.svnOk(box.dir.path(), {"checkout", "-q", "--username", "alice", fileUrl(repo), alice.string()});
    writeFile(alice / "a.lua", "first\n");
    box.svnOk(alice, {"add", "-q", "a.lua"});
    box.svnOk(alice, {"commit", "-q", "--username", "alice", "-m", "one"});
    box.svnOk(box.dir.path(), {"checkout", "-q", "--username", "bob", fileUrl(repo), bob.string()});
    // alice commits again; bob does not update
    writeFile(alice / "a.lua", "second\n");
    box.svnOk(alice, {"commit", "-q", "--username", "alice", "-m", "two"});

    VcsSettings settings = box.settings();
    settings.provider = ProviderChoice::Svn;
    settings.svnGlobalArgs = {"--username", "bob"};
    VcsService service;
    service.open(bob, bob / ".jasperengine", settings, false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.snapshot() != nullptr; })) << service.stateMessage();
    EXPECT_EQ(service.baselineRevision(), "BASE");
    EXPECT_EQ(service.effectiveRevision("HEAD"), "BASE");
    EXPECT_EQ(service.effectiveRevision("2"), "2");

    // the code editor's baseline: bob's checked-out text
    TextBaselines baselines;
    const std::vector<fs::path> files = {bob / "a.lua"};
    const TextBaseline& baseline = settle(service, baselines, files, files[0]);
    ASSERT_EQ(baseline.state, BaselineState::Ready) << baseline.error;
    EXPECT_EQ(*baseline.lines, (std::vector<std::string>{"first", ""}));

    // the text diff against "HEAD" of an unmodified file is empty (svn's own HEAD would show alice's commit)
    std::string diff = "unset";
    bool finished = false;
    VcsResult result;
    service.textDiff("a.lua", "HEAD", [&](const VcsResult& r, std::string text) {
        result = r;
        diff = std::move(text);
        finished = true;
    });
    ASSERT_TRUE(pumpUntil(service, [&]() { return finished; }));
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(diff, "");

    // an unversioned file still diffs as all added
    writeFile(bob / "new.lua", "x\n");
    finished = false;
    service.textDiff("new.lua", "HEAD", [&](const VcsResult& r, std::string text) {
        result = r;
        diff = std::move(text);
        finished = true;
    });
    ASSERT_TRUE(pumpUntil(service, [&]() { return finished; }));
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NE(diff.find("+x"), std::string::npos) << diff;
}
