// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// util/SafeWrite: writes, removals and renames that stay beneath a root when they happen (no
// symbolic link on the way followed: refused, naming the folder; a link at the final name replaced,
// never followed), the resolve-first variants (links inside the root fine, out of it refused), and
// WriteFilesCmd::writeBeneath built on them. POSIX here (the Windows half is compiled there).

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>

#include "command/CommandHistory.h"
#include "command/type/CopyFileCmd.h"
#include "command/type/CreateDirCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/RemoveFilesCmd.h"
#include "command/type/RenameFileCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "util/FileUtils.h"
#include "util/TerrainMapFileWriter.h"
#include "Generator.h"
#include "htmlui/HtmlUiImportQueue.h"
#include "htmlui/ImportManifestWriter.h"
#include <thread>
#include "util/SafeWrite.h"
#include "SymlinkSupport.h"

#include <sys/stat.h>
#include "support/PosixCompat.h"

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

    // an entry spelled exactly so: where the file system doesn't tell cases apart (APFS), fs::exists of
    // PROJECT.yaml finds the project's own project.yaml
    bool hasEntryNamed(const fs::path& path) {
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(path.parent_path(), ec)) {
            if (entry.path().filename() == path.filename()) return true;
        }
        return false;
    }

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
    Dirs d("readonly");
    writeText(d.root / "ro.txt", "keep");
    fs::permissions(d.root / "ro.txt", fs::perms::owner_read, fs::perm_options::replace);
    std::string error;
#ifdef _WIN32
    const bool root = false;   // the read-only attribute holds for administrators too
#else
    const bool root = ::geteuid() == 0;
#endif
#if defined(__linux__)
    {
        // root ignores file permissions (the build containers run the tests as root): the write is
        // tried as an unprivileged user (nobody, real and effective: the check is faccessat's, of
        // the real user), owning the folder and the read-only file as a user would. Not skipped
        // (no skipped editor test).
        if (root) {
            ASSERT_EQ(::chown(d.root.c_str(), 65534, 65534), 0) << std::strerror(errno);
            ASSERT_EQ(::chown((d.root / "ro.txt").c_str(), 65534, 65534), 0) << std::strerror(errno);
        }
        const bool dropped = root && ::setresuid(65534, 65534, 0) == 0;
        ASSERT_TRUE(!root || dropped) << "setresuid(nobody) failed: " << std::strerror(errno);
        EXPECT_FALSE(sw::writeFileBeneath(d.root, d.root / "ro.txt", "new", error));
        if (dropped) {
            ASSERT_EQ(::setresuid(0, 0, 0), 0) << std::strerror(errno);
        }
    }
#else
    if (root) GTEST_SKIP() << "root ignores file permissions";
    EXPECT_FALSE(sw::writeFileBeneath(d.root, d.root / "ro.txt", "new", error));
#endif
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

// ---- round 2: trees, scopes, commands that record their write root

namespace {

    // a step whose redo fails (its execute changes nothing and says so)
    class FailingRedoCmd : public Command {
    public:
        // S24's step label and type name
        std::string describe() const override { return "FailingRedoCmd"; }
        const char* typeName() const override { return "FailingRedoCmd"; }
        int executions = 0;
        bool execute() override { return ++executions == 1; }
        void undo() override {}
        std::string failure() const override { return "it could not be done again"; }
    };

}

TEST(SafeWrite, RemoveTreeNeverFollowsALink) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("tree");
    writeText(d.outside / "keep" / "theirs.txt", "theirs");
    writeText(d.root / "tree" / "a" / "b" / "file.txt", "x");
    fs::create_directory_symlink(d.outside / "keep", d.root / "tree" / "a" / "out");
    fs::create_symlink(d.outside / "keep" / "theirs.txt", d.root / "tree" / "link.txt");
    std::string error;
    ASSERT_TRUE(sw::removeTreeBeneath(d.root, d.root / "tree", error)) << error;
    EXPECT_FALSE(fs::exists(fs::symlink_status(d.root / "tree")));
    EXPECT_EQ(readAll(d.outside / "keep" / "theirs.txt"), "theirs") << "the links inside were removed as links";
    // a folder on the way swapped for a link before the removal: refused, nothing outside removed
    writeText(d.outside / "victim" / "inner" / "data.txt", "data");
    fs::create_directory_symlink(d.outside / "victim", d.root / "swapped");
    EXPECT_FALSE(sw::removeTreeBeneath(d.root, d.root / "swapped" / "inner", error));
    EXPECT_NE(error.find("is a symbolic link"), std::string::npos) << error;
    EXPECT_EQ(readAll(d.outside / "victim" / "inner" / "data.txt"), "data");
    EXPECT_FALSE(sw::removeTreeResolvedBeneath(d.root, d.root / "swapped" / "inner", error));
    EXPECT_EQ(readAll(d.outside / "victim" / "inner" / "data.txt"), "data");
}

TEST(SafeWrite, CopyReadsThroughLinksAndWritesBeneath) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("copy");
    writeText(d.outside / "library" / "tex" / "a.png", "A");
    fs::create_directory_symlink(d.outside / "library", d.root / "shared");
    std::string error;
    // out of a linked-in library: a read
    ASSERT_TRUE(sw::copyResolvedBeneath(d.root, d.root / "shared" / "tex", d.root / "mine", false, error)) << error;
    EXPECT_EQ(readAll(d.root / "mine" / "a.png"), "A");
    // into a link out of the root: refused, nothing written there, nothing left behind
    fs::create_directory_symlink(d.outside, d.root / "out");
    EXPECT_FALSE(sw::copyResolvedBeneath(d.root, d.root / "mine", d.root / "out" / "copied", false, error));
    EXPECT_FALSE(fs::exists(d.outside / "copied"));
    // an existing file is not replaced without overwrite
    EXPECT_FALSE(sw::copyResolvedBeneath(d.root, d.root / "mine" / "a.png", d.root / "mine" / "a.png", false, error));
    ASSERT_TRUE(sw::copyResolvedBeneath(d.root, d.root / "shared" / "tex" / "a.png", d.root / "mine" / "b.png", true, error)) << error;
}

TEST(SafeWrite, OpenFileBeneathRefusesALinkAndAppends) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("open");
    std::string error;
    std::FILE* file = sw::openFileResolvedBeneath(d.root, d.root / "logs" / "run.log", false, error);
    ASSERT_TRUE(file) << error;
    std::fputs("one\n", file);
    std::fclose(file);
    file = sw::openFileResolvedBeneath(d.root, d.root / "logs" / "run.log", true, error);
    ASSERT_TRUE(file) << error;
    std::fputs("two\n", file);
    std::fclose(file);
    EXPECT_EQ(readAll(d.root / "logs" / "run.log"), "one\ntwo\n");
    writeText(d.outside / "theirs.log", "theirs");
    fs::create_symlink(d.outside / "theirs.log", d.root / "logs" / "planted.log");
    EXPECT_FALSE(sw::openFileBeneath(d.root, d.root / "logs" / "planted.log", true, error)) << "a link at the name";
    EXPECT_EQ(readAll(d.outside / "theirs.log"), "theirs");
}

TEST(SafeWrite, AScopeRoutesFileUtilsWritesBeneathItsRoot) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("scope");
    fs::create_directory_symlink(d.outside, d.root / "out");
    std::error_code ec;
    {
        sw::Scope scope(d.root);
        EXPECT_EQ(sw::Scope::current(), d.root);
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "out" / "x.txt", "x")) << "a link out of the root";
        EXPECT_FALSE(fs::exists(d.outside / "x.txt"));
        EXPECT_NE(sw::lastScopedError().find("resolves outside"), std::string::npos) << sw::lastScopedError();
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "in" / "y.txt", "y")) << "its folders made beneath the root";
        EXPECT_EQ(readAll(d.root / "in" / "y.txt"), "y");
        EXPECT_FALSE(FileUtils::createDirectories(d.root / "out" / "made", ec));
        EXPECT_FALSE(fs::exists(d.outside / "made"));
        EXPECT_FALSE(FileUtils::renamePath(d.root / "in" / "y.txt", d.root / "out" / "y.txt", ec));
        EXPECT_TRUE(fs::exists(d.root / "in" / "y.txt"));
        // a path outside the root (the editor's settings, another project): as before
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.base / "elsewhere.txt", "e"));
        {
            sw::Scope suspended{fs::path()};
            EXPECT_TRUE(sw::Scope::current().empty());
        }
        // a command made in the scope records it
        WriteFilesCmd cmd({{d.root / "made.txt", "m"}}, nullptr);
        EXPECT_EQ(cmd.writeRoot(), d.root);
    }
    EXPECT_TRUE(sw::Scope::current().empty());
    WriteFilesCmd plain({{d.root / "made.txt", "m"}}, nullptr);
    EXPECT_TRUE(plain.writeRoot().empty()) << "outside a scope: none";
    // outside a scope the plain write follows the link, as it always did
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "out" / "z.txt", "z"));
    EXPECT_EQ(readAll(d.outside / "z.txt"), "z");
}

TEST(SafeWrite, ARefusedUndoOrRedoOfAFileCommandKeepsTheHistory) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("history");
    CommandHistory history;
    std::string error;
    // a folder made in a write scope, files put in it, then its parent swapped for a link to a
    // folder of theirs with the same name: the undo is refused, nothing of theirs removed
    fs::create_directories(d.root / "parent");
    {
        sw::Scope scope(d.root);
        ASSERT_TRUE(history.addCommand(new CreateDirCmd("made", (d.root / "parent").string())));
    }
    writeText(d.root / "parent" / "made" / "mine.txt", "mine");
    fs::remove_all(d.root / "parent");
    writeText(d.outside / "made" / "theirs.txt", "theirs");
    fs::create_directory_symlink(d.outside, d.root / "parent");
    const size_t at = history.position();
    EXPECT_FALSE(history.undo());
    EXPECT_EQ(history.position(), at) << "the index does not move";
    EXPECT_NE(history.lastRefusal().find("resolves outside"), std::string::npos) << history.lastRefusal();
    EXPECT_NE(history.lastRefusal().find("Remove the link, then undo again."), std::string::npos) << history.lastRefusal();
    EXPECT_EQ(readAll(d.outside / "made" / "theirs.txt"), "theirs");
    int tries = 0;
    while (history.position() > 0 && history.undo()) tries++;
    EXPECT_EQ(tries, 0) << "a loop to a mark ends";
    // the link gone, a real folder again: the undo goes through
    fs::remove(d.root / "parent");
    writeText(d.root / "parent" / "made" / "again.txt", "again");
    ASSERT_TRUE(history.undo()) << history.lastRefusal();
    EXPECT_FALSE(fs::exists(d.root / "parent" / "made"));
    // a step the GUI made (no write root): an MCP undo checks it against the project (escapeFor)
    writeText(d.root / "gui" / "file.txt", "f");
    RenameFileCmd gui(nullptr, "file.txt", "renamed.txt", (d.root / "gui").string());
    EXPECT_TRUE(gui.writeRoot().empty());
    EXPECT_TRUE(gui.escapeFor(d.root, true).empty());
    fs::remove_all(d.root / "gui");
    fs::create_directory_symlink(d.outside, d.root / "gui");
    EXPECT_NE(gui.escapeFor(d.root, true).find("resolves outside"), std::string::npos);
    EXPECT_TRUE(gui.canUndo()) << "the GUI's own undo is unchanged";
    // a redo whose execute fails is not taken
    CommandHistory redoHistory;
    ASSERT_TRUE(redoHistory.addCommand(new FailingRedoCmd()));
    ASSERT_TRUE(redoHistory.undo());
    EXPECT_FALSE(redoHistory.redo());
    EXPECT_EQ(redoHistory.position(), 0u);
    EXPECT_EQ(redoHistory.lastRefusal(), "it could not be done again");
    EXPECT_TRUE(redoHistory.canRedo());
}

TEST(SafeWrite, CopyAndDeleteCommandsStayBeneathTheirRoot) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("filecmds");
    CommandHistory history;
    writeText(d.root / "src" / "a.txt", "a");
    fs::create_directories(d.root / "dst");
    {
        sw::Scope scope(d.root);
        ASSERT_TRUE(history.addCommand(new CopyFileCmd(nullptr, std::vector<std::string>{(d.root / "src" / "a.txt").string()},
                                                       (d.root / "dst").string(), true)));
    }
    EXPECT_EQ(readAll(d.root / "dst" / "a.txt"), "a");
    // the destination folder swapped for a link to theirs: the copy's undo is refused
    fs::remove_all(d.root / "dst");
    writeText(d.outside / "a.txt", "theirs");
    fs::create_directory_symlink(d.outside, d.root / "dst");
    EXPECT_FALSE(history.undo());
    EXPECT_EQ(readAll(d.outside / "a.txt"), "theirs");
    fs::remove(d.root / "dst");
    writeText(d.root / "dst" / "a.txt", "a");
    ASSERT_TRUE(history.undo()) << history.lastRefusal();
    EXPECT_FALSE(fs::exists(d.root / "dst" / "a.txt"));
    // a delete into the trash and its undo, beneath the root
    {
        sw::Scope scope(d.root);
        ASSERT_TRUE(history.addCommand(new DeleteFileCmd(nullptr, {d.root / "src" / "a.txt"}, d.root)));
    }
    EXPECT_FALSE(fs::exists(d.root / "src" / "a.txt"));
    fs::remove_all(d.root / "src");
    fs::create_directory_symlink(d.outside, d.root / "src");
    EXPECT_FALSE(history.undo()) << "restoring into a link out of the root";
    EXPECT_FALSE(fs::exists(d.outside / "a.txt.restored"));
    fs::remove(d.root / "src");
    fs::create_directories(d.root / "src");
    ASSERT_TRUE(history.undo()) << history.lastRefusal();
    EXPECT_EQ(readAll(d.root / "src" / "a.txt"), "a");
}

// ---- round 3: nested scopes, redo semantics, scoped failures inside commands, more file commands

namespace {

    // a step whose redo finds nothing to do (its GameObjects are gone) and says nothing
    class NothingToRedoCmd : public Command {
    public:
        // S24's step label and type name
        std::string describe() const override { return "NothingToRedoCmd"; }
        const char* typeName() const override { return "NothingToRedoCmd"; }
        int executions = 0;
        bool execute() override { return ++executions == 1; }
        void undo() override {}
    };

    // a step writing one file through FileUtils (in its write scope: beneath the root)
    class ScopedFileCmd : public Command {
    public:
        // S24's step label and type name
        std::string describe() const override { return "ScopedFileCmd"; }
        const char* typeName() const override { return "ScopedFileCmd"; }
        explicit ScopedFileCmd(fs::path file) : file(std::move(file)) {}
        bool execute() override { return FileUtils::writeFileReplacing(file, "made"); }
        void undo() override {
            std::error_code ec;
            FileUtils::writeFileReplacing(file, "undone");
            (void)ec;
        }

    private:
        fs::path file;
    };

}

TEST(SafeWrite, NestedScopesEachApply) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("nested");
    // the inner root is a link to a folder outside the outer root
    fs::create_directories(d.outside / "inner");
    fs::create_directory_symlink(d.outside / "inner", d.root / "inner");
    sw::Scope outer(d.root);
    sw::Scope inner(d.root / "inner");
    EXPECT_EQ(sw::scopeRootFor(d.root / "inner" / "f.txt"), d.root / "inner");
    EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "inner" / "f.txt", "x")) << "the outer scope holds the path too: it applies";
    EXPECT_FALSE(fs::exists(d.outside / "inner" / "f.txt"));
    EXPECT_NE(sw::lastScopedError().find("resolves outside"), std::string::npos) << sw::lastScopedError();
    // fail-closed: a path only the outer scope holds is refused by the inner one
    EXPECT_EQ(sw::scopeRootFor(d.root / "plain.txt"), d.root);
    EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "plain.txt", "p"));
    EXPECT_FALSE(fs::exists(d.root / "plain.txt"));
}

TEST(SafeWrite, ScopesAreFailClosed) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("failclosed");
    std::error_code ec;
    writeText(d.root / "a.txt", "a");
    {
        // the same root twice (an MCP call and the step it runs): renames inside it work, one
        // end outside it is refused
        sw::Scope outer(d.root, true);
        sw::Scope inner(d.root);
        EXPECT_TRUE(FileUtils::renamePath(d.root / "a.txt", d.root / "sub" / ".." / "b.txt", ec)) << sw::lastScopedError();
        EXPECT_FALSE(FileUtils::renamePath(d.root / "b.txt", d.base / "away.txt", ec));
        EXPECT_TRUE(fs::exists(d.root / "b.txt"));
        EXPECT_FALSE(fs::exists(d.base / "away.txt"));
        // a strict scope: a path outside every root is refused, but an allowed folder
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.base / "elsewhere.txt", "x"));
        EXPECT_FALSE(fs::exists(d.base / "elsewhere.txt"));
        fs::create_directories(d.base / "allowed");
        sw::allowOutsideScopes(d.base / "allowed");
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.base / "allowed" / "ok.txt", "x"));
        // a suspended scope: as before
        sw::Scope suspended{fs::path()};
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.base / "elsewhere.txt", "x"));
    }
    // an explicit-root command checks the scopes in force too
    fs::create_directories(d.outside / "linked");
    fs::create_directory_symlink(d.outside / "linked", d.root / "linked");
    sw::Scope scope(d.root, true);
    auto failure = std::make_shared<std::string>();
    WriteFilesCmd cmd({{d.root / "linked" / "x.txt", "x"}}, nullptr);
    cmd.reportFailureTo(failure);
    cmd.writeBeneath(d.root / "linked");
    EXPECT_FALSE(cmd.execute()) << "its own root holds it, the project scope doesn't";
    EXPECT_FALSE(fs::exists(d.outside / "linked" / "x.txt"));
}

TEST(SafeWrite, ARedoWithNothingToDoIsPassedOverAsBefore) {
    CommandHistory history;
    auto* cmd = new NothingToRedoCmd();
    ASSERT_TRUE(history.addCommand(cmd));
    ASSERT_TRUE(history.undo());
    EXPECT_TRUE(history.redo()) << "no reason given: the step is passed over (the old behaviour)";
    EXPECT_EQ(history.position(), 1u);
    EXPECT_TRUE(history.lastRefusal().empty());
}

TEST(SafeWrite, AWriteTheScopeRefusesInsideAStepIsItsFailure) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("stepfail");
    fs::create_directories(d.root / "sub");
    CommandHistory history;
    {
        sw::Scope scope(d.root);
        ASSERT_TRUE(history.addCommand(new ScopedFileCmd(d.root / "sub" / "f.txt")));
    }
    EXPECT_EQ(readAll(d.root / "sub" / "f.txt"), "made");
    // the folder swapped for a link out of the root: the undo (no touchedPaths) is taken, but
    // the write it does is refused by the step's scope and reported
    fs::remove_all(d.root / "sub");
    fs::create_directory_symlink(d.outside, d.root / "sub");
    ASSERT_TRUE(history.undo());
    EXPECT_NE(history.lastUndoProblem().find("resolves outside"), std::string::npos) << history.lastUndoProblem();
    EXPECT_FALSE(fs::exists(d.outside / "f.txt"));
    // its redo fails for the same reason, with a reason: not taken
    EXPECT_FALSE(history.redo());
    EXPECT_EQ(history.position(), 0u);
    EXPECT_NE(history.lastRefusal().find("resolves outside"), std::string::npos) << history.lastRefusal();
    EXPECT_FALSE(fs::exists(d.outside / "f.txt"));
    // a group of steps: its execute says when one of them failed
    CommandHistory grouped;
    {
        sw::Scope scope(d.root);
        const size_t mark = grouped.mark();
        ASSERT_TRUE(grouped.addCommand(new ScopedFileCmd(d.root / "a.txt")));
        fs::remove(d.root / "sub");
        fs::create_directories(d.root / "sub");
        ASSERT_TRUE(grouped.addCommand(new ScopedFileCmd(d.root / "sub" / "b.txt")));
        ASSERT_TRUE(grouped.groupSince(mark));
    }
    ASSERT_TRUE(grouped.undo());
    fs::remove_all(d.root / "sub");
    fs::create_directory_symlink(d.outside, d.root / "sub");
    EXPECT_FALSE(grouped.redo()) << "one step of the group was refused";
    EXPECT_FALSE(fs::exists(d.outside / "b.txt"));
}

TEST(SafeWrite, RemoveFilesAndMaterialCommandsStayBeneathTheirRoot) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("removefiles");
    writeText(d.root / "sub" / "gone.txt", "bytes");
    CommandHistory history;
    {
        sw::Scope scope(d.root);
        ASSERT_TRUE(history.addCommand(new RemoveFilesCmd({d.root / "sub" / "gone.txt"}, nullptr)));
    }
    EXPECT_FALSE(fs::exists(d.root / "sub" / "gone.txt"));
    // its undo would write the bytes back through a folder now a link: refused, nothing written there
    fs::remove_all(d.root / "sub");
    fs::create_directory_symlink(d.outside, d.root / "sub");
    EXPECT_FALSE(history.undo());
    EXPECT_FALSE(fs::exists(d.outside / "gone.txt"));
    fs::remove(d.root / "sub");
    fs::create_directories(d.root / "sub");
    ASSERT_TRUE(history.undo()) << history.lastRefusal();
    EXPECT_EQ(readAll(d.root / "sub" / "gone.txt"), "bytes");
}

TEST(SafeWrite, ALargeCopyIsStreamed) {
    Dirs d("bigcopy");
    std::string big(5u << 20, 'z');
    for (size_t i = 0; i < big.size(); i += 4096) big[i] = static_cast<char>('a' + (i / 4096) % 26);
    writeText(d.outside / "big.bin", big);
    std::string error;
    ASSERT_TRUE(sw::copyResolvedBeneath(d.root, d.outside / "big.bin", d.root / "copies" / "big.bin", false, error)) << error;
    EXPECT_EQ(readAll(d.root / "copies" / "big.bin"), big);
    EXPECT_EQ(entries(d.root / "copies"), 1u) << "no temporary file left";
}

// ---- round 4: sticky failures, group rollback, the terrain map writer, the Lua stubs

TEST(SafeWrite, AScopedFailureStaysAfterALaterSuccess) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("sticky");
    fs::create_directory_symlink(d.outside, d.root / "out");
    sw::Scope scope(d.root);
    const unsigned long long mark = sw::scopedFailureCount();
    EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "out" / "x.txt", "x"));
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "fine.txt", "f"));
    EXPECT_TRUE(sw::lastScopedError().empty()) << "the last one succeeded";
    EXPECT_NE(sw::scopedFailureSince(mark).find("resolves outside"), std::string::npos) << "the first failure since the mark stays";
    EXPECT_TRUE(sw::scopedFailureSince(sw::scopedFailureCount()).empty());
}

TEST(SafeWrite, AFailedGroupRedoTakesBackItsDoneSteps) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("grouproll");
    fs::create_directories(d.root / "sub");
    CommandHistory history;
    {
        sw::Scope scope(d.root);
        const size_t mark = history.mark();
        ASSERT_TRUE(history.addCommand(new ScopedFileCmd(d.root / "a.txt")));
        ASSERT_TRUE(history.addCommand(new ScopedFileCmd(d.root / "sub" / "b.txt")));
        ASSERT_TRUE(history.groupSince(mark));
    }
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(readAll(d.root / "a.txt"), "undone");
    fs::remove_all(d.root / "sub");
    fs::create_directory_symlink(d.outside, d.root / "sub");
    EXPECT_FALSE(history.redo()) << "the second step is refused";
    EXPECT_EQ(history.position(), 0u);
    EXPECT_EQ(readAll(d.root / "a.txt"), "undone") << "the first step was taken back";
    // fixed: the redo goes through whole, then its undo
    fs::remove(d.root / "sub");
    fs::create_directories(d.root / "sub");
    ASSERT_TRUE(history.redo()) << history.lastRefusal();
    EXPECT_EQ(readAll(d.root / "a.txt"), "made");
    EXPECT_EQ(readAll(d.root / "sub" / "b.txt"), "made");
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(readAll(d.root / "sub" / "b.txt"), "undone");
}

TEST(SafeWrite, TheTerrainMapWriterWritesBeneathTheQueuedRoot) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("terrain");
    fs::create_directories(d.root / "maps");
    fs::create_directory_symlink(d.outside, d.root / "linked");
    std::vector<unsigned char> pixels(4 * 4 * 4, 200);
    TerrainMapFileWriter::get().enqueue(d.root / "maps" / "ok.png", 4, 4, 4, 1, pixels, d.root);
    TerrainMapFileWriter::get().enqueue(d.root / "linked" / "bad.png", 4, 4, 4, 1, pixels, d.root);
    EXPECT_FALSE(TerrainMapFileWriter::get().flushAll()) << "the write through the link failed";
    EXPECT_TRUE(fs::is_regular_file(d.root / "maps" / "ok.png"));
    EXPECT_FALSE(fs::exists(d.outside / "bad.png"));
    // forget the failed job (a later flush would retry it)
    fs::remove(d.root / "linked");
    fs::create_directories(d.root / "linked");
    EXPECT_TRUE(TerrainMapFileWriter::get().flushAll());
}

TEST(SafeWrite, LuaStubsAreNeverWrittenOrRemovedThroughALink) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("luastubs");
    const fs::path internal = d.root / ".jasperengine";
    fs::create_directories(internal / "engine-api" / "lua");
    writeText(d.outside / "Theirs.lua", "-- theirs");
    fs::create_directory_symlink(d.outside, internal / "engine-api" / "lua" / "scripts");
    Generator::writeLuaScriptStubs(d.root, internal, {});
    EXPECT_EQ(readAll(d.outside / "Theirs.lua"), "-- theirs") << "a stale-looking stub behind the link is not removed";
}

// ---- round 5: the trusted roots (the project directory and its configured assets directory)

TEST(SafeWrite, ALinkedAssetsRootIsTrustedAndLinksBelowItAreChecked) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("trusted");
    // project/assets -> /elsewhere/assets
    fs::create_directories(d.outside / "assets");
    fs::create_directories(d.outside / "further");
    fs::create_directory_symlink(d.outside / "assets", d.root / "assets");
    sw::Scope scope(std::vector<fs::path>{d.root, d.root / "assets"}, true);
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "assets" / "a.txt", "a")) << sw::lastScopedError();
    EXPECT_EQ(readAll(d.outside / "assets" / "a.txt"), "a") << "landed under the linked assets directory";
    std::error_code ec;
    EXPECT_TRUE(FileUtils::createDirectories(d.root / "assets" / "sub" / "deep", ec)) << sw::lastScopedError();
    EXPECT_TRUE(fs::is_directory(d.outside / "assets" / "sub" / "deep"));
    // a link below the assets root leading further out: refused
    fs::create_directory_symlink(d.outside / "further", d.outside / "assets" / "out");
    EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "assets" / "out" / "x.txt", "x"));
    EXPECT_FALSE(fs::exists(d.outside / "further" / "x.txt"));
    // the project directory itself still a root: its own files written beneath it
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "scene.scene", "s"));
    // a command made in the scope records both roots; its file operations go beneath the one holding the path
    CommandHistory history;
    writeText(d.root / "assets" / "move.txt", "m");
    ASSERT_TRUE(history.addCommand(new RenameFileCmd(nullptr, "move.txt", "moved.txt", (d.root / "assets").string())));
    EXPECT_EQ(readAll(d.outside / "assets" / "moved.txt"), "m");
    ASSERT_TRUE(history.undo()) << history.lastRefusal();
    EXPECT_EQ(readAll(d.outside / "assets" / "move.txt"), "m");
}

// ---- round 6: pinned trusted roots, allowed folders by resolved path, strict inheritance, GUI imports

TEST(SafeWrite, APinnedRootMovedAwayAndReplacedByALinkIsRefused) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("pinned");
    fs::create_directories(d.root / "assets");
    const fs::path pinned = sw::resolvedPath(d.root / "assets");
    sw::pinTrustedRoot(pinned);
    EXPECT_TRUE(sw::trustedRootIntact(pinned));
    std::string error;
    ASSERT_TRUE(sw::writeFileResolvedBeneath(pinned, pinned / "a.txt", "a", error)) << error;
    // the real folder moved away, a link to elsewhere put in its place
    fs::rename(d.root / "assets", d.root / "assets_old");
    fs::create_directory_symlink(d.outside, d.root / "assets");
    EXPECT_FALSE(sw::trustedRootIntact(pinned));
    EXPECT_FALSE(sw::writeFileResolvedBeneath(pinned, pinned / "pwn.txt", "x", error));
    EXPECT_NE(error.find("no longer the folder the project loaded"), std::string::npos) << error;
    EXPECT_FALSE(sw::removeFileResolvedBeneath(pinned, pinned / "keep.txt", error));
    EXPECT_FALSE(fs::exists(d.outside / "pwn.txt"));
    {
        sw::Scope scope(std::vector<fs::path>{sw::resolvedPath(d.root), pinned}, true);
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "assets" / "pwn2.txt", "x"));
        EXPECT_FALSE(fs::exists(d.outside / "pwn2.txt"));
    }
}

TEST(SafeWrite, AllowedFoldersMatchByResolvedPathAndWorkersInheritStrictness) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("allowedresolved");
    fs::create_directories(d.base / "allowedR");
    sw::allowOutsideScopes(d.base / "allowedR");
    // a link under the allowed folder leading elsewhere: not allowed (resolved, not spelling)
    fs::create_directory_symlink(d.outside, d.base / "allowedR" / "out");
    sw::Scope scope(d.root, true);
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.base / "allowedR" / "fine.txt", "f"));
    EXPECT_FALSE(FileUtils::writeFileReplacing(d.base / "allowedR" / "out" / "x.txt", "x"));
    EXPECT_FALSE(fs::exists(d.outside / "x.txt"));
    // a worker started here inherits the roots and the strictness
    const std::vector<fs::path> roots = sw::Scope::currentRoots();
    const bool strict = sw::Scope::currentStrict();
    EXPECT_TRUE(strict);
    bool refused = false;
    std::thread([&]() {
        sw::Scope inherited(roots, strict);
        refused = !FileUtils::writeFileReplacing(d.base / "elsewhere-from-worker.txt", "x");
    }).join();
    EXPECT_TRUE(refused) << "a strict scope's worker refuses writes outside its roots too";
    EXPECT_FALSE(fs::exists(d.base / "elsewhere-from-worker.txt"));
}

TEST(SafeWrite, GuiImportsWorkInALinkedAssetsFolder) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("guiimport");
    fs::create_directories(d.outside / "assets");
    fs::create_directory_symlink(d.outside / "assets", d.root / "assets");
    jasperengine::editor::htmlui::ImportTarget target;
    target.projectRoot = d.root;
    target.assetsDir = d.root / "assets";
    jasperengine::editor::htmlui::ImportQueue queue([](const jasperengine::editor::htmlui::ImportJob& job) {
        jasperengine::editor::htmlui::ImportedResource result;
        result.url = job.url;
        std::string error;
        result.ok = jasperengine::editor::htmlui::ImportManifestWriter::writeFileAtomic(job.target.assetsDir / "imported" / "font.css", "/* css */", &error);
        result.error = error;
        return result;
    });
    jasperengine::editor::htmlui::ImportJob job;
    job.target = target;
    job.url = "https://example.invalid/font.css";
    ASSERT_TRUE(queue.enqueue(job, true));
    queue.waitIdle();
    jasperengine::editor::htmlui::ImportQueue::Batch batch;
    ASSERT_TRUE(queue.takeFinishedBatch(batch));
    ASSERT_EQ(batch.results.size(), 1u);
    EXPECT_TRUE(batch.results[0].ok) << batch.results[0].error;
    EXPECT_EQ(readAll(d.outside / "assets" / "imported" / "font.css"), "/* css */") << "the GUI's import writes into the linked assets folder";
    queue.stop();
}

TEST(SafeWrite, ProjectFileNamesCompareAsTheFileSystemMay) {
    // letter case, trailing dots and spaces (Windows drops them) and an NTFS stream suffix
    for (const char* name : {"project.yaml", "PROJECT.yaml", "Project.YAML", "project.yaml.", "project.yaml ", "project.yaml. .",
                             "project.yaml:stream", "PROJECT.YAML::$DATA"}) {
        EXPECT_EQ(sw::comparableName(fs::path("sub") / name), "project.yaml") << name;
        EXPECT_TRUE(sw::isProjectFileName(fs::path("a") / "b" / name)) << name;
    }
    for (const char* name : {"project.yml", "project.yaml.bak", "myproject.yaml", "project", ".project.yaml"}) {
        EXPECT_FALSE(sw::isProjectFileName(fs::path("sub") / name)) << name;
    }
    EXPECT_EQ(sw::comparableName(fs::path(".JasperEngine.")), ".jasperengine");
    EXPECT_EQ(sw::comparableName(fs::path("CMakeLists.TXT")), "cmakelists.txt");
    EXPECT_EQ(sw::comparableName(fs::path("a") / ".."), "..");
    // folder containment: letter case counts only where the file system counts it
#if defined(_WIN32) || defined(__APPLE__)
    EXPECT_TRUE(sw::isAtOrBelow(fs::path("/p/Assets/x.png"), fs::path("/p/assets")));
#else
    EXPECT_FALSE(sw::isAtOrBelow(fs::path("/p/Assets/x.png"), fs::path("/p/assets")));
#endif
    EXPECT_TRUE(sw::isAtOrBelow(fs::path("/p/assets/x.png"), fs::path("/p/assets/")));
    EXPECT_FALSE(sw::isAtOrBelow(fs::path("/p/assetsx/x.png"), fs::path("/p/assets")));
}

TEST(SafeWrite, AStrictScopeMakesNoSecondProjectFile) {
    Dirs d("projectfile");
    writeText(d.root / "nested" / "project.yaml", "assetsDir: assets\n");   // the user's, from before
    writeText(d.root / "kit" / "inner" / "PROJECT.yaml", "x\n");
    writeText(d.root / "a.txt", "a\n");
    {
        sw::Scope scope(d.root, true);
        // the project's own: the editor saves it, through saveProjectFile's EditorInternal token
        {
            sw::EditorInternal ownProjectFile(d.root / "project.yaml");
            EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "project.yaml", "name: p\n"));
        }
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "project.yaml", "name: q\n")) << "no token: refused";
        for (const char* rel : {"sub/project.yaml", "PROJECT.yaml", "project.yaml.", "sub/Project.Yaml", "project.yaml:alt"}) {
            EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / fs::path(rel), "assetsDir: /etc\n")) << rel;
            EXPECT_FALSE(hasEntryNamed(d.root / fs::path(rel))) << rel;
        }
        EXPECT_EQ(readAll(d.root / "project.yaml"), "name: p\n") << "the project's own stays as the editor saved it";
        std::string error;
        EXPECT_FALSE(sw::writeFileBeneath(d.root, "deep/er/project.yaml", "x", error));
        EXPECT_NE(error.find("project.yaml"), std::string::npos) << error;
        EXPECT_FALSE(sw::createDirBeneath(d.root, "project.YAML", error));
        std::error_code ec;
        EXPECT_FALSE(FileUtils::createDirectories(d.root / "x" / "project.yaml", ec));
        EXPECT_FALSE(FileUtils::renamePath(d.root / "a.txt", d.root / "nested2" / "project.yaml", ec));
        EXPECT_FALSE(FileUtils::renamePath(d.root / "a.txt", d.root / "Project.yaml", ec));
        EXPECT_TRUE(fs::exists(d.root / "a.txt"));
        EXPECT_FALSE(FileUtils::copyPath(d.root / "a.txt", d.root / "copy" / "project.yaml", false, ec));
        EXPECT_FALSE(FileUtils::copyPath(d.root / "kit", d.root / "kit2", false, ec)) << "a folder holding one isn't copied";
        EXPECT_FALSE(fs::exists(d.root / "kit2" / "inner" / "PROJECT.yaml"));
        // one moved as it is (to the trash and back) is not a new one
        fs::create_directories(d.root / ".trash");
        EXPECT_TRUE(FileUtils::renamePath(d.root / "nested" / "project.yaml", d.root / ".trash" / "project.yaml", ec)) << ec.message();
        EXPECT_TRUE(FileUtils::renamePath(d.root / ".trash" / "project.yaml", d.root / "nested" / "project.yaml", ec)) << ec.message();
    }
    {
        // the GUI's scopes (not strict) are unchanged
        sw::Scope scope(d.root);
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "gui" / "project.yaml", "name: g\n"));
    }
    fs::create_directories(d.root / "plain");
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "plain" / "project.yaml", "name: g\n")) << "nor are writes outside any scope";
}

TEST(SafeWrite, AStrictScopeDeniesOwnedAndHiddenPlacesByDefault) {
    Dirs d("ownedplaces");
    writeText(d.root / "assets" / "ok.txt", "a\n");
    {
        sw::Scope scope(d.root, true);
        // hidden folders, project.yaml at any depth, and the root CMakeLists.txt: refused
        for (const char* rel : {".jasperengine/x.txt", ".jasperengine/user/build.yaml", ".git/config",
                                ".trash/x", ".vscode/settings.json", "CMakeLists.txt", "project.yaml",
                                "sub/project.yaml", "Sub/.JasperEngine/y", "CMAKELISTS.TXT"}) {
            std::error_code ec;
            EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / fs::path(rel), "x")) << rel;
            EXPECT_FALSE(fs::exists(d.root / fs::path(rel))) << rel;
            (void)ec;
        }
        // ProjectBuild.cmake is the agent's: allowed. A plain asset: allowed. A nested CMakeLists.txt
        // (not the root one) is the user's: allowed
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "ProjectBuild.cmake", "# mine\n"));
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "assets" / "new.txt", "b\n"));
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "sub" / "CMakeLists.txt", "# nested\n"));
        // an EditorInternal token lets the editor's own writer through, and only at/below its path
        {
            sw::EditorInternal ownInternal(d.root / ".jasperengine");
            EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / ".jasperengine" / "engine-api" / "x.h", "h"));
            std::error_code ec;
            EXPECT_TRUE(FileUtils::createDirectories(d.root / ".jasperengine" / "logs", ec));
            EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / ".git" / "config", "x")) << "the token is narrow";
            EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "project.yaml", "x")) << "the token is narrow";
        }
        {
            sw::EditorInternal ownProjectFile(d.root / "project.yaml");
            EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "project.yaml", "name: p\n")) << "saveProjectFile's token";
        }
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / ".jasperengine" / "after.txt", "x")) << "the token is gone";
    }
    // no scope (the GUI): owned places write normally
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / ".jasperengine" / "gui.txt", "g"));
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "project.yaml", "name: g\n"));
}

TEST(SafeWrite, AnOwnedPlaceIsDeniedWhereItResolvesToo) {
    SKIP_WITHOUT_SYMLINKS();
    Dirs d("ownedresolved");
    fs::create_directories(d.root / ".jasperengine");
    // a link inside the project pointing at the internal folder (or the root) is no way in
    fs::create_directory_symlink(d.root / ".jasperengine", d.root / "intlink");
    fs::create_directory_symlink(d.root, d.root / "rootlink");
    {
        sw::Scope scope(d.root, true);
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "intlink" / "x.txt", "x")) << "through a link to .jasperengine";
        EXPECT_FALSE(fs::exists(d.root / ".jasperengine" / "x.txt"));
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "rootlink" / "project.yaml", "x")) << "through a link to the root";
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.root / "rootlink" / "CMakeLists.txt", "x"));
    }
}

TEST(SafeWrite, OwnedPlacesAreDeniedBelowEveryTrustedRoot) {
    Dirs d("ownedroots");
    // a strict scope with two roots: the project directory and an assets root outside it
    fs::create_directories(d.root);
    fs::create_directories(d.outside / "assets");
    writeText(d.outside / "assets" / "ok.txt", "a\n");
    {
        sw::Scope scope(std::vector<fs::path>{d.root, d.outside / "assets"}, true);
        // owned/hidden places are refused under the SECOND root too, not only the project root
        for (const char* rel : {".git/config", ".jasperengine/x", "sub/project.yaml", "PROJECT.yaml"}) {
            EXPECT_FALSE(FileUtils::writeFileReplacing(d.outside / "assets" / fs::path(rel), "x")) << rel;
            EXPECT_FALSE(fs::exists(d.outside / "assets" / fs::path(rel))) << rel;
        }
        // a plain asset under the assets root is fine
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.outside / "assets" / "sprites" / "hero.png", "p"));
        // the root CMakeLists.txt rule is only for the project directory: a CMakeLists.txt under
        // the assets root (not its top) is the user's, allowed
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.outside / "assets" / "lib" / "CMakeLists.txt", "# ok\n"));
        // project.yaml anywhere (even outside both roots) is refused
        EXPECT_FALSE(FileUtils::writeFileReplacing(d.base / "project.yaml", "x"));
    }
}

#ifndef _WIN32
TEST(SafeWrite, AStrictScopeNeverGrantsExecuteBits) {
    Dirs d("execbits");
    // a file the user made executable (0755)
    writeText(d.root / "run.sh", "#!/bin/sh\n");
    chmod((d.root / "run.sh").c_str(), 0755);
    {
        sw::Scope scope(d.root, true);
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "run.sh", "#!/bin/sh\nid\n"));
    }
    struct stat st{};
    ASSERT_EQ(::stat((d.root / "run.sh").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0111, 0u) << "a strict-scope replace drops the execute bits";
    // a brand-new strict-scope file is not executable either
    {
        sw::Scope scope(d.root, true);
        EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "new.txt", "x"));
    }
    ASSERT_EQ(::stat((d.root / "new.txt").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0111, 0u);
    // outside a strict scope (the GUI) an executable file keeps its bits
    chmod((d.root / "run.sh").c_str(), 0755);
    EXPECT_TRUE(FileUtils::writeFileReplacing(d.root / "run.sh", "#!/bin/sh\necho ok\n"));
    ASSERT_EQ(::stat((d.root / "run.sh").c_str(), &st), 0);
    EXPECT_NE(st.st_mode & 0111, 0u) << "the GUI keeps a file's execute bits";
}
#endif
