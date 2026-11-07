// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A C string macro written into generated CMake (the window title): escaped bytes that come back
// unchanged through CMake's quoted argument and the C literal (util/CMakeText.h)

#include <gtest/gtest.h>

#include "util/CMakeText.h"

#include <string>

using namespace jasperengine::editor;

namespace {

    // what CMake makes of a quoted argument's escapes (\\ \" \$), then what the compiler makes of
    // the C literal's (\ooo, as the helper writes them)
    std::string decodeCMakeCString(const std::string& written) {
        std::string cmake;
        for (size_t i = 0; i < written.size(); i++) {
            if (written[i] == '\\' && i + 1 < written.size()) i++;
            cmake += written[i];
        }
        std::string c;
        for (size_t i = 0; i < cmake.size(); i++) {
            if (cmake[i] == '\\' && i + 3 < cmake.size()) {
                c += static_cast<char>(((cmake[i + 1] - '0') << 6) | ((cmake[i + 2] - '0') << 3) | (cmake[i + 3] - '0'));
                i += 3;
            } else {
                c += cmake[i];
            }
        }
        return c;
    }

}

TEST(CMakeText, EveryTitleComesBackAsTheSameBytes) {
    const std::string titles[] = {
        "A; B", "Game #2", "\"q\" \\ $HOME ${x}", "100% & a|b ^<>,", "\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0 \xF0\x9F\x8E\xAE",
        "x;1", "tab\there", "",
    };
    for (const std::string& title : titles) {
        const std::string body = cmaketext::cStringLiteralBody(title);
        for (char c : body) {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                                 c == '.' || c == '-' || c == '\\';
            EXPECT_TRUE(allowed) << "'" << c << "' in " << body;
        }
        EXPECT_EQ(decodeCMakeCString(cmaketext::cStringDefinitionValue(title)), title) << title;
    }
}

TEST(CMakeText, EscapesAreThreeDigits) {
    // a digit after an escape is not read as part of it
    EXPECT_EQ(cmaketext::cStringLiteralBody("x;1"), "x\\0731");
    EXPECT_EQ(cmaketext::cStringLiteralBody("#"), "\\043");
    EXPECT_EQ(cmaketext::cStringLiteralBody("\xFF"), "\\377");
}

TEST(CMakeText, PlainTitlesStayReadable) {
    EXPECT_EQ(cmaketext::cStringDefinitionValue("My Game 2"), "My Game 2");
    EXPECT_EQ(cmaketext::cStringDefinitionValue("a-b_c.d"), "a-b_c.d");
    // the CMake layer escapes only backslashes, quotes and $
    EXPECT_EQ(cmaketext::cmakeQuoted("a\\b\"c$d"), "a\\\\b\\\"c\\$d");
    EXPECT_EQ(cmaketext::cStringDefinitionValue(";"), "\\\\073");
}
