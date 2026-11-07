// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The export's copies keep the scripts a terminal runs executable (./gradlew, ./jasperengine.py):
// an unchanged file over an older export gets the source's permissions (exportfiles::mirrorTree), and ensureExecutable
// adds the execute bits an installed or unzipped editor lost

#include <gtest/gtest.h>

#include "Exporter.h"
#include "export/ExportFiles.h"
#include "support/TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct ExportCopyDir {
        jasperengine::test::TestTempDir dir{"export-copy"};
        const fs::path& root = dir.path();
    };

    void writeExportCopyFile(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    fs::perms exportCopyPerms(const fs::path& path) {
        return fs::status(path).permissions() & fs::perms::mask;
    }

}

TEST(ExportCopy, UnchangedFileGetsTheSourcesPermissions) {
    ExportCopyDir dir;
    const fs::path src = dir.root / "src", dst = dir.root / "dst";
    writeExportCopyFile(src / "workspaces" / "gradlew", "#!/bin/sh\n");
    fs::permissions(src / "workspaces" / "gradlew", fs::perms(0755), fs::perm_options::replace);
    // an older export: the same bytes without the execute bits
    writeExportCopyFile(dst / "workspaces" / "gradlew", "#!/bin/sh\n");
    fs::permissions(dst / "workspaces" / "gradlew", fs::perms(0644), fs::perm_options::replace);
    const auto mtime = fs::last_write_time(dst / "workspaces" / "gradlew");

    std::string error;
    ASSERT_TRUE(exportfiles::mirrorTree(src, dst, false, error)) << error;
    EXPECT_EQ(exportCopyPerms(dst / "workspaces" / "gradlew"), fs::perms(0755));
    // still unchanged for the incremental build (compared as counts: gtest would print a time_point
    // through libc++'s std::format, whose floating-point to_chars macOS has only from 13.3)
    EXPECT_EQ(fs::last_write_time(dst / "workspaces" / "gradlew").time_since_epoch().count(), mtime.time_since_epoch().count());
}

TEST(ExportCopy, EnsureExecutableAddsTheExecuteBits) {
    ExportCopyDir dir;
    const fs::path script = dir.root / "jasperengine.py";
    writeExportCopyFile(script, "#!/usr/bin/env python3\n");
    fs::permissions(script, fs::perms(0640), fs::perm_options::replace);
    std::string error;
    EXPECT_TRUE(Exporter::ensureExecutable({script}, error)) << error;
    EXPECT_EQ(exportCopyPerms(script), fs::perms(0751));
    EXPECT_TRUE(::access(script.c_str(), X_OK) == 0);

    EXPECT_FALSE(Exporter::ensureExecutable({dir.root / "missing" / "gradlew"}, error));
    EXPECT_NE(error.find("missing/gradlew"), std::string::npos) << error;
}
#endif
