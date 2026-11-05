// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpDialogs.h"

#include "McpCall.h"
#include "McpContext.h"

#include "App.h"
#include "window/EditorFilePicker.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

    Json valueJson(const EditorDialogField& field) {
        switch (field.value.kind) {
            case EditorDialogValue::Kind::Text: return field.value.text;
            case EditorDialogValue::Kind::Bool: return field.value.flag;
            case EditorDialogValue::Kind::Number:
                if (field.type == "int") return static_cast<long long>(field.value.number);
                return field.value.number;
            case EditorDialogValue::Kind::None: break;
        }
        return nullptr;
    }

    // respond_dialog's values for EditorModal; false when one is not a field's kind of value at all
    bool readValues(const Json& args, std::map<std::string, EditorDialogValue>& out, std::string& error) {
        if (!args.contains("values")) return true;
        if (!args["values"].is_object()) {
            error = "values must be an object: {field name: value} (editor_status lists a dialog's fields).";
            return false;
        }
        for (const auto& [name, value] : args["values"].items()) {
            if (value.is_string()) out[name] = EditorDialogValue::ofText(value.get<std::string>());
            else if (value.is_boolean()) out[name] = EditorDialogValue::ofBool(value.get<bool>());
            else if (value.is_number()) out[name] = EditorDialogValue::ofNumber(value.get<double>());
            else {
                error = "values." + name + " must be a string, a number or true/false.";
                return false;
            }
        }
        return true;
    }

}

Json jasperengine::editor::mcp::dialogJson(const EditorDialogInfo& info) {
    Json dialog = {{"kind", info.kind}, {"title", info.title}, {"message", info.message}, {"choices", info.choices}};
    if (!info.disabledChoices.empty()) dialog["disabled_choices"] = info.disabledChoices;
    if (!info.fields.empty()) {
        Json fields = Json::array();
        for (const EditorDialogField& field : info.fields) {
            Json item = {{"name", field.name}, {"type", field.type}, {"value", valueJson(field)}};
            if (!field.options.empty()) item["options"] = field.options;
            fields.push_back(std::move(item));
        }
        dialog["fields"] = std::move(fields);
    }
    if (!info.error.empty()) dialog["error"] = info.error;
    // a modal window that keeps concurrent edits (Project Settings): edits are not refused meanwhile
    if (!info.blocksEdits) dialog["blocks_edits"] = false;
    return dialog;
}

std::string jasperengine::editor::mcp::dialogBlockMessage(const EditorDialogInfo& info) {
    std::string text = "The editor is showing the dialog \"" + info.title + "\"";
    if (!info.message.empty()) text += " (" + info.message + ")";
    if (!info.choices.empty() || !info.disabledChoices.empty() || !info.fields.empty()) {
        text += ". Answer it with respond_dialog";
        if (!info.choices.empty()) {
            text += ", choice one of:";
            for (const std::string& choice : info.choices) text += " " + choice;
        }
        if (!info.fields.empty()) {
            text += ", values for its fields:";
            for (const EditorDialogField& field : info.fields) text += " " + field.name;
        }
    } else {
        text += ", which only a human can close in the editor window";
    }
    return text + ".";
}

bool jasperengine::editor::mcp::answerFilePicker(const Json& args, std::string& error) {
    const std::string choice = args.value("choice", "");
    if (choice == "cancel") return EditorFilePicker::cancel(error);
    if (!choice.empty() && choice != "ok") {
        error = "Choice must be one of: ok cancel.";
        return false;
    }
    std::vector<std::string> paths;
    if (args.contains("values")) {
        if (!args["values"].is_object()) {
            error = "values must be an object: {\"path\": ...}.";
            return false;
        }
        for (const auto& [name, value] : args["values"].items()) {
            if (name == "path" && value.is_string()) {
                paths.push_back(value.get<std::string>());
            } else if (name == "paths" && value.is_array()) {
                for (const Json& item : value) {
                    if (!item.is_string()) {
                        error = "values.paths must be strings.";
                        return false;
                    }
                    paths.push_back(item.get<std::string>());
                }
            } else {
                error = "A file picker takes values.path (a string) or values.paths (an array), not " + name + ".";
                return false;
            }
        }
    }
    return EditorFilePicker::answer(paths, error);
}

void jasperengine::editor::mcp::reportDialogAnswer(const CallPtr& call, const std::string& title, const std::string& choice) {
    App& app = call->ctx().app();
    std::string problem = EditorModal::takeAnswerError();
    Json data = {{"answered", {{"title", title}, {"choice", choice}}}};
    std::string text = "Answered \"" + title + "\"" + (choice.empty() ? std::string() : " with " + choice) + ".";
    EditorDialogInfo open;
    if (app.getOpenDialog(open)) {
        data["dialog"] = dialogJson(open);
        text += " Now open: \"" + open.title + "\": " + open.message;
        if (!open.error.empty()) text += " It says: " + open.error;
        // it kept its values and says why, or an error box of its own came up on top of it
        const bool sameDialog = open.title == title;
        if (problem.empty() && !choice.empty() && sameDialog && !open.error.empty()) {
            problem = "\"" + title + "\" did not take it: " + open.error + " Change its values and answer again, or choice cancel.";
        } else if (problem.empty() && !choice.empty() && !sameDialog && open.kind == "modal" && EditorModal::isOpen(title) &&
                   open.choices == std::vector<std::string>{"ok"}) {
            problem = "\"" + title + "\" did not take it: " + open.title + (open.message.empty() ? std::string() : ": " + open.message) +
                      " Answer this box with choice ok, then answer \"" + title + "\" again with other values.";
        }
    }
    if (!problem.empty()) {
        call->fail(problem, data);
        return;
    }
    call->ok(text, data);
}

void jasperengine::editor::mcp::respondDialogTool(const CallPtr& call) {
    const Json& args = call->args();
    App& app = call->ctx().app();
    const std::string choice = args.value("choice", "");
    std::string error;
    EditorDialogInfo dialog;
    if (!app.getOpenDialog(dialog)) {
        call->ok("No dialog is open.");
        return;
    }
    if (dialog.kind == "file_picker") {
        if (!answerFilePicker(args, error)) {
            call->fail(error, Json{{"dialog", dialogJson(dialog)}});
            return;
        }
        const std::string title = dialog.title;
        call->afterFrames(2, [call, title, choice]() { reportDialogAnswer(call, title, choice); });
        return;
    }
    std::map<std::string, EditorDialogValue> values;
    if (!readValues(args, values, error)) {
        call->fail(error, Json{{"dialog", dialogJson(dialog)}});
        return;
    }
    if (choice.empty() && values.empty()) {
        call->fail("Give a choice, or values for the dialog's fields.", Json{{"dialog", dialogJson(dialog)}});
        return;
    }
    if (!app.answerDialog(choice, values, error)) {
        call->fail(error, Json{{"dialog", dialogJson(dialog)}});
        return;
    }
    // The answer is pressed in the next frames; report what the editor shows after it. The press
    // may open a file picker, which waits on the main thread: the reply cannot wait for frames then.
    const std::string title = dialog.title;
    auto report = [call, title, choice]() {
        if (!call->completed()) reportDialogAnswer(call, title, choice);
    };
    call->setOnFilePicker(report);
    call->waitUntil([]() { return EditorModal::answerSettled(); }, 30000, [call, report](bool) {
        call->afterFrames(2, report);
    });
}
