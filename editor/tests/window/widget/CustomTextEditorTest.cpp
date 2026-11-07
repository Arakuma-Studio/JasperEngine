// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Public text editing operations and keyboard input preserve block indentation; comments and
// strings that span lines are highlighted on every line; the version control layer (change
// markers, Revert Change, Next/Previous Change, the diff popup) follows edits.
// Keyboard and mouse checks use ImGui input and the real renderer without a display or GPU.

#include <gtest/gtest.h>

#include "window/widget/CustomTextEditor.h"
#include "window/widget/SemanticSuggestions.h"

#include "imgui_internal.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine::editor;

namespace {

    // An editor holding text in language (CustomTextEditor can't be copied or moved)
    struct Editor : CustomTextEditor {
        Editor(SyntaxLanguage language, const std::string& text) {
            SetLanguage(language);
            SetText(text);
        }
    };

    struct EditorInput {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGuiContext* context = ImGui::CreateContext();

        EditorInput() {
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(800, 600);
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }

        ~EditorInput() {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        }

        void type(CustomTextEditor& editor, const char* text) {
            ImGui::SetCurrentContext(context);
            ImGui::GetIO().AddInputCharactersUTF8(text);
            frame(editor);
        }

        // One frame of the editor in a window at the top-left corner
        void frame(CustomTextEditor& editor) {
            ImGui::SetCurrentContext(context);
            editor.SetAutoComplete(false);
            editor.RequestFocus();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(800, 600));
            ImGui::Begin("Code editor");
            editor.Render("Text");
            ImGui::End();
            ImGui::EndFrame();
        }

        void key(CustomTextEditor& editor, ImGuiKey key, bool alt, bool shift) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Alt, alt);
            io.AddKeyEvent(ImGuiMod_Shift, shift);
            io.AddKeyEvent(key, true);
            frame(editor);
            io.AddKeyEvent(key, false);
            io.AddKeyEvent(ImGuiMod_Alt, false);
            io.AddKeyEvent(ImGuiMod_Shift, false);
            frame(editor);
        }

        // Where the editor draws text: its child window's content start
        ImVec2 textOrigin() const {
            for (ImGuiWindow* window : context->Windows) {
                if ((window->Flags & ImGuiWindowFlags_ChildWindow) && std::strstr(window->Name, "Text")) {
                    return window->DC.CursorStartPos;
                }
            }
            return ImVec2(0, 0);
        }

        // The default font's 'X' width and size, as the editor measures them inside a frame
        ImVec2 fontMetrics() {
            ImGui::SetCurrentContext(context);
            ImGui::NewFrame();
            const ImVec2 metrics(ImGui::CalcTextSize("X").x, ImGui::GetFontSize());
            ImGui::EndFrame();
            return metrics;
        }

        void click(CustomTextEditor& editor, ImVec2 at) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y);
            frame(editor);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame(editor);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame(editor);
        }
    };

    std::shared_ptr<const std::vector<std::string>> baseOf(const std::vector<std::string>& lines) {
        return std::make_shared<const std::vector<std::string>>(lines);
    }

    std::string cursor(const CustomTextEditor& editor) {
        int line = 0, column = 0;
        editor.GetCursorPosition(line, column);
        return std::to_string(line) + ":" + std::to_string(column);
    }

}

TEST(CustomTextEditorIndent, LuaIndentsAfterBlockOpeners) {
    const char* openers[] = {"function f()", "if a then", "for i = 1, 3 do", "while a do", "repeat",
                             "else", "local t = {", "x = function(a)", "print("};
    for (const char* opener : openers) {
        Editor editor(SyntaxLanguage::Lua, opener);
        editor.SetCursorPosition(0, static_cast<int>(std::strlen(opener)));
        editor.InsertText("\n");
        EXPECT_EQ(editor.GetText(), std::string(opener) + "\n    ") << opener;
        EXPECT_EQ(cursor(editor), "1:4") << opener;
    }

    // Blocks are counted: a block closed on its own line, a comment or a string opens none
    const char* closed[] = {"if a then return end", "x = 1 -- then", "s = \"do\"", "local function f() end", "end"};
    for (const char* line : closed) {
        Editor editor(SyntaxLanguage::Lua, line);
        editor.SetCursorPosition(0, static_cast<int>(std::strlen(line)));
        editor.InsertText("\n");
        EXPECT_EQ(editor.GetText(), std::string(line) + "\n") << line;
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
    EXPECT_EQ(editor.GetText(), "--[[\n  function f()\n  ");

    Editor css(SyntaxLanguage::Css, "a {");
    css.SetCursorPosition(0, 3);
    css.InsertText("\n");
    EXPECT_EQ(css.GetText(), "a {\n    ");
}

TEST(CustomTextEditorIndent, TypedClosersAreOutdented) {
    EditorInput input;
    Editor editor(SyntaxLanguage::Lua, "if a then\n    foo()\n    ");
    editor.SetCursorPosition(2, 4);
    input.type(editor, "end");
    EXPECT_EQ(editor.GetText(), "if a then\n    foo()\nend");
    EXPECT_EQ(cursor(editor), "2:3");

    // Growing into a longer word gives the indent back
    input.type(editor, "point = 1");
    EXPECT_EQ(editor.GetText(), "if a then\n    foo()\n    endpoint = 1");

    // else stays outdented while it becomes elseif
    Editor elseif(SyntaxLanguage::Lua, "if a then\n    foo()\n    ");
    elseif.SetCursorPosition(2, 4);
    input.type(elseif, "elseif");
    EXPECT_EQ(elseif.GetText(), "if a then\n    foo()\nelseif");

    // A bracket lines up with the line that opened it
    Editor cpp(SyntaxLanguage::Cpp, "  int a[] = {\n      1,\n      ");
    cpp.SetCursorPosition(2, 6);
    input.type(cpp, "}");
    EXPECT_EQ(cpp.GetText(), "  int a[] = {\n      1,\n  }");

    // Inside a block comment typing is text
    Editor comment(SyntaxLanguage::Lua, "--[[\n    ");
    comment.SetCursorPosition(1, 4);
    input.type(comment, "end");
    EXPECT_EQ(comment.GetText(), "--[[\n    end");
}

TEST(CustomTextEditorHighlight, LuaBlockCommentSpansLines) {
    Editor editor(SyntaxLanguage::Lua,
        "--[[ first\n"
        "local x = 1\n"
        "]] local y = 2\n"
        "local z = 3");
    EXPECT_EQ(editor.GetTokenTypeAt(0, 0), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(0, 6), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(1, 10), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(2, 0), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(2, 3), TokenType::Keyword);   // local
    EXPECT_EQ(editor.GetTokenTypeAt(3, 0), TokenType::Keyword);
}

TEST(CustomTextEditorHighlight, LuaLevelledLongBrackets) {
    Editor editor(SyntaxLanguage::Lua,
        "--[==[ a ]] still\n"
        "comment ]==] local\n"
        "s = [=[ text ]]\n"
        "more ]=] local");
    EXPECT_EQ(editor.GetTokenTypeAt(0, 12), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(1, 13), TokenType::Keyword);
    EXPECT_EQ(editor.GetTokenTypeAt(2, 4), TokenType::String);
    EXPECT_EQ(editor.GetTokenTypeAt(3, 0), TokenType::String);
    EXPECT_EQ(editor.GetTokenTypeAt(3, 9), TokenType::Keyword);
}

TEST(CustomTextEditorHighlight, LuaLineCommentsAndStringsEndAtTheLine) {
    Editor editor(SyntaxLanguage::Lua,
        "-- not a block [[\n"
        "local a = 'open\n"
        "local b = \"cont\\\n"
        "inued\" local\n"
        "---[[ still a line comment\n"
        "local c");
    EXPECT_EQ(editor.GetTokenTypeAt(0, 0), TokenType::Comment);
    EXPECT_EQ(editor.GetTokenTypeAt(1, 0), TokenType::Keyword);
    EXPECT_EQ(editor.GetTokenTypeAt(1, 10), TokenType::String);
    // An unclosed quote stops at the line end, a trailing backslash carries it on
    EXPECT_EQ(editor.GetTokenTypeAt(2, 0), TokenType::Keyword);
    EXPECT_EQ(editor.GetTokenTypeAt(3, 0), TokenType::String);
    EXPECT_EQ(editor.GetTokenTypeAt(3, 7), TokenType::Keyword);
    EXPECT_EQ(editor.GetTokenTypeAt(4, 0), TokenType::Comment);
    EXPECT_EQ(editor.GetTokenTypeAt(5, 0), TokenType::Keyword);
}

TEST(CustomTextEditorHighlight, CppBlockCommentsAndRawStrings) {
    Editor editor(SyntaxLanguage::Cpp,
        "/* a\n"
        "b */ int x;\n"
        "auto s = R\"x(one )\"\n"
        "two)x\"; int y;\n"
        "#include \"a.h\"\n"
        "int z;");
    EXPECT_EQ(editor.GetTokenTypeAt(0, 0), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(1, 5), TokenType::Type);
    EXPECT_EQ(editor.GetTokenTypeAt(2, 9), TokenType::String);
    EXPECT_EQ(editor.GetTokenTypeAt(3, 0), TokenType::String);
    EXPECT_EQ(editor.GetTokenTypeAt(3, 8), TokenType::Type);
    EXPECT_EQ(editor.GetTokenTypeAt(4, 0), TokenType::Preprocessor);
    EXPECT_EQ(editor.GetTokenTypeAt(5, 0), TokenType::Type);
}

TEST(CustomTextEditorHighlight, CMakeBracketComments) {
    Editor editor(SyntaxLanguage::CMake,
        "#[[ a\n"
        "set(X 1)\n"
        "]] set(Y 2)\n"
        "# plain\n"
        "set(Z 3)");
    EXPECT_EQ(editor.GetTokenTypeAt(1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetTokenTypeAt(2, 3), TokenType::Keyword);
    EXPECT_EQ(editor.GetTokenTypeAt(3, 0), TokenType::Comment);
    EXPECT_EQ(editor.GetTokenTypeAt(4, 0), TokenType::Keyword);
}

TEST(CustomTextEditorDiff, MarkersFollowEdits) {
    Editor editor(SyntaxLanguage::Lua, "a\nb\nc");
    EXPECT_TRUE(editor.GetDiffChanges().empty());
    editor.SetDiffBase(baseOf({"a", "b", "c"}), "HEAD");
    EXPECT_TRUE(editor.GetDiffChanges().empty());

    // a line typed after a
    editor.SetCursorPosition(0, 1);
    editor.InsertText("\nnew", false);
    std::vector<LineChange> changes = editor.GetDiffChanges();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{1, 1, 1, 2}));
    EXPECT_EQ(editor.GetDiffChangeAt(1), 0);
    EXPECT_EQ(editor.GetDiffChangeAt(0), -1);

    // b edited too: a separate modification
    editor.SetCursorPosition(2, 1);
    editor.InsertText("!", false);
    changes = editor.GetDiffChanges();
    ASSERT_EQ(changes.size(), 1u);   // the added and the changed line touch: one change
    EXPECT_EQ(changes[0].kind(), LineChange::Kind::Modified);

    // c deleted: a deletion marked at the end
    Editor deletion(SyntaxLanguage::Lua, "a\nb");
    deletion.SetDiffBase(baseOf({"a", "b", "c"}), "HEAD");
    changes = deletion.GetDiffChanges();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].kind(), LineChange::Kind::Deleted);
    EXPECT_EQ(deletion.GetDiffChangeAt(1), 0);   // past the end: on the last line

    editor.ClearDiffBase();
    EXPECT_FALSE(editor.HasDiffBase());
    EXPECT_TRUE(editor.GetDiffChanges().empty());
}

TEST(CustomTextEditorDiff, RevertIsOneUndoStep) {
    Editor editor(SyntaxLanguage::Lua, "a\nB\nc\nd");
    editor.SetDiffBase(baseOf({"a", "b", "c", "d"}), "HEAD");
    // typed just before the revert (under the 300 ms merge window)
    editor.SetCursorPosition(3, 1);
    editor.InsertText("x", false);
    ASSERT_EQ(editor.GetText(), "a\nB\nc\ndx");
    ASSERT_EQ(editor.GetDiffChanges().size(), 2u);

    ASSERT_TRUE(editor.RevertDiffChange(editor.GetDiffChangeAt(1)));
    EXPECT_EQ(editor.GetText(), "a\nb\nc\ndx");
    EXPECT_EQ(cursor(editor), "3:2");   // a caret below the change keeps its place
    ASSERT_EQ(editor.GetDiffChanges().size(), 1u);

    // Undo brings the change back and keeps the typing
    editor.Undo();
    EXPECT_EQ(editor.GetText(), "a\nB\nc\ndx");
    editor.Redo();
    EXPECT_EQ(editor.GetText(), "a\nb\nc\ndx");

    // typed right after the revert (under 300 ms): its own step, the revert stays whole
    editor.SetCursorPosition(0, 1);
    editor.InsertText("y", false);
    ASSERT_EQ(editor.GetText(), "ay\nb\nc\ndx");
    editor.Undo();
    EXPECT_EQ(editor.GetText(), "a\nb\nc\ndx");
    editor.Undo();
    EXPECT_EQ(editor.GetText(), "a\nB\nc\ndx");

    // a removed line comes back, a caret inside an added block goes to its first line
    Editor removed(SyntaxLanguage::Lua, "a\nnew\nnewer\nc");
    removed.SetDiffBase(baseOf({"a", "c"}), "HEAD");
    removed.SetCursorPosition(2, 3);
    ASSERT_TRUE(removed.RevertDiffChange(0));
    EXPECT_EQ(removed.GetText(), "a\nc");
    EXPECT_EQ(cursor(removed), "1:0");
    EXPECT_FALSE(removed.RevertDiffChange(0));   // nothing left
}

TEST(CustomTextEditorDiff, CrlfBaseMatches) {
    Editor editor(SyntaxLanguage::Lua, "local a = 1\r\nlocal b = 2\r\n");
    editor.SetDiffBase(baseOf(LineDiff::splitLines("local a = 1\nlocal b = 2\n")), "HEAD");
    EXPECT_TRUE(editor.GetDiffChanges().empty());
}

TEST(CustomTextEditorDiff, PeekTokensCarryBlockComments) {
    // the removed second line sits inside the baseline's block comment
    Editor editor(SyntaxLanguage::Lua, "--[[\n]]\nlocal a");
    editor.SetDiffBase(baseOf({"--[[", "local removed", "]]", "local a"}), "HEAD");
    ASSERT_EQ(editor.GetDiffChanges().size(), 1u);
    EXPECT_EQ(editor.GetDiffBaseTokenTypeAt(1, 0), TokenType::MultiLineComment);
    EXPECT_EQ(editor.GetDiffBaseTokenTypeAt(3, 0), TokenType::Keyword);
}

TEST(CustomTextEditorDiff, KeysAndMarkerClick) {
    EditorInput input;
    Editor editor(SyntaxLanguage::Lua, "a\nB\nc\nd\nE\nf");
    editor.SetDiffBase(baseOf({"a", "b", "c", "d", "e", "f"}), "HEAD");
    editor.SetCursorPosition(0, 0);
    input.frame(editor);

    // Alt+F5 visits the changes in order and wraps, Shift+Alt+F5 goes back
    input.key(editor, ImGuiKey_F5, true, false);
    EXPECT_EQ(cursor(editor), "1:0");
    input.key(editor, ImGuiKey_F5, true, false);
    EXPECT_EQ(cursor(editor), "4:0");
    input.key(editor, ImGuiKey_F5, true, false);
    EXPECT_EQ(cursor(editor), "1:0");
    input.key(editor, ImGuiKey_F5, true, true);
    EXPECT_EQ(cursor(editor), "4:0");

    // a click on the marker of line 2 pins its popup and leaves the caret where it was
    const ImVec2 origin = input.textOrigin();
    const ImVec2 metrics = input.fontMetrics();
    const float charWidth = metrics.x;
    const float lineHeight = std::round(metrics.y * 1.35f);
    const float gutterX = std::round(origin.x + 10.0f + 4 * charWidth + 5.0f);
    input.click(editor, ImVec2(gutterX + 1.0f, origin.y + lineHeight * 1.5f));
    EXPECT_EQ(editor.GetDiffPeekChange(), 0);
    EXPECT_EQ(cursor(editor), "4:0");

    // Esc closes it
    input.key(editor, ImGuiKey_Escape, false, false);
    EXPECT_EQ(editor.GetDiffPeekChange(), -1);
}

namespace {

    bool suggests(SyntaxLanguage language, const std::string& text, const std::string& label) {
        Editor editor(language, text);
        editor.SetAutoComplete(true);
        editor.SetCursorPosition(0, static_cast<int>(text.size()));
        editor.TriggerAutoComplete(true);
        for (const SuggestionItem& item : editor.GetCurrentSuggestions()) {
            if (item.label == label) return true;
        }
        return false;
    }

}

// C++ completion offers what C++ has (Doriax 755972f92): Engine.deltatime is a Lua property backed
// by Engine::getDeltatime(); Engine::deltatime does not compile
TEST(CustomTextEditorCompletion, CppHidesLuaOnlyMembers) {
    EXPECT_FALSE(suggests(SyntaxLanguage::Cpp, "jasperengine::Engine::delta", "deltatime"));
    EXPECT_TRUE(suggests(SyntaxLanguage::Cpp, "jasperengine::Engine::getDelta", "getDeltatime"));
    EXPECT_TRUE(suggests(SyntaxLanguage::Lua, "Engine.delta", "deltatime"));
}
