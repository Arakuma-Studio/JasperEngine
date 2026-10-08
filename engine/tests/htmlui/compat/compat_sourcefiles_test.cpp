// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SourceFiles: ids, offset -> line/column with CR, LF and CRLF line ends, UTF-8 columns, the
// BOM, include chains; UTF-8 sanitizing.

#include "htmlui/compat/SourceFiles.h"

#include <gtest/gtest.h>

using namespace jasperengine::htmlui::compat;

TEST(CompatSourceFiles, IdsPathsAndText){
    SourceFiles files;
    EXPECT_EQ(files.count(), 0u);
    const uint32_t a = files.add("ui/a.css", "a{}");
    const uint32_t b = files.add("ui/b.css", "b{}");
    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, 2u);
    EXPECT_EQ(files.add("ui/a.css", "other"), a);   // once per session
    EXPECT_EQ(files.text(a), "a{}");
    EXPECT_EQ(files.path(b), "ui/b.css");
    EXPECT_EQ(files.find("ui/b.css"), b);
    EXPECT_EQ(files.find("ui/c.css"), 0u);
    EXPECT_EQ(files.path(0), "");
    EXPECT_EQ(files.path(9), "");
    EXPECT_EQ(files.text(0), "");
    EXPECT_EQ(files.position(0, 3), SourcePos{});
    EXPECT_EQ(files.count(), 2u);
}

TEST(CompatSourceFiles, LineEndsCrLfAndCrlf){
    SourceFiles files;
    // line 1 "ab", LF; line 2 "cd", CRLF; line 3 "ef", CR; line 4 "gh"
    const uint32_t file = files.add("x.css", "ab\ncd\r\nef\rgh");
    EXPECT_EQ(files.position(file, 0), (SourcePos{1, 1}));
    EXPECT_EQ(files.position(file, 1), (SourcePos{1, 2}));
    EXPECT_EQ(files.position(file, 2), (SourcePos{1, 3}));   // the '\n' itself
    EXPECT_EQ(files.position(file, 3), (SourcePos{2, 1}));
    EXPECT_EQ(files.position(file, 5), (SourcePos{2, 3}));   // '\r' of CRLF
    EXPECT_EQ(files.position(file, 6), (SourcePos{2, 4}));   // '\n' of CRLF: still line 2
    EXPECT_EQ(files.position(file, 7), (SourcePos{3, 1}));
    EXPECT_EQ(files.position(file, 10), (SourcePos{4, 1}));
    EXPECT_EQ(files.position(file, 11), (SourcePos{4, 2}));
    // past the end: clamped to the end
    EXPECT_EQ(files.position(file, 100), (SourcePos{4, 3}));
}

TEST(CompatSourceFiles, ColumnsCountCodePoints){
    SourceFiles files;
    // "é" is 2 bytes, "€" 3, "😀" 4
    const std::string text = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80x";
    const uint32_t file = files.add("x.css", text);
    EXPECT_EQ(files.position(file, 0).column, 1u);
    EXPECT_EQ(files.position(file, 2).column, 2u);
    EXPECT_EQ(files.position(file, 5).column, 3u);
    EXPECT_EQ(files.position(file, 9).column, 4u);
    EXPECT_EQ(files.codePointLength(SourceRef{file, 0, (uint32_t)text.size()}), 4u);
    EXPECT_EQ(files.slice(SourceRef{file, 9, 1}), "x");
    EXPECT_EQ(files.slice(SourceRef{file, 9, 50}), "x");
    EXPECT_EQ(files.slice(SourceRef{file, 50, 1}), "");
}

TEST(CompatSourceFiles, BomIsStrippedBeforeOffsets){
    std::string text = "\xEF\xBB\xBF" "a{}";
    EXPECT_TRUE(stripUtf8Bom(text));
    EXPECT_EQ(text, "a{}");
    EXPECT_FALSE(stripUtf8Bom(text));
    SourceFiles files;
    const uint32_t file = files.add("x.css", text);
    EXPECT_EQ(files.position(file, 0), (SourcePos{1, 1}));
}

TEST(CompatSourceFiles, IncludeChainOrigin){
    SourceFiles files;
    const uint32_t html = files.add("ui/menu.html", "<html>\n<link rel=stylesheet href=menu.css>");
    const uint32_t css = files.add("ui/menu.css", "@import 'base.css';\n", SourceRef{html, 7, 36});
    const uint32_t base = files.add("ui/base.css", "a{}", SourceRef{css, 0, 19});
    EXPECT_EQ(files.includedFrom(base).file, css);
    EXPECT_EQ(files.originText(base), "imported by ui/menu.css:1");
    EXPECT_EQ(files.originText(css), "linked by ui/menu.html:2");
    EXPECT_EQ(files.originText(html), "");
}

TEST(CompatSourceFiles, SanitizeUtf8){
    std::string good = "ok \xC3\xA9";
    EXPECT_TRUE(sanitizeUtf8(good).empty());
    EXPECT_EQ(good, "ok \xC3\xA9");

    std::string bad = std::string("a\xFF" "b\xC3") + "c" + '\0' + "d\xED\xA0\x80";
    const std::vector<uint32_t> replaced = sanitizeUtf8(bad);
    ASSERT_EQ(replaced.size(), 6u);   // FF, a lone C3, NUL, and the 3 bytes of an encoded surrogate
    EXPECT_EQ(replaced[0], 1u);
    EXPECT_EQ(bad.substr(0, 5), "a\xEF\xBF\xBD" "b");
    EXPECT_EQ(bad.find('\0'), std::string::npos);
}

TEST(CompatSourceFiles, CopiesAreIndependent){
    SourceFiles files;
    files.add("a.css", "a");
    SourceFiles copy = files;
    copy.add("b.css", "b");
    EXPECT_EQ(files.count(), 1u);
    EXPECT_EQ(copy.count(), 2u);
    EXPECT_EQ(copy.text(1), "a");
}

TEST(CompatSourceRef, CoverAndEquality){
    const SourceRef a{1, 10, 5};
    const SourceRef b{1, 20, 2};
    EXPECT_EQ(a.cover(b), (SourceRef{1, 10, 12}));
    EXPECT_EQ(b.cover(a), (SourceRef{1, 10, 12}));
    EXPECT_EQ(a.cover(SourceRef{}), a);
    EXPECT_EQ(SourceRef{}.cover(b), b);
    EXPECT_EQ(a.cover(SourceRef{2, 0, 1}), a);
    EXPECT_FALSE(SourceRef{}.valid());
    RcssLineMap lines;
    lines.lines = {a, SourceRef{}, b};
    EXPECT_EQ(lines.at(1), a);
    EXPECT_FALSE(lines.at(2).valid());
    EXPECT_FALSE(lines.at(0).valid());
    EXPECT_FALSE(lines.at(4).valid());
}
