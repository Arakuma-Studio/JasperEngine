// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SvnProvider against a file:// repository and two working copies (docs/specs/
// S29-version-control.md R10, AC9)

#include "VcsTestSupport.h"

#include "vcs/SvnProvider.h"
#include "vcs/SvnXml.h"

using namespace jasperengine;
using namespace jasperengine::editor::test;

namespace {

    template<class F>
    auto offMain(F&& fn) {
        using R = decltype(fn());
        R result{};
        std::thread thread([&]() { result = fn(); });
        thread.join();
        return result;
    }

    std::string fileUrl(const fs::path& path) {
        std::string generic = genericUtf8(path);
        if (!generic.empty() && generic[0] != '/') generic = "/" + generic;   // C:/... on Windows
        return "file://" + generic;
    }

    VcsSnapshot statusOf(SvnProvider& provider, uint32_t depth = StatusDepth::Fast) {
        return offMain([&]() {
            VcsSnapshot snapshot;
            const VcsResult result = provider.status(snapshot, depth);
            EXPECT_TRUE(result.ok) << result.message;
            snapshot.finalize(nullptr);
            return snapshot;
        });
    }

    ProviderConfig svnConfig(const VcsSandbox& box, const fs::path& wc, const char* user) {
        ProviderConfig cfg = box.config(wc, wc);
        cfg.executable = box.svn;
        cfg.userName = user;
        cfg.svnGlobalArgs = {"--username", user};
        return cfg;
    }

}

TEST(SvnXml, ParsesSvnOutput) {
    const std::string text =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- a comment -->\n<status>\n<target path=\".\">\n"
        "<entry path=\"a &amp; b.txt\"><wc-status item=\"modified\" revision=\"3\" props=\"none\">"
        "<lock><token>opaquelocktoken:1</token><owner>al&#xE9;</owner></lock></wc-status></entry>\n"
        "<against revision=\"5\"/>\n</target>\n</status>\n";
    XmlElement root;
    std::string error;
    ASSERT_TRUE(parseXml(text, root, &error)) << error;
    EXPECT_EQ(root.name, "status");
    const XmlElement* target = root.child("target");
    ASSERT_NE(target, nullptr);
    const XmlElement* entry = target->child("entry");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->attribute("path"), "a & b.txt");
    const XmlElement* wc = entry->child("wc-status");
    ASSERT_NE(wc, nullptr);
    EXPECT_EQ(wc->attribute("item"), "modified");
    ASSERT_NE(wc->child("lock"), nullptr);
    EXPECT_EQ(wc->child("lock")->childText("owner"), "al\xC3\xA9");
    EXPECT_EQ(target->child("against")->attribute("revision"), "5");
    EXPECT_FALSE(parseXml("<a><b></a>", root, &error));
    EXPECT_FALSE(parseXml("<a attr=\"x>", root, &error));
}

TEST(SvnProvider, EndToEnd) {
    VcsSandbox box;
    ASSERT_FALSE(box.svn.empty()) << "svn is in the build image";
    ASSERT_FALSE(box.svnadmin.empty());
    const fs::path repo = box.path("repo");
    ASSERT_TRUE(box.run(box.svnadmin, box.dir.path(), {"create", repo.string()}).ok());
    const fs::path wcA = box.path("alice");
    const fs::path wcB = box.path("bob");
    box.svnOk(box.dir.path(), {"checkout", "-q", "--username", "alice", fileUrl(repo), wcA.string()});
    box.svnOk(box.dir.path(), {"checkout", "-q", "--username", "bob", fileUrl(repo), wcB.string()});

    SvnProvider alice(svnConfig(box, wcA, "alice"));
    SvnProvider bob(svnConfig(box, wcB, "bob"));
    EXPECT_TRUE(alice.validRevision("BASE"));
    EXPECT_TRUE(alice.validRevision("HEAD"));
    EXPECT_TRUE(alice.validRevision("42"));
    EXPECT_FALSE(alice.validRevision("--revision=3"));
    EXPECT_FALSE(alice.validRevision("HEAD~1"));

    // add: unversioned files show as such, and a commit of them adds them first
    writeFile(wcA / "a.txt", "line1\n");
    writeFile(wcA / "icon@2x.lua", "x\n");
    writeFile(wcA / "art/needs.bin", "binary\n");
    VcsSnapshot snapshot = statusOf(alice);
    ASSERT_NE(snapshot.find("a.txt"), nullptr);
    EXPECT_EQ(snapshot.find("a.txt")->worktree, FileState::Untracked);
    std::string revision;
    VcsResult result = offMain([&]() { return alice.commit(std::vector<RepoPath>{"a.txt", "icon@2x.lua", "art/needs.bin"}, "add files", revision); });
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(revision, "1");
    box.svnOk(wcA, {"propset", "svn:needs-lock", "*", "art/needs.bin"});
    result = offMain([&]() { return alice.commit(std::vector<RepoPath>{"art/needs.bin"}, "needs-lock", revision); });
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(revision, "2");
    // nothing changed (the needs-lock file is listed for its lock state only)
    EXPECT_EQ(statusOf(alice).changedCount(), 0u);

    // modify
    writeFile(wcA / "a.txt", "line1 alice\n");
    snapshot = statusOf(alice);
    ASSERT_NE(snapshot.find("a.txt"), nullptr);
    EXPECT_EQ(snapshot.find("a.txt")->worktree, FileState::Modified);
    std::string base;
    ASSERT_TRUE(offMain([&]() { return alice.baseContent("a.txt", "BASE", base); }).ok);
    EXPECT_EQ(base, "line1\n");
    ASSERT_TRUE(offMain([&]() { return alice.baseContent("a.txt", "BASE", base, 2); }).ok);
    EXPECT_EQ(base, "lin");
    std::string diff;
    ASSERT_TRUE(offMain([&]() { return alice.textDiff("a.txt", "BASE", diff); }).ok);
    EXPECT_NE(diff.find("+line1 alice"), std::string::npos);

    // a name with '@': svn reads a peg revision in it unless the path is escaped, and diff
    // against BASE takes no escape
    writeFile(wcA / "icon@2x.lua", "y\n");
    ASSERT_TRUE(offMain([&]() { return alice.baseContent("icon@2x.lua", "BASE", base); }).ok);
    EXPECT_EQ(base, "x\n");
    for (const char* rev : {"BASE", "1"}) {
        diff.clear();
        const VcsResult atDiff = offMain([&]() { return alice.textDiff("icon@2x.lua", rev, diff); });
        ASSERT_TRUE(atDiff.ok) << rev << ": " << atDiff.message;
        EXPECT_NE(diff.find("-x\n+y\n"), std::string::npos) << rev << ": " << diff;
    }
    writeFile(wcA / "icon@2x.lua", "x\n");

    // conflict: bob changes the same line and updates
    ASSERT_TRUE(offMain([&]() { return bob.pull(); }).ok);
    EXPECT_EQ(readFile(wcB / "a.txt"), "line1\n");
    result = offMain([&]() { return alice.commit(std::vector<RepoPath>{"a.txt"}, "alice's line", revision); });
    ASSERT_TRUE(result.ok) << result.message;
    writeFile(wcB / "a.txt", "line1 bob\n");
    ASSERT_TRUE(offMain([&]() { return bob.pull(); }).ok);
    snapshot = statusOf(bob);
    ASSERT_NE(snapshot.find("a.txt"), nullptr);
    EXPECT_TRUE(snapshot.find("a.txt")->conflict);
    ConflictSides sides;
    ASSERT_TRUE(offMain([&]() { return bob.conflictSides("a.txt", sides); }).ok);
    EXPECT_TRUE(sides.hasBase);
    EXPECT_EQ(sides.base, "line1\n");
    EXPECT_EQ(sides.ours, "line1 bob\n");
    EXPECT_EQ(sides.theirs, "line1 alice\n");
    ASSERT_TRUE(offMain([&]() { return bob.resolveWith("a.txt", false); }).ok);
    EXPECT_EQ(readFile(wcB / "a.txt"), "line1 alice\n");
    EXPECT_EQ(statusOf(bob).find("a.txt"), nullptr);

    // needs-lock: alice locks, bob sees her lock, can't release it without force
    std::vector<bool> lockable;
    ASSERT_TRUE(offMain([&]() { return bob.lockable(std::vector<RepoPath>{"art/needs.bin", "a.txt"}, lockable); }).ok);
    ASSERT_EQ(lockable.size(), 2u);
    EXPECT_TRUE(lockable[0]);
    EXPECT_FALSE(lockable[1]);
    ASSERT_TRUE(offMain([&]() { return alice.lock(std::vector<RepoPath>{"art/needs.bin"}); }).ok);
    snapshot = statusOf(alice, StatusDepth::WithLocksAndRemote);
    ASSERT_NE(snapshot.find("art/needs.bin"), nullptr);
    EXPECT_EQ(snapshot.find("art/needs.bin")->lock, LockState::LockedByMe);
    snapshot = statusOf(bob, StatusDepth::WithLocksAndRemote);
    ASSERT_NE(snapshot.find("art/needs.bin"), nullptr);
    EXPECT_EQ(snapshot.find("art/needs.bin")->lock, LockState::LockedByOther);
    EXPECT_EQ(snapshot.find("art/needs.bin")->lockOwner, "alice");
    EXPECT_TRUE(snapshot.isLockable("art/needs.bin"));
    EXPECT_FALSE(offMain([&]() { return bob.unlock(std::vector<RepoPath>{"art/needs.bin"}, false); }).ok);
    EXPECT_TRUE(offMain([&]() { return bob.unlock(std::vector<RepoPath>{"art/needs.bin"}, true); }).ok);
    snapshot = statusOf(alice, StatusDepth::WithLocksAndRemote);
    ASSERT_NE(snapshot.find("art/needs.bin"), nullptr);
    EXPECT_EQ(snapshot.find("art/needs.bin")->lock, LockState::Unlocked);

    // log: newest first
    std::vector<VcsLogEntry> log;
    ASSERT_TRUE(offMain([&]() { return alice.log(nullptr, 10, 0, log); }).ok);
    ASSERT_GE(log.size(), 3u);
    EXPECT_EQ(log[0].message, "alice's line");
    EXPECT_EQ(log[0].author, "alice");
    EXPECT_EQ(log.back().message, "add files");
    std::vector<VcsLogEntry> skipped;
    ASSERT_TRUE(offMain([&]() { return alice.log(nullptr, 1, 1, skipped); }).ok);
    ASSERT_EQ(skipped.size(), 1u);
    EXPECT_EQ(skipped[0].revision, log[1].revision);

    // revert puts BASE back
    writeFile(wcA / "a.txt", "scratch\n");
    ASSERT_TRUE(offMain([&]() { return alice.revert(std::vector<RepoPath>{"a.txt"}); }).ok);
    EXPECT_EQ(readFile(wcA / "a.txt"), "line1 alice\n");

    // branch: the working copy's revision and its URL below the root
    VcsBranchInfo branch;
    ASSERT_TRUE(offMain([&]() { return alice.branchInfo(branch); }).ok);
    EXPECT_EQ(branch.name, "^/");
    EXPECT_FALSE(branch.headOid.empty());
}

// A scheduled add has no BASE to compare with (the scene diff lists all of it as added, as for a
// git add); a move's new path compares with its source, as a git rename does; a copy is new, as
// in git (which tracks no copies)
TEST(SvnProvider, AddedMovedAndCopiedFiles) {
    VcsSandbox box;
    ASSERT_FALSE(box.svn.empty()) << "svn is in the build image";
    const fs::path repo = box.path("repo");
    ASSERT_TRUE(box.run(box.svnadmin, box.dir.path(), {"create", repo.string()}).ok());
    const fs::path wc = box.path("wc");
    box.svnOk(box.dir.path(), {"checkout", "-q", "--username", "alice", fileUrl(repo), wc.string()});
    writeFile(wc / "Level.scene", "base\n");
    box.svnOk(wc, {"add", "-q", "Level.scene"});
    box.svnOk(wc, {"commit", "-q", "--username", "alice", "-m", "one"});
    SvnProvider alice(svnConfig(box, wc, "alice"));

    writeFile(wc / "New.scene", "new\n");
    box.svnOk(wc, {"add", "-q", "New.scene"});
    box.svnOk(wc, {"move", "-q", "Level.scene", "Moved.scene"});
    box.svnOk(wc, {"copy", "-q", "Moved.scene", "Copy.scene"});
    const VcsSnapshot snapshot = statusOf(alice);

    const VcsChange* added = snapshot.find("New.scene");
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->worktree, FileState::Added);
    EXPECT_TRUE(added->newFile());

    const VcsChange* moved = snapshot.find("Moved.scene");
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->oldPath, "Level.scene");
    EXPECT_FALSE(moved->newFile());
    std::string base;
    ASSERT_TRUE(offMain([&]() { return alice.baseContent(moved->oldPath, "BASE", base); }).ok);
    EXPECT_EQ(base, "base\n");
    ASSERT_NE(snapshot.find("Level.scene"), nullptr);
    EXPECT_TRUE(snapshot.find("Level.scene")->deleted());

    const VcsChange* copy = snapshot.find("Copy.scene");
    ASSERT_NE(copy, nullptr);
    EXPECT_TRUE(copy->oldPath.empty());
    EXPECT_TRUE(copy->newFile());
}
