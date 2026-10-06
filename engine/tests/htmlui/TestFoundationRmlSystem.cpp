// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "harness/TestSystem.h"

#include "RmlSystem.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    std::string join(const std::string& document, const std::string& path){
        RmlSystem system;
        Rml::String result;
        system.JoinPath(result, document, path);
        return result;
    }
}

TEST(RmlSystemTest, JoinPathRelative){
    EXPECT_EQ(join("ui/menu.html", "menu.css"), "ui/menu.css");
    EXPECT_EQ(join("ui/menu.html", "img/logo.png"), "ui/img/logo.png");
    EXPECT_EQ(join("menu.html", "menu.css"), "menu.css");
    EXPECT_EQ(join("ui/menu.html", "./menu.css"), "ui/menu.css");
    EXPECT_EQ(join("", "menu.css"), "menu.css");
}

TEST(RmlSystemTest, JoinPathParent){
    EXPECT_EQ(join("ui/a/b.html", "../c.css"), "ui/c.css");
    EXPECT_EQ(join("ui/a/b.html", "../../c.css"), "c.css");
    // above the root is dropped, like FileData::simplifyPath
    EXPECT_EQ(join("ui/b.html", "../../../c.css"), "c.css");
    EXPECT_EQ(join("ui/a/b.html", "x/../y/./z.css"), "ui/a/y/z.css");
}

TEST(RmlSystemTest, JoinPathScheme){
    EXPECT_EQ(join("asset://ui/m.html", "x.png"), "asset://ui/x.png");
    EXPECT_EQ(join("data://saves/ui/m.html", "../x.png"), "data://saves/x.png");
    EXPECT_EQ(join("asset://m.html", "x.png"), "asset://x.png");
}

TEST(RmlSystemTest, JoinPathAbsoluteAndUrl){
    EXPECT_EQ(join("ui/m.html", "/abs/x.css"), "/abs/x.css");
    EXPECT_EQ(join("ui/m.html", "asset://other/x.css"), "asset://other/x.css");
    EXPECT_EQ(join("ui/m.html", "data://x.png"), "data://x.png");
    EXPECT_EQ(join("ui/m.html", "https://example.com/a.css"), "https://example.com/a.css");
    EXPECT_EQ(join("ui/m.html", "C:/fonts/a.ttf"), "C:/fonts/a.ttf");
    EXPECT_EQ(join("ui/m.html", "d:\\fonts\\a.ttf"), "d:\\fonts\\a.ttf");
}

TEST(RmlSystemTest, JoinPathPipeRestored){
    // RmlUi hands document paths over with ':' replaced by '|'
    EXPECT_EQ(join("asset|//ui/m.html", "x.png"), "asset://ui/x.png");
    EXPECT_EQ(join("data|//a/b/m.html", "../x.png"), "data://a/x.png");
}

TEST(RmlSystemTest, JoinPathBackslashes){
    EXPECT_EQ(join("ui\\sub\\m.html", "x.css"), "ui/sub/x.css");
    EXPECT_EQ(join("ui/m.html", "img\\x.png"), "ui/img/x.png");
}

TEST(RmlSystemTest, LogRoutingAndCounters){
    RmlSystem system;
    std::vector<std::pair<Rml::Log::Type, std::string>> heard;
    system.setLogListener([&heard](Rml::Log::Type type, const std::string& message){
        heard.emplace_back(type, message);
    });

    TestSystem& testSystem = TestSystem::get();
    testSystem.clearLogs();

    EXPECT_TRUE(system.LogMessage(Rml::Log::LT_ERROR, "routing-error"));
    EXPECT_TRUE(system.LogMessage(Rml::Log::LT_ASSERT, "routing-assert"));
    EXPECT_TRUE(system.LogMessage(Rml::Log::LT_WARNING, "routing-warning"));
    EXPECT_TRUE(system.LogMessage(Rml::Log::LT_INFO, "routing-info"));
    EXPECT_TRUE(system.LogMessage(Rml::Log::LT_ALWAYS, "routing-always"));
    EXPECT_TRUE(system.LogMessage(Rml::Log::LT_DEBUG, "routing-debug"));

    EXPECT_EQ(system.getErrorCount(), 2u);
    EXPECT_EQ(system.getWarningCount(), 1u);

    EXPECT_EQ(testSystem.countLogs(D_LOG_ERROR, "[HTML UI] routing-error"), 1u);
    EXPECT_EQ(testSystem.countLogs(D_LOG_ERROR, "[HTML UI] routing-assert"), 1u);
    EXPECT_EQ(testSystem.countLogs(D_LOG_WARN, "[HTML UI] routing-warning"), 1u);
    EXPECT_EQ(testSystem.countLogs(D_LOG_VERBOSE, "[HTML UI] routing-info"), 1u);
    EXPECT_EQ(testSystem.countLogs(D_LOG_VERBOSE, "[HTML UI] routing-always"), 1u);
#ifndef NDEBUG
    EXPECT_EQ(testSystem.countLogs(D_LOG_DEBUG, "[HTML UI] routing-debug"), 1u);
#else
    // Log::debug prints nothing in release builds
    EXPECT_EQ(testSystem.countLogs(D_LOG_DEBUG, "[HTML UI] routing-debug"), 0u);
#endif

    ASSERT_EQ(heard.size(), 6u);
    EXPECT_EQ(heard[0].first, Rml::Log::LT_ERROR);
    EXPECT_EQ(heard[0].second, "routing-error");
    EXPECT_EQ(heard[2].first, Rml::Log::LT_WARNING);
    EXPECT_EQ(heard[5].first, Rml::Log::LT_DEBUG);

    // a message with printf conversions is logged as text
    EXPECT_TRUE(system.LogMessage(Rml::Log::LT_WARNING, "100% %s %d"));
    EXPECT_EQ(testSystem.countLogs(D_LOG_WARN, "[HTML UI] 100% %s %d"), 1u);

    system.resetCounters();
    EXPECT_EQ(system.getErrorCount(), 0u);
    EXPECT_EQ(system.getWarningCount(), 0u);
}

TEST(RmlSystemTest, ElapsedTimeIsMonotonic){
    RmlSystem system;
    const double first = system.GetElapsedTime();
    const double second = system.GetElapsedTime();
    EXPECT_GE(first, 0.0);
    EXPECT_GE(second, first);
}

TEST(RmlSystemTest, ClipboardInProcess){
    RmlSystem system;
    Rml::String text = "unchanged";
    system.GetClipboardText(text);
    EXPECT_EQ(text, "");
    system.SetClipboardText("copied ünïcode");
    system.GetClipboardText(text);
    EXPECT_EQ(text, "copied ünïcode");
}
