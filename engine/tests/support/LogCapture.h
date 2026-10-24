// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_LOG_CAPTURE_H
#define JASPER_TEST_LOG_CAPTURE_H

#include "TestSystem.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // The engine log lines (Log::error, Log::warn, ...) logged through the TestSystem since this
    // capture started. TestMain marks the start of every test, so sinceTestStart() is a capture
    // of the running test.
    class LogCapture {
    public:
        LogCapture();                       // starts now
        static LogCapture sinceTestStart();

        std::vector<TestSystem::LogEntry> entries() const;
        // entries of that type (D_LOG_ERROR, D_LOG_WARN, ...) whose message contains `text`
        std::vector<TestSystem::LogEntry> entries(int type, const std::string& text = "") const;
        size_t count(int type, const std::string& text = "") const;
        std::vector<TestSystem::LogEntry> errors() const;
        std::vector<TestSystem::LogEntry> warnings() const;
        bool contains(const std::string& text) const;     // any type

        // the errors, one per line, for a failure message
        std::string describeErrors() const;

        // called by TestMain's listener when a test starts
        static void markTestStart();

    private:
        explicit LogCapture(size_t start);
        size_t start;
    };

} }

// fails the test (non-fatally) when the engine logged an error since the test started
#define EXPECT_NO_ENGINE_ERRORS() \
    do { \
        const ::jasperengine::test::LogCapture logCapture_ = ::jasperengine::test::LogCapture::sinceTestStart(); \
        EXPECT_TRUE(logCapture_.errors().empty()) << "engine errors:\n" << logCapture_.describeErrors(); \
    } while (0)

#endif //JASPER_TEST_LOG_CAPTURE_H
