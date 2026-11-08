// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A compiler error in the Console counts as an Error (export-pipeline spec 6.10): build lines keep
// their hammer but go to the counter and filter of their own level.

#include "window/ConsoleWindow.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(ConsoleSeverity, BuildErrorLinesCountAsErrors) {
    EXPECT_EQ(ConsoleWindow::severityOf(LogType::Build, LogLevel::Error), ConsoleWindow::SEVERITY_ERROR_MESSAGE);
    EXPECT_EQ(ConsoleWindow::severityOf(LogType::Build, LogLevel::Warn), ConsoleWindow::SEVERITY_WARNING);
    EXPECT_EQ(ConsoleWindow::severityOf(LogType::Build, LogLevel::Info), ConsoleWindow::SEVERITY_LOG);
    EXPECT_EQ(ConsoleWindow::severityOf(LogType::Error, LogLevel::Error), ConsoleWindow::SEVERITY_ERROR_MESSAGE);
    EXPECT_EQ(ConsoleWindow::severityOf(LogType::Warning, LogLevel::Warn), ConsoleWindow::SEVERITY_WARNING);
    EXPECT_EQ(ConsoleWindow::severityOf(LogType::Info, LogLevel::Info), ConsoleWindow::SEVERITY_LOG);
}
