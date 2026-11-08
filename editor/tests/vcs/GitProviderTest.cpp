// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// GitProvider against temp repositories (docs/specs/S29-version-control.md R4, R9, AC5-AC7)

#include "VcsTestSupport.h"

#include "vcs/GitProvider.h"
#include "vcs/VcsEditorOps.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor::test;

namespace {

    // provider calls spawn processes: never on the main thread (R24)
    template<class F>
    auto offMain(F&& fn) {
        using R = decltype(fn());
        R result{};
        std::thread thread([&]() { result = fn(); });
        thread.join();
        return result;
    }

    const VcsChange* entry(const VcsSnapshot& snapshot, const std::string& path) {
        return snapshot.find(path);
    }

    VcsSnapshot statusOf(GitProvider& provider, uint32_t depth = StatusDepth::Fast) {
        return offMain([&]() {
            VcsSnapshot snapshot;
            const VcsResult result = provider.status(snapshot, depth);
            EXPECT_TRUE(result.ok) << result.message;
            snapshot.finalize(nullptr);
            return snapshot;
        });
    }

}

TEST(GitProvider, StatusPorcelainV2) {
    VcsSandbox box;
    ASSERT_FALSE(box.git.empty());
    const fs::path repo = box.gitRepo("repo", {{"a.txt", "a\n"}, {"d.txt", "d\n"}, {"r.txt", "r\n"}, {"with space.txt", "s\n"},
                                               {".gitignore", "*.log\n"}});
    writeFile(repo / "a.txt", "a changed\n");
    writeFile(repo / "new.txt", "new\n");
    box.gitOk(repo, {"add", "--", "new.txt"});
    writeFile(repo / "u.txt", "untracked\n");
    fs::remove(repo / "d.txt");
    box.gitOk(repo, {"mv", "r.txt", "r2.txt"});
    writeFile(repo / "ignored.log", "log\n");
    writeFile(repo / "with space.txt", "s changed\n");
    writeFile(repo / "-dash.txt", "dash\n");
#ifndef _WIN32
    writeFile(repo / "new\nline.txt", "newline\n");
#endif

    GitProvider provider(box.config(repo, repo));
    const VcsSnapshot snapshot = statusOf(provider, StatusDepth::WithIgnored);

    const VcsChange* a = entry(snapshot, "a.txt");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->index, FileState::Unmodified);
    EXPECT_EQ(a->worktree, FileState::Modified);
    EXPECT_FALSE(a->baseOid.empty());

    const VcsChange* added = entry(snapshot, "new.txt");
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->index, FileState::Added);
    EXPECT_EQ(added->worktree, FileState::Unmodified);
    EXPECT_TRUE(added->newFile());

    const VcsChange* untracked = entry(snapshot, "u.txt");
    ASSERT_NE(untracked, nullptr);
    EXPECT_EQ(untracked->index, FileState::Untracked);
    EXPECT_TRUE(untracked->newFile());

    const VcsChange* deleted = entry(snapshot, "d.txt");
    ASSERT_NE(deleted, nullptr);
    EXPECT_EQ(deleted->worktree, FileState::Deleted);
    EXPECT_TRUE(deleted->deleted());

    const VcsChange* renamed = entry(snapshot, "r2.txt");
    ASSERT_NE(renamed, nullptr);
    EXPECT_EQ(renamed->index, FileState::Renamed);
    EXPECT_EQ(renamed->oldPath, "r.txt");
    EXPECT_EQ(entry(snapshot, "r.txt"), nullptr);

    EXPECT_TRUE(snapshot.isIgnored("ignored.log"));
    EXPECT_EQ(entry(snapshot, "ignored.log"), nullptr);

    const VcsChange* spaced = entry(snapshot, "with space.txt");
    ASSERT_NE(spaced, nullptr);
    EXPECT_EQ(spaced->worktree, FileState::Modified);

    const VcsChange* dash = entry(snapshot, "-dash.txt");
    ASSERT_NE(dash, nullptr);
    EXPECT_EQ(dash->index, FileState::Untracked);
#ifndef _WIN32
    const VcsChange* newline = entry(snapshot, "new\nline.txt");
    ASSERT_NE(newline, nullptr);
    EXPECT_EQ(newline->index, FileState::Untracked);
#endif
    // nothing else, and no clean file
    std::set<std::string> paths;
    for (const VcsChange& change : snapshot.entries) paths.insert(change.path);
    std::set<std::string> expected = {"a.txt", "new.txt", "u.txt", "d.txt", "r2.txt", "with space.txt", "-dash.txt"};
#ifndef _WIN32
    expected.insert("new\nline.txt");
#endif
    EXPECT_EQ(paths, expected);
    EXPECT_EQ(snapshot.branch.name, "main");
    EXPECT_FALSE(snapshot.branch.headOid.empty());
    const VcsDirEntry* rootDir = snapshot.findDir("");
    ASSERT_NE(rootDir, nullptr);
    EXPECT_EQ(rootDir->state, DirState::Changed);
}

TEST(GitProvider, BranchAheadBehindAgainstABareUpstream) {
    VcsSandbox box;
    const fs::path remote = box.path("remote.git");
    fs::create_directories(remote);
    box.gitOk(remote, {"init", "-q", "--bare"});
    const fs::path mine = box.gitRepo("mine");
    box.gitOk(mine, {"remote", "add", "origin", remote.string()});
    box.gitOk(mine, {"push", "-q", "-u", "origin", "main"});
    const fs::path other = box.path("other");
    box.gitOk(box.dir.path(), {"clone", "-q", remote.string(), other.string()});
    for (int i = 0; i < 2; i++) {
        writeFile(other / ("theirs" + std::to_string(i) + ".txt"), "theirs\n");
        box.gitOk(other, {"add", "-A"});
        box.gitOk(other, {"commit", "-q", "-m", "theirs"});
    }
    box.gitOk(other, {"push", "-q"});
    writeFile(mine / "ours.txt", "ours\n");
    box.gitOk(mine, {"add", "-A"});
    box.gitOk(mine, {"commit", "-q", "-m", "ours"});

    GitProvider provider(box.config(mine, mine));
    const VcsResult fetched = offMain([&]() { return provider.fetch(); });
    ASSERT_TRUE(fetched.ok) << fetched.message;
    VcsBranchInfo branch;
    ASSERT_TRUE(offMain([&]() { return provider.branchInfo(branch); }).ok);
    EXPECT_EQ(branch.name, "main");
    EXPECT_EQ(branch.upstream, "origin/main");
    EXPECT_EQ(branch.ahead, 1);
    EXPECT_EQ(branch.behind, 2);
    // the server's changes, read with the locks (no LFS here)
    const VcsSnapshot snapshot = statusOf(provider, StatusDepth::WithLocksAndRemote);
    EXPECT_TRUE(snapshot.hasRemote);
    const VcsChange* theirs = snapshot.find("theirs0.txt");
    ASSERT_NE(theirs, nullptr);
    EXPECT_TRUE(theirs->remoteChanged);
    EXPECT_FALSE(theirs->changed());
    // a pull fast-forwards only: with a commit of ours it is refused
    const VcsResult pulled = offMain([&]() { return provider.pull(); });
    EXPECT_FALSE(pulled.ok);
}

TEST(GitProvider, CommitOnlySelected) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"A.txt", "a\n"}, {"B.txt", "b\n"}});
    writeFile(repo / "A.txt", "a2\n");
    writeFile(repo / "B.txt", "b2\n");
    box.gitOk(repo, {"add", "--", "B.txt"});
    writeFile(repo / "C.txt", "c\n");
    const std::string message = "-m \"x\"; $(touch pwn)";

    GitProvider provider(box.config(repo, repo));
    const std::vector<RepoPath> paths = {"A.txt", "C.txt"};
    std::string revision;
    const VcsResult result = offMain([&]() { return provider.commit(paths, message, revision); });
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(revision, box.gitOk(repo, {"rev-parse", "HEAD"}).substr(0, revision.size()));
    EXPECT_EQ(revision.size(), 40u);
    // exactly A and C, with the message as it was given
    EXPECT_EQ(box.gitOk(repo, {"show", "--name-only", "--format=", "HEAD"}), "A.txt\nC.txt\n");
    EXPECT_EQ(box.gitOk(repo, {"log", "-1", "--format=%s"}), message + "\n");
    EXPECT_EQ(box.gitOk(repo, {"cat-file", "commit", "HEAD"}).substr(box.gitOk(repo, {"cat-file", "commit", "HEAD"}).find("\n\n") + 2),
              message + "\n");
    // B stays staged, and nothing ran the message
    EXPECT_EQ(box.gitOk(repo, {"diff", "--cached", "--name-only"}), "B.txt\n");
    EXPECT_FALSE(fs::exists(repo / "pwn"));
    EXPECT_FALSE(fs::exists(fs::current_path() / "pwn"));
    // the log lists it first
    std::vector<VcsLogEntry> log;
    ASSERT_TRUE(offMain([&]() { return provider.log(nullptr, 10, 0, log); }).ok);
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].revision, revision);
    EXPECT_EQ(log[0].message, message);
    EXPECT_EQ(log[0].author, "Tester");
    EXPECT_EQ(log[1].message, "initial");
}

TEST(GitProvider, FsmonitorIgnored) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"a.txt", "a\n"}});
    const fs::path marker = box.path("fsmonitor-ran");
#ifdef _WIN32
    const fs::path hook = box.path("fsmonitor.bat");
    writeFile(hook, "@echo off\r\necho ran > \"" + marker.string() + "\"\r\n");
#else
    const fs::path hook = box.path("fsmonitor.sh");
    writeFile(hook, "#!/bin/sh\necho ran > '" + marker.string() + "'\n");
    fs::permissions(hook, fs::perms::owner_all, fs::perm_options::replace);
#endif
    box.gitOk(repo, {"config", "core.fsmonitor", genericUtf8(hook)});
    writeFile(repo / "a.txt", "a changed\n");

    GitProvider provider(box.config(repo, repo));
    statusOf(provider);
    std::vector<VcsLogEntry> log;
    EXPECT_TRUE(offMain([&]() { return provider.log(nullptr, 5, 0, log); }).ok);
    std::string diff;
    EXPECT_TRUE(offMain([&]() { return provider.textDiff("a.txt", "HEAD", diff); }).ok);
    EXPECT_NE(diff.find("+a changed"), std::string::npos);
    EXPECT_FALSE(fs::exists(marker));

    // the control: git's own status runs that hook (the test would see it)
    box.run(box.git, repo, {"status"});
    EXPECT_TRUE(fs::exists(marker));
}

TEST(GitProvider, ProjectInASubdirectory) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"outside.txt", "o\n"}, {"game/inside.txt", "i\n"}});
    writeFile(repo / "outside.txt", "o2\n");
    writeFile(repo / "game/inside.txt", "i2\n");
    writeFile(repo / "game/new.txt", "n\n");
    GitProvider provider(box.config(repo, repo / "game"));
    const VcsSnapshot snapshot = statusOf(provider);
    EXPECT_EQ(snapshot.find("outside.txt"), nullptr);
    ASSERT_NE(snapshot.find("game/inside.txt"), nullptr);
    ASSERT_NE(snapshot.find("game/new.txt"), nullptr);
    const VcsDirEntry* game = snapshot.findDir("game");
    ASSERT_NE(game, nullptr);
    EXPECT_EQ(game->state, DirState::Changed);   // a modified file outranks an untracked one
    EXPECT_EQ(game->changed, 2u);
    // the project's history only
    std::vector<VcsLogEntry> log;
    ASSERT_TRUE(offMain([&]() { return provider.log(nullptr, 10, 0, log); }).ok);
    EXPECT_EQ(log.size(), 1u);
}

TEST(GitProvider, StageUnstageRevertAndBaseContent) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"a.txt", "base\n"}});
    writeFile(repo / "a.txt", "edited\n");
    GitProvider provider(box.config(repo, repo));
    const std::vector<RepoPath> a = {"a.txt"};
    ASSERT_TRUE(offMain([&]() { return provider.stage(a); }).ok);
    EXPECT_EQ(statusOf(provider).find("a.txt")->index, FileState::Modified);
    ASSERT_TRUE(offMain([&]() { return provider.unstage(a); }).ok);
    EXPECT_EQ(statusOf(provider).find("a.txt")->index, FileState::Unmodified);
    std::string base;
    ASSERT_TRUE(offMain([&]() { return provider.baseContent("a.txt", "HEAD", base); }).ok);
    EXPECT_EQ(base, "base\n");
    // over the limit: only enough of it to see so (TextBaselines' 4 MiB check)
    ASSERT_TRUE(offMain([&]() { return provider.baseContent("a.txt", "HEAD", base, 2); }).ok);
    EXPECT_EQ(base, "bas");
    ASSERT_TRUE(offMain([&]() { return provider.stage(a); }).ok);
    ASSERT_TRUE(offMain([&]() { return provider.revert(a); }).ok);
    EXPECT_EQ(readFile(repo / "a.txt"), "base\n");
    EXPECT_EQ(statusOf(provider).find("a.txt"), nullptr);
    // a revision is one argument, never an option
    EXPECT_FALSE(offMain([&]() { return provider.baseContent("a.txt", "--output=/tmp/x", base); }).ok);
}

TEST(GitProvider, ManyPathsGoThroughAPathspecFile) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo");
    std::vector<RepoPath> paths;
    for (int i = 0; i < 100; i++) {
        const std::string name = "many/file " + std::to_string(i) + ".txt";
        writeFile(repo / name, std::to_string(i));
        paths.push_back(name);
    }
    GitProvider provider(box.config(repo, repo));
    std::string revision;
    const VcsResult result = offMain([&]() { return provider.commit(paths, "100 files", revision); });
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(statusOf(provider).entries.empty());
    // the pathspec file is gone again
    EXPECT_TRUE(!fs::exists(repo / ".jasperengine" / "vcs") || fs::is_empty(repo / ".jasperengine" / "vcs"));
}

TEST(GitProvider, ValidRevisions) {
    VcsSandbox box;
    GitProvider provider(box.config(box.path("none"), box.path("none")));
    EXPECT_TRUE(provider.validRevision("HEAD"));
    EXPECT_TRUE(provider.validRevision("HEAD~3"));
    EXPECT_TRUE(provider.validRevision("abc1234"));
    EXPECT_TRUE(provider.validRevision(std::string(40, 'f')));
    EXPECT_FALSE(provider.validRevision("--output=/tmp/x"));
    EXPECT_FALSE(provider.validRevision("-n"));
    EXPECT_FALSE(provider.validRevision("abc12"));
    EXPECT_FALSE(provider.validRevision("HEAD~12345"));
    EXPECT_FALSE(provider.validRevision("HEAD^"));
    EXPECT_FALSE(provider.validRevision("main"));
}

TEST(GitProvider, MergeDriverSetup) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo");
    GitProvider provider(box.config(repo, repo));
    EXPECT_FALSE(offMain([&]() { return provider.mergeDriverConfigured(); }));
    const fs::path editor = box.path("Jasper Engine/it's here/jasperengine-editor");
    ASSERT_TRUE(offMain([&]() { return provider.setupMergeDriver(editor); }).ok);
    EXPECT_TRUE(offMain([&]() { return provider.mergeDriverConfigured(); }));
    const std::string driver = box.gitOk(repo, {"config", "--get", "merge.jasperengine-scene.driver"});
    EXPECT_EQ(driver, GitProvider::mergeDriverCommand(editor) + "\n");
    EXPECT_NE(driver.find("'\\''s here"), std::string::npos);
    EXPECT_NE(driver.find(" merge-scene %O %A %B %P"), std::string::npos);
}

// The status bar's HEAD (docs/superpowers/specs/2026-11-04-code-editor-git-design.md 6.5): a
// detached HEAD's tag, a merge and a rebase left unfinished, read from the git directory
TEST(GitProvider, DetachedAndOperations) {
    VcsSandbox box;
    ASSERT_FALSE(box.git.empty());
    const fs::path repo = box.gitRepo("repo", {{"a.txt", "base\n"}});
    GitProvider provider(box.config(repo, repo));
    VcsSnapshot snapshot = statusOf(provider);
    EXPECT_FALSE(snapshot.branch.detached);
    EXPECT_EQ(snapshot.branch.operation, VcsOperation::None);
    EXPECT_EQ(headLabel(snapshot.branch), "main");

    // detached at a tag, and at a commit no tag names
    box.gitOk(repo, {"tag", "v1.0"});
    writeFile(repo / "a.txt", "second\n");
    box.gitOk(repo, {"commit", "-q", "-am", "second"});
    box.gitOk(repo, {"checkout", "-q", "v1.0"});
    snapshot = statusOf(provider);
    EXPECT_TRUE(snapshot.branch.detached);
    std::string tag;
    ASSERT_TRUE(offMain([&]() { return provider.headTag(tag); }).ok);
    EXPECT_EQ(tag, "v1.0");
    snapshot.branch.tag = tag;
    EXPECT_EQ(headLabel(snapshot.branch), "tag v1.0");
    box.gitOk(repo, {"checkout", "-q", "main"});
    box.gitOk(repo, {"checkout", "-q", "--detach", "HEAD"});
    snapshot = statusOf(provider);
    EXPECT_TRUE(snapshot.branch.detached);
    EXPECT_FALSE(offMain([&]() { return provider.headTag(tag); }).ok);
    EXPECT_EQ(headLabel(snapshot.branch), snapshot.branch.headOid.substr(0, 7));
    box.gitOk(repo, {"checkout", "-q", "main"});

    // a merge that stopped on a conflict
    box.gitOk(repo, {"checkout", "-q", "-b", "other"});
    writeFile(repo / "a.txt", "other\n");
    box.gitOk(repo, {"commit", "-q", "-am", "other"});
    box.gitOk(repo, {"checkout", "-q", "main"});
    writeFile(repo / "a.txt", "main\n");
    box.gitOk(repo, {"commit", "-q", "-am", "main"});
    EXPECT_FALSE(box.run(box.git, repo, {"merge", "-q", "other"}).ok());
    snapshot = statusOf(provider);
    EXPECT_EQ(snapshot.branch.operation, VcsOperation::Merge);
    EXPECT_EQ(headLabel(snapshot.branch), "main | MERGING");
    ASSERT_NE(snapshot.find("a.txt"), nullptr);
    EXPECT_TRUE(snapshot.find("a.txt")->conflict);
    box.gitOk(repo, {"merge", "--abort"});
    EXPECT_EQ(statusOf(provider).branch.operation, VcsOperation::None);

    // a rebase that stopped on a conflict: its branch and step
    box.gitOk(repo, {"checkout", "-q", "other"});
    EXPECT_FALSE(box.run(box.git, repo, {"rebase", "main"}).ok());
    snapshot = statusOf(provider);
    EXPECT_EQ(snapshot.branch.operation, VcsOperation::Rebase);
    EXPECT_EQ(snapshot.branch.operationBranch, "other");
    EXPECT_EQ(snapshot.branch.operationStep, 1);
    EXPECT_EQ(snapshot.branch.operationTotal, 1);
    EXPECT_EQ(headLabel(snapshot.branch), "other | REBASE 1/1");
    box.gitOk(repo, {"rebase", "--abort"});
    EXPECT_EQ(statusOf(provider).branch.operation, VcsOperation::None);
}

#ifndef _WIN32
// A git dir read that failed once (a timeout, a lock) is asked again: a cached failure would hide
// every merge or rebase state for the editor's whole session
TEST(GitProvider, GitDirFailureIsNotCached) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"a.txt", "base\n"}});
    const fs::path flag = box.path("rev-parse-failed");
    const fs::path wrapper = box.path("git-fails-rev-parse-once.sh");
    writeFile(wrapper, "#!/bin/sh\nfor arg in \"$@\"; do\n  if [ \"$arg\" = rev-parse ] && [ ! -e '" + flag.string() +
                           "' ]; then\n    : > '" + flag.string() + "'\n    exit 1\n  fi\ndone\nexec '" + box.git.string() +
                           "' \"$@\"\n");
    fs::permissions(wrapper, fs::perms::owner_all, fs::perm_options::replace);
    ProviderConfig cfg = box.config(repo, repo);
    cfg.executable = wrapper;
    GitProvider provider(cfg);
    VcsSnapshot snapshot = statusOf(provider);
    writeFile(repo / ".git" / "MERGE_HEAD", snapshot.branch.headOid + "\n");
    EXPECT_EQ(snapshot.branch.operation, VcsOperation::None);
    ASSERT_TRUE(fs::exists(flag)) << "the first read failed";
    EXPECT_EQ(statusOf(provider).branch.operation, VcsOperation::Merge);
}
#endif

TEST(GitProvider, HeadLabels) {
    VcsBranchInfo branch;
    branch.name = "main";
    EXPECT_EQ(headLabel(branch), "main");
    branch.operation = VcsOperation::CherryPick;
    EXPECT_EQ(headLabel(branch), "main | CHERRY-PICKING");
    branch.operation = VcsOperation::Revert;
    EXPECT_EQ(headLabel(branch), "main | REVERTING");
    branch.operation = VcsOperation::Bisect;
    EXPECT_EQ(headLabel(branch), "main | BISECTING");
    branch = VcsBranchInfo();
    branch.detached = true;
    branch.headOid = "0123456789abcdef";
    EXPECT_EQ(headLabel(branch), "0123456");
    branch.operation = VcsOperation::Rebase;
    branch.operationBranch = "feature";
    EXPECT_EQ(headLabel(branch), "feature | REBASE");
    EXPECT_STREQ(vcsOperationName(VcsOperation::CherryPick), "cherry_pick");
}

// A copy (status.renames=copies) is a new file whose diff base is its source: Revert refuses it
// (Delete it instead) and so never touches the source; a rename is no new file
TEST(GitProvider, CopiesAreNewFilesDiffedAgainstTheirSource) {
    VcsSandbox box;
    ASSERT_FALSE(box.git.empty());
    const std::string text = "one\ntwo\nthree\nfour\nfive\n";
    const fs::path repo = box.gitRepo("repo", {{"source.txt", text}, {"r.txt", "r\n"}});
    box.gitOk(repo, {"config", "status.renames", "copies"});
    writeFile(repo / "source.txt", text + "six\n");
    writeFile(repo / "copy.txt", text);
    box.gitOk(repo, {"add", "--", "source.txt", "copy.txt"});
    box.gitOk(repo, {"mv", "r.txt", "r2.txt"});

    GitProvider provider(box.config(repo, repo));
    const VcsSnapshot snapshot = statusOf(provider);
    const VcsChange* copy = entry(snapshot, "copy.txt");
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->oldPath, "source.txt");
    EXPECT_TRUE(copy->copied);
    EXPECT_TRUE(copy->newFile());
    EXPECT_FALSE(copy->renamed());
    EXPECT_TRUE(copy->hasBaseline());
    EXPECT_EQ(copy->diffSource(), "source.txt");
    const std::string refusal = revertRefusal(snapshot, {"copy.txt"}, false);
    EXPECT_NE(refusal.find("is new"), std::string::npos) << refusal;

    const VcsChange* renamed = entry(snapshot, "r2.txt");
    ASSERT_NE(renamed, nullptr);
    EXPECT_TRUE(renamed->renamed());
    EXPECT_FALSE(renamed->newFile());
    EXPECT_EQ(revertRefusal(snapshot, {"r2.txt"}, false), "");
}
