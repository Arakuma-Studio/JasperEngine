// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The export's copies keep the scripts a terminal runs executable (./gradlew, ./jasperengine.py):
// an unchanged file over an older export gets the source's permissions (exportfiles::mirrorTree), and ensureExecutable
// adds the execute bits an installed or unzipped editor lost

#include <gtest/gtest.h>

#include "Exporter.h"
#include "export/ExportFiles.h"
#include "support/TestTempDir.h"
#include "titleart/fonts/WebFontDecode.h"
#include "util/FontFace.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

// A web font ships decoded (Exporter::shipWebFontDecoded): players have no web font decoder.
// Red control: copying the .woff2 as it is ships bytes a player cannot open
TEST(ExportCopy, WebFontsShipDecodedAtTheirPath) {
    namespace wfs = std::filesystem;
    using jasperengine::editor::Exporter;
    namespace titlefonts = jasperengine::editor::titlefonts;
    jasperengine::test::TestTempDir dir{"export-webfont"};
    auto readAll = [](const wfs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    auto writeAll = [](const wfs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    };
    const wfs::path fonts = wfs::path(JASPER_TEST_FIXTURES_DIR) / "titleart" / "fonts";
    const std::vector<uint8_t> ttf = readAll(wfs::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "titleart" / "fonts" / "Oswald-wght.ttf");
    std::vector<uint8_t> damaged = readAll(fonts / "Oswald-wght.woff2");
    ASSERT_FALSE(ttf.empty());
    ASSERT_GT(damaged.size(), 48u);
    damaged.resize(damaged.size() / 2);
    writeAll(dir.path() / "plain.woff", ttf);
    writeAll(dir.path() / "damaged.woff2", damaged);
    writeAll(dir.path() / "brand.ttf", ttf);

    for (const char* name : {"Oswald-wght.woff2", "Oswald-wght.woff"}) {
        const wfs::path shipped = dir.path() / (std::string("out-") + name);
        ASSERT_TRUE(Exporter::shipWebFontDecoded(fonts / name, shipped)) << name;
        const std::vector<uint8_t> bytes = readAll(shipped);
        EXPECT_EQ(titlefonts::sniffFontFormat(bytes.data(), bytes.size()), titlefonts::FontFileFormat::TrueType) << name;
        std::string error;
        EXPECT_NE(jasperengine::FontFace::loadMemory(bytes.data(), bytes.size(), 0, true, name, &error), nullptr) << error;
    }
    // left to the plain copy: a TrueType file named .woff, one that does not decode, not a web font
    EXPECT_FALSE(Exporter::shipWebFontDecoded(dir.path() / "plain.woff", dir.path() / "out-plain.woff"));
    EXPECT_FALSE(Exporter::shipWebFontDecoded(dir.path() / "damaged.woff2", dir.path() / "out-damaged.woff2"));
    EXPECT_FALSE(Exporter::shipWebFontDecoded(dir.path() / "brand.ttf", dir.path() / "out-brand.ttf"));
    EXPECT_FALSE(wfs::exists(dir.path() / "out-damaged.woff2"));
}
