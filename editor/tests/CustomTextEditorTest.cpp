// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// window/widget/CustomTextEditor: comments and strings that span lines are highlighted on
// every line they cover (Lua's --[[ ]] and long brackets, C++'s /* */ and raw strings,
// CMake's #[[ ]]), and auto-indent indents after a block opener (Lua's function/then/do/
// repeat/else, brackets), breaks a block open on Enter and outdents a typed closer
// (end/else/elseif/until, closing brackets).

#include <gtest/gtest.h>

#include "window/widget/CustomTextEditor.h"

#include <cstring>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct CustomTextEditorTestAccess {
        static const std::vector<Token>& tokens(const CustomTextEditor& editor, int line) {
            return editor.lineTokens[line];
        }
        static const std::string& line(const CustomTextEditor& editor, int line) {
            return editor.lines[line];
        }
        // What handleTextInput does with one typed character
        static void type(CustomTextEditor& editor, const std::string& text) {
            for (char c : text) {
                const char utf8[2] = {c, 0};
                editor.InsertText(utf8);
                editor.reindentAfterTyping(static_cast<int>(std::strlen(utf8)));
            }
        }
    };

}

using namespace jasperengine::editor;
using Access = CustomTextEditorTestAccess;

namespace {

    // An editor holding text in language (CustomTextEditor can't be copied or moved)
    struct Editor : CustomTextEditor {
        Editor(SyntaxLanguage language, const std::string& text) {
            SetLanguage(language);
            SetText(text);
        }
    };

    // The type of the token covering column, Default between tokens
    TokenType typeAt(const CustomTextEditor& editor, int line, int column) {
        for (const Token& token : Access::tokens(editor, line)) {
            if (column >= token.start && column < token.start + token.length) return token.type;
        }
        return TokenType::Default;
    }

    std::string cursor(const CustomTextEditor& editor) {
        int line = 0, column = 0;
        editor.GetCursorPosition(line, column);
        return std::to_string(line) + ":" + std::to_string(column);
    }

}

TEST(CustomTextEditorHighlight, LuaBlockCommentSpansLines) {
    Editor editor(SyntaxLanguage::Lua,
        "--[[ first\n"
        "local x = 1\n"
        "]] local y = 2\n"
        "local z = 3");
    EXPECT_EQ(typeAt(editor, 0, 0), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 0, 6), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 1, 10), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 2, 0), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 2, 3), TokenType::Keyword);   // local
    EXPECT_EQ(typeAt(editor, 3, 0), TokenType::Keyword);
}

TEST(CustomTextEditorHighlight, LuaLevelledLongBrackets) {
    Editor editor(SyntaxLanguage::Lua,
        "--[==[ a ]] still\n"
        "comment ]==] local\n"
        "s = [=[ text ]]\n"
        "more ]=] local");
    EXPECT_EQ(typeAt(editor, 0, 12), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 1, 13), TokenType::Keyword);
    EXPECT_EQ(typeAt(editor, 2, 4), TokenType::String);
    EXPECT_EQ(typeAt(editor, 3, 0), TokenType::String);
    EXPECT_EQ(typeAt(editor, 3, 9), TokenType::Keyword);
}

TEST(CustomTextEditorHighlight, LuaLineCommentsAndStringsEndAtTheLine) {
    Editor editor(SyntaxLanguage::Lua,
        "-- not a block [[\n"
        "local a = 'open\n"
        "local b = \"cont\\\n"
        "inued\" local\n"
        "---[[ still a line comment\n"
        "local c");
    EXPECT_EQ(typeAt(editor, 0, 0), TokenType::Comment);
    EXPECT_EQ(typeAt(editor, 1, 0), TokenType::Keyword);
    EXPECT_EQ(typeAt(editor, 1, 10), TokenType::String);
    // An unclosed quote stops at the line end, a trailing backslash carries it on
    EXPECT_EQ(typeAt(editor, 2, 0), TokenType::Keyword);
    EXPECT_EQ(typeAt(editor, 3, 0), TokenType::String);
    EXPECT_EQ(typeAt(editor, 3, 7), TokenType::Keyword);
    EXPECT_EQ(typeAt(editor, 4, 0), TokenType::Comment);
    EXPECT_EQ(typeAt(editor, 5, 0), TokenType::Keyword);
}

TEST(CustomTextEditorHighlight, CppBlockCommentsAndRawStrings) {
    Editor editor(SyntaxLanguage::Cpp,
        "/* a\n"
        "b */ int x;\n"
        "auto s = R\"x(one )\"\n"
        "two)x\"; int y;\n"
        "#include \"a.h\"\n"
        "int z;");
    EXPECT_EQ(typeAt(editor, 0, 0), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 1, 5), TokenType::Type);
    EXPECT_EQ(typeAt(editor, 2, 9), TokenType::String);
    EXPECT_EQ(typeAt(editor, 3, 0), TokenType::String);
    EXPECT_EQ(typeAt(editor, 3, 8), TokenType::Type);
    EXPECT_EQ(typeAt(editor, 4, 0), TokenType::Preprocessor);
    EXPECT_EQ(typeAt(editor, 5, 0), TokenType::Type);
}

TEST(CustomTextEditorHighlight, CMakeBracketComments) {
    Editor editor(SyntaxLanguage::CMake,
        "#[[ a\n"
        "set(X 1)\n"
        "]] set(Y 2)\n"
        "# plain\n"
        "set(Z 3)");
    EXPECT_EQ(typeAt(editor, 1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(typeAt(editor, 2, 3), TokenType::Keyword);
    EXPECT_EQ(typeAt(editor, 3, 0), TokenType::Comment);
    EXPECT_EQ(typeAt(editor, 4, 0), TokenType::Keyword);
}

TEST(CustomTextEditorIndent, LuaIndentsAfterBlockOpeners) {
    const char* openers[] = {"function f()", "if a then", "for i = 1, 3 do", "while a do", "repeat",
                             "else", "local t = {", "x = function(a)", "print("};
    for (const char* opener : openers) {
        Editor editor(SyntaxLanguage::Lua, opener);
        editor.SetCursorPosition(0, static_cast<int>(std::strlen(opener)));
        editor.InsertText("\n");
        EXPECT_EQ(Access::line(editor, 1), "    ") << opener;
        EXPECT_EQ(cursor(editor), "1:4") << opener;
    }

    // Blocks are counted: a block closed on its own line, a comment or a string opens none
    const char* closed[] = {"if a then return end", "x = 1 -- then", "s = \"do\"", "local function f() end", "end"};
    for (const char* line : closed) {
        Editor editor(SyntaxLanguage::Lua, line);
        editor.SetCursorPosition(0, static_cast<int>(std::strlen(line)));
        editor.InsertText("\n");
        EXPECT_EQ(Access::line(editor, 1), "") << line;
    }
}

TEST(CustomTextEditorIndent, EnterBreaksABlockOpen) {
    Editor lua(SyntaxLanguage::Lua, "  if a then end");
    lua.SetCursorPosition(0, 12);
    lua.InsertText("\n");
    EXPECT_EQ(lua.GetText(), "  if a then \n      \n  end");
    EXPECT_EQ(cursor(lua), "1:6");

    Editor cpp(SyntaxLanguage::Cpp, "void f() {}");
    cpp.SetCursorPosition(0, 10);
    cpp.InsertText("\n");
    EXPECT_EQ(cpp.GetText(), "void f() {\n    \n}");
    EXPECT_EQ(cursor(cpp), "1:4");

    // HtmlCssLexer leaves braces out of its tokens
    Editor css(SyntaxLanguage::Css, "a { /* rule */ }");
    css.SetCursorPosition(0, 15);
    css.InsertText("\n");
    EXPECT_EQ(css.GetText(), "a { /* rule */ \n    \n}");

    // Before a closer that opens nothing, the line is outdented
    Editor outdent(SyntaxLanguage::Lua, "    foo() end");
    outdent.SetCursorPosition(0, 10);
    outdent.InsertText("\n");
    EXPECT_EQ(outdent.GetText(), "    foo() \nend");
}

TEST(CustomTextEditorIndent, NoBlockIndentInsideMultiLineCommentsOrStrings) {
    Editor editor(SyntaxLanguage::Lua, "--[[\n  function f()");
    editor.SetCursorPosition(1, 14);
    editor.InsertText("\n");
    EXPECT_EQ(Access::line(editor, 2), "  ");

    Editor css(SyntaxLanguage::Css, "a {");
    css.SetCursorPosition(0, 3);
    css.InsertText("\n");
    EXPECT_EQ(Access::line(css, 1), "    ");
}

TEST(CustomTextEditorIndent, TypedClosersAreOutdented) {
    Editor editor(SyntaxLanguage::Lua, "if a then\n    foo()\n    ");
    editor.SetCursorPosition(2, 4);
    Access::type(editor, "end");
    EXPECT_EQ(Access::line(editor, 2), "end");
    EXPECT_EQ(cursor(editor), "2:3");

    // Growing into a longer word gives the indent back
    Access::type(editor, "point = 1");
    EXPECT_EQ(Access::line(editor, 2), "    endpoint = 1");

    // else stays outdented while it becomes elseif
    Editor elseif(SyntaxLanguage::Lua, "if a then\n    foo()\n    ");
    elseif.SetCursorPosition(2, 4);
    Access::type(elseif, "elseif");
    EXPECT_EQ(Access::line(elseif, 2), "elseif");

    // A bracket lines up with the line that opened it
    Editor cpp(SyntaxLanguage::Cpp, "  int a[] = {\n      1,\n      ");
    cpp.SetCursorPosition(2, 6);
    Access::type(cpp, "}");
    EXPECT_EQ(Access::line(cpp, 2), "  }");

    // Inside a block comment typing is text
    Editor comment(SyntaxLanguage::Lua, "--[[\n    ");
    comment.SetCursorPosition(1, 4);
    Access::type(comment, "end");
    EXPECT_EQ(Access::line(comment, 1), "    end");
}
