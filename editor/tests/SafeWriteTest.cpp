// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// util/SafeWrite: writes, removals and renames that stay beneath a root when they happen (no
// symbolic link on the way followed: refused, naming the folder; a link at the final name replaced,
// never followed), the resolve-first variants (links inside the root fine, out of it refused), and
// WriteFilesCmd::writeBeneath built on them. POSIX here (the Windows half is compiled there).

#include <gtest/gtest.h>

#include "command/type/WriteFilesCmd.h"
#include "util/SafeWrite.h"
#include "SymlinkSupport.h"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace sw = jasperengine::editor::safewrite;

namespace {

    struct Dirs {
        fs::path base, root, outside;
        explicit Dirs(const std::string& name) {
            base = fs::temp_directory_path() / ("jasper-safewrite-" + name + "-" + std::to_string(::getpid()));
            fs::remove_all(base);
            root = base / "root";
            outside = base / "outside";
            fs::create_directories(root);
            fs::create_directories(outside);
        }
        ~Dirs() {
            std::error_code ec;
            fs::remove_all(base, ec);
        }
    };

    std::string readAll(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void writeText(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    size_t entries(const fs::path& dir) {
        return static_cast<size_t>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
    }

}

TEST(SafeWrite, WritesNewAndReplacesKeepingPermissions) {
    Dirs d("plain");
    std::string error;
    std::vector<fs::path> created;
    ASSERT_TRUE(sw::writeFileBeneath(d.root, d.root / "a" / "b" / "new.txt", "one", error, &created)) << error;
    EXPECT_EQ(readAll(d.root / "a" / "b" / "new.txt"), "one");
    ASSERT_EQ(created.size(), 2u) << "the folders it made, outermost first";
    EXPECT_EQ(created[0], fs::canonical(d.root) / "a");
    EXPECT_EQ(created[1], fs::canonical(d.root) / "a" / "b");
    // relative to the root works too; an existing file keeps its permissions
    fs::permissions(d.root / "a" / "b" / "new.txt", fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read,
                    fs::perm_options::replace);
    ASSERT_TRUE(sw::writeFileBeneath(d.root, fs::path("a") / "b" / "new.txt", "two", error)) << error;
    EXPECT_EQ(readAll(d.root / "a" / "b" / "new.txt"), "two");
    EXPECT_EQ(fs::status(d.root / "a" / "b" / "new.txt").permissions() & fs::perms::all,
              fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    EXPECT_EQ(entries(d.root / "a" / "b"), 1u) << "no temporary file left";
    // a folder at the name, a file on the way, a path outside the root: refused, nothing made
    fs::create_directories(d.root / "dir.txt");
    EXPECT_FALSE(sw::writeFileBeneath(d.root, d.root / "dir.txt", "x", error));
    EXPECT_NE(error.find("is a folder"), std::string::npos) << error;
    created.clear();
    EXPECT_FALSE(sw::writeFileBeneath(d.root, d.root / "a" / "b" / "new.txt" / "deeper" / "x.txt", "x", error, &created));
    EXPECT_NE(error.find("not a folder"), std::string::npos) << error;
    EXPECT_TRUE(created.empty());
    EXPECT_FALSE(sw::writeFileBeneath(d.root, d.outside / "x.txt", "x", error));
    EXPECT_NE(error.find("is not inside"), std::string::npos) << error;
    EXPECT_FALSE(sw::writeFileBeneath(d.root, fs::path("..") / "outside" / "x.txt", "x", error));
    EXPECT_FALSE(fs::exists(d.outside / "x.txt"));
    // a failed write in new folders leaves none of them
    created.clear();
    EXPECT_FALSE(sw::writeFileBeneath(d.root, d.root / "m" / "n" / std::string(300, 'x'), "x", error, &created)) << "a name too long";
    EXPECT_TRUE(created.empty());
    EXPECT_FALSE(fs::exists(d.root / "m"));
}

TEST(SafeWrite, ReadOnlyFilesAreRefused) {
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores file permissions";
    Dirs d("readonly");
    writeText(d.root / "ro.txt", "keep");
    fs::permissions(d.root / "ro.txt", fs::perms::owner_read, fs::perm_options::replace);
    std::string error;
    EXPECT_FALSE(sw::writeFileBeneath(d.root, d.root / "ro.txt", "new", error));
    EXPECT_NE(error.find("read-only"), std::string::npos) << error;
    EXPECT_EQ(readAll(d.root / "ro.txt"), "keep");
    fs::permissions(d.root / "ro.txt", fs::perms::owner_all, fs::perm_options::replace);
}

TEST(SafeWrite, ALinkOnTheWayIsRefusedAndALinkAtTheNameIsReplaced) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("links");
    // a folder on the way that is a link (to outside, or even inside): refused, naming it and the fix
    fs::create_directory_symlink(d.outside, d.root / "out");
    fs::create_directories(d.root / "real");
    fs::create_directory_symlink(d.root / "real", d.root / "in");
    std::string error;
    EXPECT_FALSE(sw::writeFileBeneath(d.root, d.root / "out" / "x.txt", "x", error));
    EXPECT_NE(error.find((fs::canonical(d.root) / "out").generic_string() + " is a symbolic link"), std::string::npos) << error;
    EXPECT_NE(error.find("Remove the link (or make it a real folder), then try again."), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(d.outside / "x.txt"));
    EXPECT_FALSE(sw::writeFileBeneath(d.root, d.root / "in" / "x.txt", "x", error)) << "no link followed at all";
    EXPECT_FALSE(fs::exists(d.root / "real" / "x.txt"));
    // removals and renames: the same
    writeText(d.outside / "theirs.txt", "theirs");
    EXPECT_FALSE(sw::removeFileBeneath(d.root, d.root / "out" / "theirs.txt", error));
    EXPECT_EQ(readAll(d.outside / "theirs.txt"), "theirs");
    writeText(d.root / "mine.txt", "mine");
    EXPECT_FALSE(sw::renameBeneath(d.root, d.root / "mine.txt", d.root / "out" / "mine.txt", error));
    EXPECT_FALSE(fs::exists(d.outside / "mine.txt"));
    EXPECT_FALSE(sw::createDirBeneath(d.root, d.root / "out" / "made", error));
    EXPECT_FALSE(fs::exists(d.outside / "made"));
    // a link at the final name (live, to outside): replaced by the file, its target untouched
    fs::create_symlink(d.outside / "theirs.txt", d.root / "name.txt");
    ASSERT_TRUE(sw::writeFileBeneath(d.root, d.root / "name.txt", "ours", error)) << error;
    EXPECT_FALSE(fs::is_symlink(d.root / "name.txt"));
    EXPECT_EQ(readAll(d.root / "name.txt"), "ours");
    EXPECT_EQ(readAll(d.outside / "theirs.txt"), "theirs");
    // a removal removes a link itself, never its target
    fs::create_symlink(d.outside / "theirs.txt", d.root / "gone.txt");
    ASSERT_TRUE(sw::removeFileBeneath(d.root, d.root / "gone.txt", error)) << error;
    EXPECT_FALSE(fs::exists(fs::symlink_status(d.root / "gone.txt")));
    EXPECT_EQ(readAll(d.outside / "theirs.txt"), "theirs");
    // a rename moves a link as a link, and never replaces what is at the destination
    fs::create_symlink(d.outside / "theirs.txt", d.root / "moved.txt");
    ASSERT_TRUE(sw::renameBeneath(d.root, d.root / "moved.txt", d.root / "real" / "moved.txt", error)) << error;
    EXPECT_TRUE(fs::is_symlink(d.root / "real" / "moved.txt"));
    EXPECT_FALSE(sw::renameBeneath(d.root, d.root / "mine.txt", d.root / "name.txt", error));
    EXPECT_NE(error.find("already exists"), std::string::npos) << error;
    EXPECT_EQ(readAll(d.root / "name.txt"), "ours");
    // a folder made a link: an empty-folder removal leaves it (and what it leads to) alone
    EXPECT_TRUE(sw::removeEmptyDirBeneath(d.root, d.root / "in", error));
    EXPECT_TRUE(fs::is_symlink(d.root / "in"));
    EXPECT_TRUE(fs::is_directory(d.root / "real"));
}

TEST(SafeWrite, TheRootMayBeALinkAndResolvedWritesFollowLinksInsideIt) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("resolved");
    // the root reached through a link: trusted, resolved once
    fs::create_directory_symlink(d.root, d.base / "via");
    std::string error;
    ASSERT_TRUE(sw::writeFileBeneath(d.base / "via", d.base / "via" / "a.txt", "a", error)) << error;
    EXPECT_EQ(readAll(d.root / "a.txt"), "a");
    // resolve first: a folder link inside the root is followed to its real place
    fs::create_directories(d.root / "real");
    fs::create_directory_symlink(d.root / "real", d.root / "in");
    ASSERT_TRUE(sw::writeFileResolvedBeneath(d.base / "via", d.base / "via" / "in" / "b.txt", "b", error)) << error;
    EXPECT_EQ(readAll(d.root / "real" / "b.txt"), "b");
    // ... a folder link out of it is refused, naming where it leads and the fix
    fs::create_directory_symlink(d.outside, d.root / "out");
    EXPECT_FALSE(sw::writeFileResolvedBeneath(d.root, d.root / "out" / "c.txt", "c", error));
    EXPECT_NE(error.find("resolves outside"), std::string::npos) << error;
    EXPECT_NE(error.find("Remove the link"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(d.outside / "c.txt"));
    EXPECT_FALSE(sw::escapeOf(d.root, d.root / "out" / "c.txt").empty());
    EXPECT_TRUE(sw::escapeOf(d.root, d.root / "in" / "c.txt").empty());
    // a live file link inside: written through (the link stays); out of the root: refused;
    // dangling: replaced, never followed
    writeText(d.root / "real" / "target.txt", "old");
    fs::create_symlink(d.root / "real" / "target.txt", d.root / "inlink.txt");
    ASSERT_TRUE(sw::writeFileResolvedBeneath(d.root, d.root / "inlink.txt", "new", error)) << error;
    EXPECT_TRUE(fs::is_symlink(d.root / "inlink.txt"));
    EXPECT_EQ(readAll(d.root / "real" / "target.txt"), "new");
    writeText(d.outside / "theirs.txt", "theirs");
    fs::create_symlink(d.outside / "theirs.txt", d.root / "outlink.txt");
    EXPECT_FALSE(sw::writeFileResolvedBeneath(d.root, d.root / "outlink.txt", "x", error));
    EXPECT_EQ(readAll(d.outside / "theirs.txt"), "theirs");
    fs::create_symlink(d.outside / "nothing.txt", d.root / "dangling.txt");
    ASSERT_TRUE(sw::writeFileResolvedBeneath(d.root, d.root / "dangling.txt", "here", error)) << error;
    EXPECT_FALSE(fs::is_symlink(d.root / "dangling.txt"));
    EXPECT_FALSE(fs::exists(d.outside / "nothing.txt"));
    // a removal of a link out of the root removes the link (its entry is inside)
    ASSERT_TRUE(sw::removeFileResolvedBeneath(d.root, d.root / "outlink.txt", error)) << error;
    EXPECT_EQ(readAll(d.outside / "theirs.txt"), "theirs");
    // a loop: refused
    fs::create_symlink("loop2", d.root / "loop1");
    fs::create_symlink("loop1", d.root / "loop2");
    EXPECT_FALSE(sw::writeFileResolvedBeneath(d.root, d.root / "loop1" / "x.txt", "x", error));
    EXPECT_NE(error.find("loops"), std::string::npos) << error;
}

TEST(SafeWrite, WriteFilesCmdBeneathARootUndoesAndRefusesLinks) {
    Dirs d("command");
    writeText(d.root / "keep.txt", "before");
    auto failure = std::make_shared<std::string>();
    WriteFilesCmd cmd({{d.root / "keep.txt", "after"}, {d.root / "new" / "deep" / "made.txt", "made"}}, nullptr);
    cmd.reportFailureTo(failure);
    cmd.writeBeneath(d.root);
    ASSERT_TRUE(cmd.execute()) << *failure;
    EXPECT_EQ(readAll(d.root / "keep.txt"), "after");
    EXPECT_EQ(readAll(d.root / "new" / "deep" / "made.txt"), "made");
    cmd.undo();
    EXPECT_EQ(readAll(d.root / "keep.txt"), "before");
    EXPECT_FALSE(fs::exists(d.root / "new")) << "the folders it made go with the undo";
    if (!tests::symlinksPermitted()) return;
    // a folder on the way a link out of the root: the write fails, nothing written, the first file put back
    fs::create_directory_symlink(d.outside, d.root / "out");
    WriteFilesCmd escape({{d.root / "keep.txt", "after"}, {d.root / "out" / "x.txt", "x"}}, nullptr);
    escape.reportFailureTo(failure);
    escape.writeBeneath(d.root);
    EXPECT_FALSE(escape.execute());
    EXPECT_NE(failure->find("resolves outside"), std::string::npos) << *failure;
    EXPECT_EQ(escape.getFailedPath(), d.root / "out" / "x.txt");
    EXPECT_EQ(readAll(d.root / "keep.txt"), "before");
    EXPECT_FALSE(fs::exists(d.outside / "x.txt"));
    // an undo after the folder became a link: nothing removed through it
    fs::create_directories(d.root / "sub");
    WriteFilesCmd later({{d.root / "sub" / "y.txt", "y"}}, nullptr);
    later.writeBeneath(d.root);
    ASSERT_TRUE(later.execute());
    fs::remove_all(d.root / "sub");
    fs::create_directory_symlink(d.outside, d.root / "sub");
    writeText(d.outside / "y.txt", "theirs");
    later.undo();
    EXPECT_EQ(readAll(d.outside / "y.txt"), "theirs");
}
