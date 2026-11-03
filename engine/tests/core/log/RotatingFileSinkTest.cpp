// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "log/LogTestSupport.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    LogRecord makeRecord(const std::string& text) {
        LogRecord record;
        record.id = 1;
        record.level = LogLevel::Info;
        record.category = &logCategoryEditor();
        record.wallTime = 1700000000.0;
        record.text = text;
        return record;
    }

}

TEST(FileSinkTest, RotatesAtRuntime) {
    TempDir dir;
    RotatingFileSink::Options options;
    options.path = dir.file("game.log").string();
    options.maxBytes = 1024 * 1024;
    options.maxFiles = 3;
    RotatingFileSink sink(options);
    ASSERT_TRUE(sink.isOpen());
    const std::string text(1000, 'r');
    const size_t lineBytes = 1000 + 30;
    // 3 MB in batches of 64 lines
    for (int i = 0; i < 3 * 1024; i++) {
        sink.write(makeRecord(text));
        if (i % 64 == 63) sink.flush();
    }
    sink.flush();
    for (const char* name : {"game.log", "game.log.1", "game.log.2", "game.log.3"}) {
        EXPECT_TRUE(dir.exists(name)) << name;
        if (!dir.exists(name)) continue;
        EXPECT_LE(std::filesystem::file_size(dir.file(name)), options.maxBytes + lineBytes) << name;
    }
    EXPECT_FALSE(dir.exists("game.log.4")) << "only maxFiles rotated files are kept";
    EXPECT_FALSE(sink.rotationDisabled());
}

TEST(FileSinkTest, RotatesAtOpen) {
    TempDir dir;
    dir.write("old.log", std::string(4096, 'o'));
    RotatingFileSink::Options options;
    options.path = dir.file("old.log").string();
    options.maxBytes = 1024;
    options.banner = "banner\n";
    RotatingFileSink sink(options);
    sink.flush();
    EXPECT_EQ(dir.read("old.log"), "banner\n");
    EXPECT_EQ(dir.read("old.log.1").size(), 4096u);
}

TEST(FileSinkTest, RotationFailureKeepsAppending) {
    TempDir dir;
    int renames = 0;
    RotatingFileSink::Options options;
    options.path = dir.file("stuck.log").string();
    options.maxBytes = 4096;
    options.rename = [&renames](const std::string&, const std::string&) {
        renames++;
        return false;
    };
    RotatingFileSink sink(options);
    for (int i = 0; i < 200; i++) {
        sink.write(makeRecord("line " + std::to_string(i)));
        sink.flush();
    }
    EXPECT_TRUE(sink.rotationDisabled());
    EXPECT_EQ(renames, 1) << "a failed rotation is not retried per write";
    const std::string text = dir.read("stuck.log");
    for (int i = 0; i < 200; i++) {
        EXPECT_NE(text.find("] line " + std::to_string(i) + "\n"), std::string::npos) << "line " << i << " lost";
    }
}
