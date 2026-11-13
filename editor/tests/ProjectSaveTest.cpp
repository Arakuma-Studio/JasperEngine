// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Saving and the child scene graph: FileUtils::writeFileReplacing (a forwarder to S03's
// io::writeFileAtomic, whose own suites are engine/tests/core/io/AtomicFileTest.cpp) leaves the
// old file whole when a write fails, retries a publish that fails for a moment and keeps a
// file's \r\n line endings (FileUtils::withLineEndings, as every document save does), and
// Project refuses a child scene that would make a cycle, also when an undo or redo would.
// The Stop and model load cases run in the editor (tools/mcp/tests/test_project_save.py).

#include <gtest/gtest.h>

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/AddChildSceneCmd.h"
#include "command/type/RemoveChildSceneCmd.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"

#include "support/PosixCompat.h"
#ifdef __linux__
#include <csignal>
#include <sys/resource.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    fs::path tempDir(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / ("jasper-save-test-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeAll(const fs::path& path, const std::string& content) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    // Only the files a test made itself: no temporary file is left next to them
    size_t entryCount(const fs::path& dir) {
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(dir)) {
            (void)entry;
            count++;
        }
        return count;
    }

    SceneProject& addScene(Project& project, uint32_t id, const std::string& name) {
        SceneProject scene;
        scene.id = id;
        scene.name = name;
        project.getScenes().push_back(std::move(scene));
        return project.getScenes().back();
    }

}

// ---- writeFileReplacing ------------------------------------------------------------------

TEST(WriteFileReplacing, ReplacesTheFileAndLeavesNoTemporary) {
    const fs::path dir = tempDir("replace");
    const fs::path file = dir / "Main.scene";
    writeAll(file, "old content that is longer than the new one\n");

    EXPECT_TRUE(FileUtils::writeFileReplacing(file, "new\n"));
    EXPECT_EQ(readAll(file), "new\n");
    EXPECT_EQ(entryCount(dir), 1u);

    // a file that is not there yet is created
    EXPECT_TRUE(FileUtils::writeFileReplacing(dir / "New.prefab", "guid: x\n"));
    EXPECT_EQ(readAll(dir / "New.prefab"), "guid: x\n");
    EXPECT_EQ(entryCount(dir), 2u);
    fs::remove_all(dir);
}

TEST(WriteFileReplacing, AFailedRenameKeepsTheTarget) {
    const fs::path dir = tempDir("rename");
    // a directory in the way: the temporary file is written, the rename fails
    const fs::path target = dir / "Level.scene";
    fs::create_directories(target / "inside");

    EXPECT_FALSE(FileUtils::writeFileReplacing(target, "content\n"));
    EXPECT_TRUE(fs::is_directory(target / "inside"));
    EXPECT_EQ(entryCount(dir), 1u) << "the temporary file is removed";

    // no directory to write into: io::writeFileAtomic makes it (S03 R1)
    EXPECT_TRUE(FileUtils::writeFileReplacing(dir / "missing" / "Level.scene", "content\n"));
    EXPECT_EQ(readAll(dir / "missing" / "Level.scene"), "content\n");
    fs::remove_all(dir);
}

// Windows: a reader or a virus scan holding the file for a moment makes the publish fail with a
// sharing violation; it is tried again before the save gives up (io::renameWithRetry's policy,
// injected here as a busy target)
TEST(WriteFileReplacing, RetriesAPublishThatFailsForAMoment) {
    const fs::path dir = tempDir("retry");
    const fs::path target = dir / "Main.scene";
    writeAll(target, "old\n");

    auto renames = std::make_shared<int>(0);
    io::setAtomicFileFaultInjector([renames](std::string_view step, const fs::path&) -> std::error_code {
        if (step != "rename") return {};
        return ++(*renames) <= 2 ? std::make_error_code(std::errc::device_or_resource_busy) : std::error_code();
    });
    const bool written = FileUtils::writeFileReplacing(target, "saved\n");
    io::setAtomicFileFaultInjector(nullptr);

    EXPECT_TRUE(written);
    EXPECT_EQ(*renames, 3);
    EXPECT_EQ(readAll(target), "saved\n");
    EXPECT_EQ(entryCount(dir), 1u);
    fs::remove_all(dir);
}

// A file of a Windows checkout keeps its \r\n; new files and files with \n get \n
TEST(WriteFileReplacing, KeepsTheLineEndingsOfTheFileItReplaces) {
    const fs::path dir = tempDir("eol");
    const auto keep = FileUtils::LineEndings::KeepExisting;

    const fs::path crlf = dir / "Windows.scene";
    writeAll(crlf, "id: 1\r\nname: Old\r\n");
    EXPECT_TRUE(FileUtils::writeFileReplacing(crlf, "id: 1\nname: New\nscene: {}\n", keep));
    EXPECT_EQ(readAll(crlf), "id: 1\r\nname: New\r\nscene: {}\r\n");
    // text that already has \r\n (a script read on Linux) gets no second \r
    EXPECT_TRUE(FileUtils::writeFileReplacing(crlf, "a\r\nb\nc", keep));
    EXPECT_EQ(readAll(crlf), "a\r\nb\r\nc");

    const fs::path lf = dir / "Linux.scene";
    writeAll(lf, "id: 2\nname: Old\r\n");  // the first line decides
    EXPECT_TRUE(FileUtils::writeFileReplacing(lf, "id: 2\nname: New\n", keep));
    EXPECT_EQ(readAll(lf), "id: 2\nname: New\n");

    EXPECT_TRUE(FileUtils::writeFileReplacing(dir / "New.prefab", "guid: x\nname: y\n", keep));
    EXPECT_EQ(readAll(dir / "New.prefab"), "guid: x\nname: y\n");

    // AsGiven (the default): the bytes as they are, whatever the file had
    const fs::path binary = dir / "Data.bin";
    writeAll(binary, std::string("\x01\r\n\x02", 4));
    const std::string bytes("\x89PNG\n\x1a\n\0", 8);
    EXPECT_TRUE(FileUtils::writeFileReplacing(binary, bytes));
    EXPECT_EQ(readAll(binary), bytes);
    fs::remove_all(dir);
}

#ifdef __linux__
// A write that stops halfway (here the file size limit, as a full disk would) leaves the
// file as it was; a plain std::ofstream would have truncated it first
TEST(WriteFileReplacing, AFailedWriteKeepsTheOldFile) {
    const fs::path dir = tempDir("partial");
    const fs::path file = dir / "project.yaml";
    const std::string original = "name: Original\nscenes: [1, 2, 3]\n";
    writeAll(file, original);

    rlimit previous{};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &previous), 0);
    void (*previousHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
    rlimit small = previous;
    small.rlim_cur = 64;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &small), 0);

    const bool written = FileUtils::writeFileReplacing(file, std::string(64 * 1024, 'x'));

    ::setrlimit(RLIMIT_FSIZE, &previous);
    std::signal(SIGXFSZ, previousHandler);

    EXPECT_FALSE(written);
    EXPECT_EQ(readAll(file), original);
    EXPECT_EQ(entryCount(dir), 1u) << "the temporary file is removed";
    fs::remove_all(dir);
}

TEST(WriteFileReplacing, KeepsPermissionsAndWritesThroughSymlinks) {
    const fs::path dir = tempDir("links");
    const fs::path real = dir / "Real.material";
    writeAll(real, "old\n");
    fs::permissions(real, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read, fs::perm_options::replace);

    EXPECT_TRUE(FileUtils::writeFileReplacing(real, "new\n"));
    EXPECT_EQ(fs::status(real).permissions() & fs::perms::all,
              fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);

    const fs::path link = dir / "Link.material";
    fs::create_symlink(real, link);
    EXPECT_TRUE(FileUtils::writeFileReplacing(link, "through the link\n"));
    EXPECT_TRUE(fs::is_symlink(link)) << "the link stays a link";
    EXPECT_EQ(readAll(real), "through the link\n");
    EXPECT_EQ(entryCount(dir), 2u);

    // a read-only file (a locked one) stays, as a plain write leaves it; root may write anything
#ifdef _WIN32
    const bool root = false;   // the read-only attribute holds for administrators too
#else
    const bool root = ::geteuid() == 0;
#endif
    if (!root) {
        fs::permissions(real, fs::perms::owner_read, fs::perm_options::replace);
        EXPECT_FALSE(FileUtils::writeFileReplacing(real, "replaced\n"));
        EXPECT_EQ(readAll(real), "through the link\n");
        fs::permissions(real, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    }
    fs::remove_all(dir);
}

// A link planted at a temporary file's name never redirects the write: the old fixed name is not
// used any more, and writeNewFile (what writes the temporary file) refuses anything already there
TEST(WriteFileReplacing, APlantedLinkAtTheTemporaryNameCantRedirectTheWrite) {
    const fs::path dir = tempDir("planted");
    const fs::path outside = tempDir("planted-outside");
    const fs::path target = dir / "Hero.png";
    writeAll(target, "old\n");
    // the name writeFileReplacing used to write its temporary file to, linked out
    fs::create_symlink(outside / "stolen.png", dir / ".Hero.png.tmp");
    EXPECT_TRUE(FileUtils::writeFileReplacing(target, "new\n"));
    EXPECT_EQ(readAll(target), "new\n");
    EXPECT_FALSE(fs::exists(outside / "stolen.png")) << "nothing written through the link";
    EXPECT_TRUE(fs::is_symlink(dir / ".Hero.png.tmp")) << "someone else's entry is left alone";
    // exclusive creation: a dangling link, a file, a link to a file are all "already there"
    fs::create_symlink(outside / "dangling.png", dir / "dangling.tmp");
    errno = 0;
    EXPECT_FALSE(FileUtils::writeNewFile(dir / "dangling.tmp", "x", 1));
    EXPECT_EQ(errno, EEXIST);
    EXPECT_FALSE(fs::exists(outside / "dangling.png"));
    writeAll(outside / "theirs.png", "theirs\n");
    fs::create_symlink(outside / "theirs.png", dir / "live.tmp");
    EXPECT_FALSE(FileUtils::writeNewFile(dir / "live.tmp", "x", 1));
    EXPECT_EQ(readAll(outside / "theirs.png"), "theirs\n");
    EXPECT_TRUE(FileUtils::writeNewFile(dir / "fresh.tmp", "abc", 3));
    EXPECT_EQ(readAll(dir / "fresh.tmp"), "abc");
    EXPECT_FALSE(FileUtils::writeNewFile(dir / "fresh.tmp", "x", 1));
    fs::remove_all(dir);
    fs::remove_all(outside);
}
#endif

// ---- child scene cycles --------------------------------------------------------------------

TEST(ChildSceneCycle, AddingAnAncestorIsRefused) {
    Project project;
    addScene(project, 101, "A");
    addScene(project, 102, "B");
    addScene(project, 103, "C");

    EXPECT_TRUE(project.addChildScene(101, 102));
    EXPECT_TRUE(project.addChildScene(102, 103));

    EXPECT_TRUE(project.wouldCreateChildSceneCycle(102, 101));
    EXPECT_TRUE(project.wouldCreateChildSceneCycle(103, 101)) << "a grandparent too";
    EXPECT_TRUE(project.wouldCreateChildSceneCycle(101, 101));
    EXPECT_FALSE(project.wouldCreateChildSceneCycle(101, 103)) << "a grandchild may also be a child";

    EXPECT_FALSE(project.addChildScene(102, 101));
    EXPECT_FALSE(project.addChildScene(103, 101));
    EXPECT_FALSE(project.addChildScene(101, 101));
    EXPECT_FALSE(project.hasChildScene(102, 101));
    EXPECT_FALSE(project.hasChildScene(103, 101));

    EXPECT_TRUE(project.addChildScene(101, 103));
    EXPECT_FALSE(project.addChildScene(101, 103)) << "already a child";
}

TEST(ChildSceneCycle, AnExistingCycleDoesNotHangTheCheck) {
    // as a hand-edited or older project may have it on disk
    Project project;
    addScene(project, 201, "A").childScenes.push_back({202, true});
    addScene(project, 202, "B").childScenes.push_back({201, true});
    addScene(project, 203, "C");

    EXPECT_TRUE(project.wouldCreateChildSceneCycle(201, 202));
    EXPECT_FALSE(project.wouldCreateChildSceneCycle(203, 201));
    EXPECT_FALSE(project.hasSceneUnsavedChanges(201));
}

TEST(ChildSceneCycle, RedoOfAnAddThatWouldNowCycleIsRefused) {
    Project project;
    addScene(project, 301, "A");
    addScene(project, 302, "B");

    CommandHistory historyA(301);
    CommandHistory historyB(302);
    ASSERT_TRUE(historyA.addCommand(new AddChildSceneCmd(&project, 301, 302)));
    ASSERT_TRUE(project.hasChildScene(301, 302));
    EXPECT_FALSE(historyB.addCommand(new AddChildSceneCmd(&project, 302, 301))) << "B already is A's child";

    ASSERT_TRUE(historyA.undo());
    ASSERT_FALSE(project.hasChildScene(301, 302));
    // with the add undone, B may take A
    ASSERT_TRUE(historyB.addCommand(new AddChildSceneCmd(&project, 302, 301)));

    EXPECT_TRUE(historyA.canRedo());
    EXPECT_FALSE(historyA.redo()) << "redoing A -> B would close the cycle";
    EXPECT_FALSE(project.hasChildScene(301, 302));
    EXPECT_TRUE(historyA.canRedo()) << "the history did not move";

    // once B lets go of A, the redo applies again
    ASSERT_TRUE(historyB.undo());
    EXPECT_TRUE(historyA.redo());
    EXPECT_TRUE(project.hasChildScene(301, 302));
}

TEST(ChildSceneCycle, UndoOfARemovalThatWouldNowCycleIsRefused) {
    Project project;
    addScene(project, 401, "A");
    addScene(project, 402, "B");
    ASSERT_TRUE(project.addChildScene(401, 402));

    CommandHistory historyA(401);
    CommandHistory historyB(402);
    ASSERT_TRUE(historyA.addCommand(new RemoveChildSceneCmd(&project, 401, 402)));
    ASSERT_FALSE(project.hasChildScene(401, 402));
    ASSERT_TRUE(historyB.addCommand(new AddChildSceneCmd(&project, 402, 401)));

    EXPECT_FALSE(historyA.undo()) << "restoring A -> B would close the cycle";
    EXPECT_FALSE(project.hasChildScene(401, 402));
    EXPECT_TRUE(historyA.canUndo());

    ASSERT_TRUE(historyB.undo());
    EXPECT_TRUE(historyA.undo());
    EXPECT_TRUE(project.hasChildScene(401, 402));
}
