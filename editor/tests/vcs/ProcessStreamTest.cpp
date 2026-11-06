// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ProcessStream (docs/specs/S27-editor-tools.md R33, AC19): the Recorder's FFmpeg pipe and Git
// import's progress, on S29's argv-only spawn

#include "vcs/ProcessExec.h"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <vector>

using namespace jasperengine::editor::vcs;
namespace fs = std::filesystem;

namespace {

    fs::path helper() {
        return fs::path(JASPER_TEST_VCS_PROCESS_HELPER);
    }

}

TEST(ProcessExec, StreamStdin) {
    ProcessStream process;
    ProcessSpec spec;
    spec.exe = helper();
    spec.args = {"stdin"};
    std::mutex mutex;
    std::vector<std::string> out;
    ProcessStream::Callbacks callbacks;
    callbacks.stdoutLine = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        out.push_back(line);
    };
    std::string error;
    ASSERT_TRUE(process.start(spec, callbacks, error)) << error;
    EXPECT_TRUE(process.running());
    // 4 MiB in frame-sized writes: each blocks until the child reads (back-pressure)
    const std::vector<uint8_t> frame(256 * 1024, 0x5A);
    for (int i = 0; i < 16; i++) ASSERT_TRUE(process.writeStdin(frame.data(), frame.size()));
    process.closeStdin();
    int exitCode = -1;
    ASSERT_TRUE(process.wait(30000, exitCode));
    EXPECT_EQ(exitCode, 0);
    EXPECT_FALSE(process.running());
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], std::to_string(16 * 256 * 1024));
}

TEST(ProcessExec, StreamLines) {
    ProcessStream process;
    ProcessSpec spec;
    spec.exe = helper();
    spec.args = {"lines", "5"};
    std::mutex mutex;
    std::vector<std::string> out, err;
    ProcessStream::Callbacks callbacks;
    callbacks.stdoutLine = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        out.push_back(line);
    };
    callbacks.stderrLine = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        err.push_back(line);
    };
    std::string error;
    ASSERT_TRUE(process.start(spec, callbacks, error)) << error;
    process.closeStdin();
    int exitCode = -1;
    ASSERT_TRUE(process.wait(30000, exitCode));
    EXPECT_EQ(exitCode, 0);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[4], "line 4");
    // \r ends a line too (progress)
    ASSERT_EQ(err.size(), 5u);
    EXPECT_EQ(err[2], "progress 2%");
}

TEST(ProcessExec, StreamArgumentsAndKill) {
    // the arguments reach the child verbatim: no shell between
    {
        ProcessStream process;
        ProcessSpec spec;
        spec.exe = helper();
        spec.args = {"args", "a b", "$(echo no)", "-oProxyCommand=x", "\"q\""};
        std::string all;
        std::mutex mutex;
        ProcessStream::Callbacks callbacks;
        callbacks.stdoutLine = [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex);
            all += line;
        };
        std::string error;
        ASSERT_TRUE(process.start(spec, callbacks, error)) << error;
        process.closeStdin();
        int exitCode = -1;
        ASSERT_TRUE(process.wait(30000, exitCode));
        std::lock_guard<std::mutex> lock(mutex);
        static const char expected[] = "a b\0$(echo no)\0-oProxyCommand=x\0\"q\"";
        // each argument and its NUL (the literal's own NUL ends the last one)
        EXPECT_EQ(all, std::string(expected, sizeof(expected)));
    }
    // kill ends a child that would run for a minute
    ProcessStream process;
    ProcessSpec spec;
    spec.exe = helper();
    spec.args = {"sleep", "60000"};
    std::string error;
    ASSERT_TRUE(process.start(spec, {}, error)) << error;
    int exitCode = 0;
    EXPECT_FALSE(process.wait(100, exitCode));
    const auto start = std::chrono::steady_clock::now();
    process.kill();
    ASSERT_TRUE(process.wait(10000, exitCode));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
    EXPECT_NE(exitCode, 0);
    // a write after the end fails instead of blocking
    EXPECT_FALSE(process.writeStdin("x", 1));
    // and a relative executable never spawns
    ProcessStream relative;
    spec.exe = fs::relative(helper(), fs::current_path());
    EXPECT_FALSE(relative.start(spec, {}, error));
}
