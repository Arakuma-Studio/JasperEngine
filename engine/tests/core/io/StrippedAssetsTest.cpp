// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Development Build's left-out assets (docs/specs/S98-editor-tooling-gaps.md REQ-10): a file on
// the manifest's list that does not open logs which setting left it out, once, instead of only a
// plain "not found"; one not on the list logs nothing more.

#include <gtest/gtest.h>

#include "LogCapture.h"

#include "io/File.h"
#include "io/FileData.h"
#include "io/StrippedAssets.h"

using namespace jasperengine;

namespace {

    struct StrippedListScope {
        explicit StrippedListScope(std::map<std::string, std::string> paths) { StrippedAssets::set(std::move(paths)); }
        ~StrippedListScope() { StrippedAssets::set({}); }
    };

}

TEST(StrippedAssets, AMissingStrippedFileNamesTheSettingOnce) {
    StrippedListScope list({{"stripping-test/b.png", "unreferenced"}, {"stripping-test/dev.png", "excluded: assets/dev/**"}});
    test::LogCapture log;
    File file;
    EXPECT_EQ(file.open("stripping-test/b.png"), (unsigned int)FileErrors::FILE_NOT_FOUND);
    EXPECT_EQ(file.open("asset://stripping-test/b.png"), (unsigned int)FileErrors::FILE_NOT_FOUND);
    EXPECT_EQ(log.count(D_LOG_WARN, "stripping-test/b.png was stripped from the build (Asset Stripping: Referenced Only): add it to Always Include"), 1u);
    EXPECT_EQ(file.open("stripping-test/dev.png"), (unsigned int)FileErrors::FILE_NOT_FOUND);
    EXPECT_EQ(log.count(D_LOG_WARN, "stripping-test/dev.png was left out of the build (Exclude From Build: assets/dev/**)"), 1u);
    EXPECT_EQ(file.open("stripping-test/other.png"), (unsigned int)FileErrors::FILE_NOT_FOUND);
    EXPECT_EQ(log.count(D_LOG_WARN, "other.png"), 0u);
}

TEST(StrippedAssets, WithoutAListNothingIsExplained) {
    StrippedAssets::set({});
    EXPECT_TRUE(StrippedAssets::empty());
    EXPECT_FALSE(StrippedAssets::explainMissing("stripping-test/b.png"));
}
