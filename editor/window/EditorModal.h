// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // A value of a dialog's field, as respond_dialog gives it and editor_status reports it
    struct EditorDialogValue {
        enum class Kind {
            None,
            Text,
            Bool,
            Number
        };
        Kind kind = Kind::None;
        std::string text;
        bool flag = false;
        double number = 0.0;

        static EditorDialogValue ofText(std::string value);
        static EditorDialogValue ofBool(bool value);
        static EditorDialogValue ofNumber(double value);
    };

    struct EditorDialogField {
        std::string name;                   // its key in respond_dialog's values
        std::string type;                   // text, path, bool, int, float or enum
        EditorDialogValue value;
        std::vector<std::string> options;   // enum: the values it takes
    };

    // A dialog waiting for an answer, as MCP reports it to agents (App::getOpenDialog)
    struct EditorDialogInfo {
        std::string kind;                  // "alert", "save_dialog", "modal" or "file_picker"
        std::string title;
        std::string message;
        std::vector<std::string> choices;  // answers App::answerDialog takes; empty: a human must close it
        // choices of greyed-out buttons: new values may enable them
        std::vector<std::string> disabledChoices;
        std::vector<EditorDialogField> fields;
        std::string error;                 // why it does not take its current values (EditorModal::error)
        // Edits are refused while it is open, except under a modal window built to keep them
        // (Project Settings, EditorModal::Role::MergesEdits)
        bool blocksEdits = true;
    };

    // Every editor modal popup is drawn through EditorModal, so MCP sees and answers it as a human
    // would (S69): editor_status reports the top-most one (App::getOpenDialog), respond_dialog presses
    // its buttons and fills its fields, and edits are refused while it is open. MCP calls run
    // between frames (after ImGui::NewFrame, before the windows are drawn), when no window is
    // active, so they see the modal of the last complete frame (endFrame). tools/check-editor-modals.py
    // fails on a modal popup begun anywhere else. Main thread only.
    class EditorModal {
    public:
        enum class Role {
            Dialog,         // a question, a name prompt, an error box, a modal window: refuses agent edits
            MergesEdits,    // a modal window whose OK keeps concurrent agent edits (Project Settings)
            Progress        // closes by itself, nothing to answer (Loading): not a dialog
        };

        // ImGui's modal popup, registering the modal as drawn this frame. With p_open, an agent's
        // "cancel" on a modal that has no Cancel button presses the title bar's close button.
        static bool begin(const char* name, bool* p_open = nullptr, ImGuiWindowFlags flags = 0, Role role = Role::Dialog);
        // What the modal says, for agents (between begin and EndPopup)
        static void message(const std::string& text);
        // Why the modal refuses its current values, when it says so in its body rather than in a nested
        // error box; respond_dialog's reply carries it. Empty: no problem.
        static void error(const std::string& text);
        // ImGui::Button that answers the current modal with `choice` (ok, yes, no, cancel, or a word
        // of its own for any other button): true when clicked, or when an agent answered that choice.
        // A greyed-out button is listed as a disabled choice and never pressed.
        static bool button(const char* label, const char* choice, const ImVec2& size = ImVec2(0, 0));
        // The choice of a button whose label comes from data: "Keep Mine" -> keep_mine
        static std::string choiceOf(const std::string& label);

        // Fields: report a variable of the current modal as `name` and write an agent's value into it
        // (true when written). Call right before the widget that edits the variable, which then
        // draws the new value; widgets that need nothing else use the wrappers below.
        static bool field(const char* name, std::string& value);
        static bool field(const char* name, char* buffer, size_t size);
        static bool field(const char* name, bool& value);
        static bool field(const char* name, int& value);
        static bool field(const char* name, float& value);
        // a path (values.path of a picker's answer takes the same word)
        static bool pathField(const char* name, std::string& value);
        static bool pathField(const char* name, char* buffer, size_t size);
        // one of options, by its index or by its text
        static bool enumField(const char* name, int& index, const std::vector<std::string>& options);
        static bool enumField(const char* name, std::string& value, const std::vector<std::string>& options);

        // ImGui's widgets with their field. With ImGuiInputTextFlags_EnterReturnsTrue inputText answers
        // what ImGui does, Enter only (its callers confirm on true); otherwise an agent's value
        // counts as an edit.
        static bool inputText(const char* label, const char* name, char* buffer, size_t size, ImGuiInputTextFlags flags = 0,
                              ImGuiInputTextCallback callback = nullptr, void* userData = nullptr);
        static bool inputTextWithHint(const char* label, const char* name, const char* hint, char* buffer, size_t size, ImGuiInputTextFlags flags = 0);
        static bool checkbox(const char* label, const char* name, bool* value);
        static bool combo(const char* label, const char* name, int* index, const std::vector<std::string>& options);
        static bool inputInt(const char* label, const char* name, int* value, int step = 1, int stepFast = 100);
        static bool inputFloat(const char* label, const char* name, float* value, float step = 0.0f, float stepFast = 0.0f, const char* format = "%.3f");
        static bool dragInt(const char* label, const char* name, int* value, float speed, int min, int max);

        // EditorFrame, once the windows are drawn and before ImGui::Render (windows still active):
        // keeps the top-most modal for MCP and drops an answer no frame took
        static void endFrame();
        // A popup was just asked for and no frame has drawn it yet, or an answer waits for its frame:
        // the frame loop must not idle now (a headless editor gets no other reason to draw).
        static bool wantsFrame();
        // Only the first: a modal opened this frame is reported by the next one
        static bool settling();

        // MCP: the top-most modal of the last frame; false when none (or only a progress one)
        static bool topModal(EditorDialogInfo& out);
        // MCP: answers it: values go into its fields the next time it is drawn, then the button
        // of `choice` (empty: none, the fields only) is pressed, or the close button; false, and why,
        // when they cannot apply
        static bool answer(const std::string& choice, const std::map<std::string, EditorDialogValue>& values, std::string& error);
        // MCP: the answer has been taken (or dropped): its frames have run
        static bool answerSettled();
        // MCP: why the last answer was not taken ("OK is greyed out: ..."), once
        static std::string takeAnswerError();
        // MCP: a modal of this title is open in the last frame (under the top-most one too)
        static bool isOpen(const std::string& title);

        // Tests: forgets the frames so far
        static void reset();
    };

}
