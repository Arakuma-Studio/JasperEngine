// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Public text editing operations and keyboard input preserve block indentation.
// Keyboard checks use ImGui input and the real renderer without a display or GPU.

#include <gtest/gtest.h>

#include "window/widget/CustomTextEditor.h"
#include "window/widget/SemanticSuggestions.h"

#include <cstring>
#include <string>

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
            editor.SetAutoComplete(false);
            editor.RequestFocus();
            ImGui::GetIO().AddInputCharactersUTF8(text);
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImVec2(800, 600));
            ImGui::Begin("Code editor");
            editor.Render("Text");
            ImGui::End();
            ImGui::EndFrame();
        }
    };

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
