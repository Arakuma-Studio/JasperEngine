// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The text of the generated .gitignore and .gitattributes (util/VersionControlFiles.h): the
// editor's block, then the user's lines, kept as they are

#include <gtest/gtest.h>

#include "util/VersionControlFiles.h"

#include <string>

using namespace jasperengine::editor;
using vcsfiles::FileKind;
using vcsfiles::Refresh;

namespace {

    std::string managedVcsText(const std::string& block) {
        return block + vcsfiles::endMarker + "\n";
    }

}

TEST(VersionControlFiles, NewFileIsTheBlockAndTheEndMarker) {
    std::string out;
    EXPECT_EQ(vcsfiles::refresh(std::nullopt, vcsfiles::ignoreBlock(), FileKind::Ignore, out), Refresh::Write);
    EXPECT_EQ(out, managedVcsText(vcsfiles::ignoreBlock()));
    EXPECT_EQ(out.rfind(vcsfiles::startMarker, 0), 0u);
}

TEST(VersionControlFiles, UsersOwnFileIsLeftAlone) {
    std::string out = "untouched";
    EXPECT_EQ(vcsfiles::refresh(std::string("# mine\nbuild/\n"), vcsfiles::ignoreBlock(), FileKind::Ignore, out), Refresh::LeaveAlone);
    EXPECT_EQ(out, "untouched");
    // the marker on a later line is not the editor's file either
    EXPECT_EQ(vcsfiles::refresh(std::string("x\n") + vcsfiles::startMarker + "\n", vcsfiles::ignoreBlock(), FileKind::Ignore, out),
              Refresh::LeaveAlone);
}

TEST(VersionControlFiles, LinesBelowTheEndMarkerAreKeptByteForByte) {
    const std::string user = "\r\n# mine\r\nsecrets.env\r\n\r\n\r\nbuild/\r\n*.log";   // CRLF, blank lines, generated twins, no final newline
    const std::string old = managedVcsText(vcsfiles::attributesBlock(true, false)) + user;
    std::string out;
    ASSERT_EQ(vcsfiles::refresh(old, vcsfiles::attributesBlock(false, true), FileKind::Attributes, out), Refresh::Write);
    EXPECT_EQ(out, managedVcsText(vcsfiles::attributesBlock(false, true)) + user);
    // a refresh of the result is the result
    std::string again;
    vcsfiles::refresh(out, vcsfiles::attributesBlock(false, true), FileKind::Attributes, again);
    EXPECT_EQ(again, out);
}

TEST(VersionControlFiles, OlderFileKeepsOnlyTheUsersLines) {
    // as an editor before the end marker wrote it, LFS on, the user's lines among the editor's
    const std::string old = vcsfiles::attributesBlock(true, true) + "docs/** linguist-documentation\n\n*.foo  binary\n" +
                            "*.scene text eol=lf\n*.bundle    text eol=lf\n";
    std::string out;
    ASSERT_EQ(vcsfiles::refresh(old, vcsfiles::attributesBlock(false, false), FileKind::Attributes, out), Refresh::Write);
    EXPECT_EQ(out, managedVcsText(vcsfiles::attributesBlock(false, false)) + "docs/** linguist-documentation\n\n*.foo  binary\n");
    EXPECT_EQ(out.find("filter=lfs"), std::string::npos) << out;
}

TEST(VersionControlFiles, OlderIgnoreFileWithUserLinesBetweenTheEditors) {
    const std::string block = vcsfiles::ignoreBlock();
    const std::string middle = "# Build output.\nbuild/\n";
    const size_t at = block.find(middle);
    ASSERT_NE(at, std::string::npos);
    const std::string old = "\xEF\xBB\xBF" + block.substr(0, at) + "\n/MyCache/\n\n" + block.substr(at) + "my.local\n";
    std::string out;
    ASSERT_EQ(vcsfiles::refresh(old, block, FileKind::Ignore, out), Refresh::Write);
    EXPECT_EQ(out, managedVcsText(block) + "/MyCache/\n\nmy.local\n");
}

TEST(VersionControlFiles, EveryVariantsLinesAreKnown) {
    for (bool lfs : {false, true}) {
        for (bool lockScenes : {false, true}) {
            std::string out;
            // nothing of another variant counts as the user's
            vcsfiles::refresh(vcsfiles::attributesBlock(lfs, lockScenes), vcsfiles::attributesBlock(!lfs, !lockScenes),
                              FileKind::Attributes, out);
            EXPECT_EQ(out, managedVcsText(vcsfiles::attributesBlock(!lfs, !lockScenes)));
        }
    }
    EXPECT_TRUE(vcsfiles::knownGeneratedLines(FileKind::Ignore).count("build/"));
    EXPECT_TRUE(vcsfiles::knownGeneratedLines(FileKind::Attributes).count("*.png binary"));
}
