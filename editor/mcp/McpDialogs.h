// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpCall.h"
#include "McpJson.h"
#include "window/EditorModal.h"

namespace jasperengine::editor::mcp {

    // What editor_status and respond_dialog say of a dialog
    Json dialogJson(const EditorDialogInfo& info);
    // "The editor is showing the dialog ...": how to answer it, for a call it refuses
    std::string dialogBlockMessage(const EditorDialogInfo& info);

    // respond_dialog
    void respondDialogTool(const CallPtr& call);
    // respond_dialog on a file picker, from any thread: the picker returns to the code that opened it
    // once this answers true. Only checks and takes its path: the picker's caller runs after.
    bool answerFilePicker(const Json& args, std::string& error);
    // Main thread, the answer's frames run: replies with what the editor shows now (a validation
    // error of the dialog that was answered: an error reply)
    void reportDialogAnswer(const CallPtr& call, const std::string& title, const std::string& choice);

}
