// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// util/BuiltPlayers: the record a Desktop build leaves in .jasperengine/user/runbuild.yaml and the
// verify() run_build calls before launching a player. A recorded, unchanged file passes; a symbolic
// link, an unrecorded file, a replaced file and a change of size, bytes or mtime are refused, each
// with its own reason. The record's fields: dev/ino and st_mtim on POSIX, last_write_time on Windows.

#include <gtest/gtest.h>

#include "util/BuiltPlayers.h"
#include "SymlinkSupport.h"
#include "yaml-cpp/yaml.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace bp = jasperengine::editor::builtplayers;

namespace {

    struct BuiltDirs {
        fs::path base, internal, exe;
        explicit BuiltDirs(const std::string& name) {
            base = fs::temp_directory_path() / ("jasper-builtplayers-" + name + "-" + std::to_string(JASPER_TEST_GETPID()));
            fs::remove_all(base);
            internal = base / "project" / ".jasperengine";
            fs::create_directories(internal);
            exe = base / "project" / "build" / "desktop" / "game";
            writeExe(exe, "#!/bin/sh\necho built\n");
        }
        ~BuiltDirs() {
            std::error_code ec;
            fs::remove_all(base, ec);
        }
        static void writeExe(const fs::path& path, const std::string& bytes) {
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
            fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
        }
    };

    bool contains(const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    }

}

TEST(BuiltPlayers, RecordedPlayerVerifies) {
    BuiltDirs d("ok");
    ASSERT_TRUE(bp::record(d.internal, d.exe, "desktop"));
    EXPECT_EQ(bp::recordFile(d.internal), d.internal / "user" / "runbuild.yaml");
    ASSERT_TRUE(fs::is_regular_file(bp::recordFile(d.internal)));
    EXPECT_EQ(bp::verify(d.internal, d.exe), "");

    // the record holds exactly what stat and the bytes say (the format the MCP test fixture writes)
    const YAML::Node root = YAML::LoadFile(bp::recordFile(d.internal).string());
    ASSERT_TRUE(root["desktop"].IsMap());
    const YAML::Node e = root["desktop"];
    EXPECT_EQ(e["resolved"].as<std::string>(), fs::canonical(d.exe).string());
#ifdef _WIN32
    EXPECT_EQ(e["dev"].as<std::uint64_t>(), 0u);
    EXPECT_EQ(e["ino"].as<std::uint64_t>(), 0u);
    EXPECT_EQ(e["size"].as<std::uint64_t>(), static_cast<std::uint64_t>(fs::file_size(d.exe)));
    EXPECT_EQ(e["mtime"].as<std::int64_t>(), static_cast<std::int64_t>(fs::last_write_time(d.exe).time_since_epoch().count()));
#else
    struct stat st{};
    ASSERT_EQ(::stat(d.exe.c_str(), &st), 0);
    EXPECT_EQ(e["dev"].as<std::uint64_t>(), static_cast<std::uint64_t>(st.st_dev));
    EXPECT_EQ(e["ino"].as<std::uint64_t>(), static_cast<std::uint64_t>(st.st_ino));
    EXPECT_EQ(e["size"].as<std::uint64_t>(), static_cast<std::uint64_t>(st.st_size));
#if defined(__APPLE__)
    EXPECT_EQ(e["mtime"].as<std::int64_t>(), static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec);
#else
    EXPECT_EQ(e["mtime"].as<std::int64_t>(), static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec);
#endif
#endif
    const std::string sha = e["sha256"].as<std::string>();
    EXPECT_EQ(sha.size(), 64u);
    EXPECT_EQ(sha.find_first_not_of("0123456789abcdef"), std::string::npos);

    const bp::Fingerprint was = bp::recorded(d.internal, fs::canonical(d.exe));
    ASSERT_TRUE(was.valid);
    EXPECT_EQ(was.sha256, sha);
}

TEST(BuiltPlayers, RecordKeepsOtherModesAndRecordingAMissingFileFails) {
    BuiltDirs d("modes");
    const fs::path other = d.base / "project" / "build" / "other" / "game";
    BuiltDirs::writeExe(other, "other player\n");
    ASSERT_TRUE(bp::record(d.internal, other, "other"));
    ASSERT_TRUE(bp::record(d.internal, d.exe, "desktop"));
    EXPECT_EQ(bp::verify(d.internal, d.exe), "");
    EXPECT_EQ(bp::verify(d.internal, other), "");
    EXPECT_FALSE(bp::record(d.internal, d.base / "missing", "desktop"));
    EXPECT_EQ(bp::verify(d.internal, d.exe), "");   // a failed record leaves the old one
}

TEST(BuiltPlayers, NoRecordOrNoEntryIsRefused) {
    BuiltDirs d("none");
    // no runbuild.yaml at all
    EXPECT_TRUE(contains(bp::verify(d.internal, d.exe), "not built by this editor")) << bp::verify(d.internal, d.exe);
    // a record that holds another file only
    const fs::path other = d.base / "project" / "build" / "desktop" / "other";
    BuiltDirs::writeExe(other, "#!/bin/sh\necho built\n");
    ASSERT_TRUE(bp::record(d.internal, other, "desktop"));
    EXPECT_TRUE(contains(bp::verify(d.internal, d.exe), "not built by this editor")) << bp::verify(d.internal, d.exe);
    // a file that can't be read at all
    EXPECT_TRUE(contains(bp::verify(d.internal, d.base / "missing"), "can't be read"));
    // a damaged record file refuses, never crashes
    std::ofstream(bp::recordFile(d.internal), std::ios::binary | std::ios::trunc) << "desktop: [unclosed\n";
    EXPECT_TRUE(contains(bp::verify(d.internal, other), "not built by this editor"));
}

TEST(BuiltPlayers, ReplacedFileIsRefused) {
    BuiltDirs d("replaced");
    ASSERT_TRUE(bp::record(d.internal, d.exe, "desktop"));
    // the same bytes in a new file renamed over the recorded one: another inode
    const fs::path fresh = d.exe.parent_path() / "game.new";
    BuiltDirs::writeExe(fresh, "#!/bin/sh\necho built\n");
    fs::rename(fresh, d.exe);
    EXPECT_TRUE(contains(bp::verify(d.internal, d.exe), "replaced since")) << bp::verify(d.internal, d.exe);
}

TEST(BuiltPlayers, SizeChangeIsRefused) {
    BuiltDirs d("size");
    ASSERT_TRUE(bp::record(d.internal, d.exe, "desktop"));
    std::ofstream(d.exe, std::ios::binary | std::ios::app) << "echo more\n";
    EXPECT_TRUE(contains(bp::verify(d.internal, d.exe), "contents changed")) << bp::verify(d.internal, d.exe);
}

TEST(BuiltPlayers, SameSizeContentChangeIsRefused) {
    BuiltDirs d("content");
    ASSERT_TRUE(bp::record(d.internal, d.exe, "desktop"));
    const auto mtime = fs::last_write_time(d.exe);
    {
        std::fstream f(d.exe, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(-2, std::ios::end);
        f << "X";
    }
    fs::last_write_time(d.exe, mtime);   // the time put back: only the bytes give it away
    EXPECT_EQ(fs::file_size(d.exe), static_cast<std::uintmax_t>(std::string("#!/bin/sh\necho built\n").size()));
    EXPECT_TRUE(contains(bp::verify(d.internal, d.exe), "contents changed")) << bp::verify(d.internal, d.exe);
}

TEST(BuiltPlayers, MtimeChangeIsRefused) {
    BuiltDirs d("mtime");
    ASSERT_TRUE(bp::record(d.internal, d.exe, "desktop"));
    fs::last_write_time(d.exe, fs::last_write_time(d.exe) - std::chrono::seconds(30));
    EXPECT_TRUE(contains(bp::verify(d.internal, d.exe), "modified since")) << bp::verify(d.internal, d.exe);
}

TEST(BuiltPlayers, SymbolicLinkIsRefused) {
    if (!jasperengine::editor::tests::symlinksPermitted()) GTEST_SKIP() << "symbolic links not permitted here";
    BuiltDirs d("link");
    ASSERT_TRUE(bp::record(d.internal, d.exe, "desktop"));
    const fs::path link = d.exe.parent_path() / "game-link";
    fs::create_symlink(d.exe, link);
    EXPECT_TRUE(contains(bp::verify(d.internal, link), "symbolic link")) << bp::verify(d.internal, link);
    // the recorded file itself still runs
    EXPECT_EQ(bp::verify(d.internal, d.exe), "");
    // a link recorded as the player is refused too (record resolves it; the link is never run)
    ASSERT_TRUE(bp::record(d.internal, link, "desktop"));
    EXPECT_TRUE(contains(bp::verify(d.internal, link), "symbolic link"));
}
