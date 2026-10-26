// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Scripting & extensibility tools (S23 R44): reload_scripts, get_script_status /
// set_script_settings, the project's editor menus (list_editor_menus, run_editor_menu) and
// eval_expression.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "AppSettings.h"
#include "Project.h"
#include "util/EditorScripts.h"
#include "util/ExternalCodeEditor.h"
#include "util/NumericExpression.h"
#include "util/ScriptReloadService.h"
#include "util/ScriptStatus.h"

#include <cmath>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ reload_scripts

void reloadScriptsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string language = args.value("language", std::string("all"));
    if (language != "lua" && language != "cpp" && language != "all") {
        call->fail("language is lua, cpp or all.");
        return;
    }
    const bool lua = language != "cpp";
    const bool cpp = language != "lua";
    const bool wait = args.value("wait", true);
    const int timeoutMs = static_cast<int>(std::lround(args.value("timeout_seconds", 300.0) * 1000.0));

    std::vector<fs::path> paths;
    if (args.contains("paths") && args["paths"].is_array()) {
        for (const Json& item : args["paths"]) {
            if (!item.is_string()) continue;
            fs::path absolute;
            std::string error;
            if (!McpRefs::resolveProjectPath(project, item.get<std::string>(), absolute, error, true)) {
                call->fail(error);
                return;
            }
            paths.push_back(absolute.lexically_normal());
        }
    }

    ScriptReloadService& reloads = ScriptReloadService::get();
    const uint64_t serial = reloads.lastSerial();
    Json notes = Json::array();
    bool cppStarted = false;
    if (project->isAnyScenePlaying()) {
        if (cpp && !ScriptReloadService::cppReloadEnabled()) {
            if (language == "cpp") {
                call->fail("C++ reload during Play is off: turn on Preferences > Scripting > Reload C++ Scripts During Play "
                           "(set_script_settings reload_cpp_during_play: true), or stop Play.");
                return;
            }
            notes.push_back("C++ scripts are not reloaded during Play (Preferences > Scripting > Reload C++ Scripts During Play is off).");
        }
        const bool cppNow = cpp && ScriptReloadService::cppReloadEnabled() && project->hasCppPlayLibrary();
        reloads.requestReload(paths, lua, cppNow);
        cppStarted = cppNow;
    } else {
        if (lua) notes.push_back("No scene is playing: Lua scripts load as they are at the next Play.");
        if (cpp) {
            std::string message;
            cppStarted = reloads.rebuildCpp(message);
            if (!cppStarted) notes.push_back("C++: " + message);
        }
    }

    auto finish = [call, serial, notes, cpp](bool timedOut) {
        Json report = ScriptReloadService::reportJson(ScriptReloadService::get().recordsSince(serial));
        if (cpp) {
            const Json status = ScriptStatus::get().toJson();
            report["cpp"] = {{"state", status["cpp"]["state"]}, {"fingerprint", status["fingerprint"]}};
            if (!status["cpp"]["findings"].empty()) report["cpp"]["findings"] = status["cpp"]["findings"];
        }
        if (!notes.empty()) report["notes"] = notes;
        if (timedOut) report["timed_out"] = true;
        std::string text = "Reloaded " + std::to_string(report["reloaded"].size()) + ", kept the old code of " +
                           std::to_string(report["kept_old"].size()) + ".";
        for (const Json& kept : report["kept_old"]) {
            for (const Json& finding : kept["findings"]) {
                text += "\n" + finding.value("file", std::string()) +
                        (finding.contains("line") ? ":" + std::to_string(finding["line"].get<int>()) : std::string()) + ": " +
                        finding.value("message", std::string());
            }
        }
        for (const Json& note : notes) text += "\n" + note.get<std::string>();
        call->ok(text, report);
    };
    if (!wait) {
        finish(false);
        return;
    }
    call->waitUntil([paths, cppStarted]() {
        ScriptReloadService& service = ScriptReloadService::get();
        if (cppStarted && service.isCppBuilding()) return false;
        return paths.empty() ? !service.busy() : service.idleFor(paths);
    }, timeoutMs, finish);
}

// ------------------------------------------------------------------ get_script_status, set_script_settings

Json scriptSettingsJson() {
    return {{"changes_while_playing", AppSettings::getScriptChangesWhilePlaying()},
            {"reload_cpp_during_play", AppSettings::getReloadCppDuringPlay()},
            {"external_script_editor", AppSettings::getExternalScriptEditor()},
            {"external_script_editor_command", AppSettings::getExternalScriptEditorCommand()}};
}

void getScriptStatusTool(const CallPtr& call) {
    Json status = ScriptStatus::get().toJson();
    status["settings"] = scriptSettingsJson();
    call->ok(status["summary"].get<std::string>(), status);
}

void setScriptSettingsTool(const CallPtr& call) {
    const Json& args = call->args();
    // the Preferences > Scripting settings, when given
    bool changed = false;
    if (args.contains("changes_while_playing") && args["changes_while_playing"].is_string()) {
        const std::string policy = args["changes_while_playing"].get<std::string>();
        if (policy != "reload" && policy != "after_stop" && policy != "stop_and_reload") {
            call->fail("changes_while_playing is reload, after_stop or stop_and_reload.");
            return;
        }
        AppSettings::setScriptChangesWhilePlaying(policy);
        changed = true;
    }
    if (args.contains("reload_cpp_during_play") && args["reload_cpp_during_play"].is_boolean()) {
        AppSettings::setReloadCppDuringPlay(args["reload_cpp_during_play"].get<bool>());
        changed = true;
    }
    if (args.contains("external_script_editor") && args["external_script_editor"].is_string()) {
        const std::string id = args["external_script_editor"].get<std::string>();
        bool known = false;
        std::string ids;
        for (const ExternalCodeEditor::Choice& choice : ExternalCodeEditor::choices()) {
            known = known || id == choice.id;
            ids += (ids.empty() ? "" : ", ") + std::string(choice.id);
        }
        if (!known) {
            call->fail("external_script_editor is one of " + ids + ".");
            return;
        }
        AppSettings::setExternalScriptEditor(id);
        changed = true;
    }
    if (args.contains("external_script_editor_command") && args["external_script_editor_command"].is_string()) {
        AppSettings::setExternalScriptEditorCommand(args["external_script_editor_command"].get<std::string>());
        changed = true;
    }
    if (changed) AppSettings::saveSettings();
    const Json settings = scriptSettingsJson();
    call->ok("Scripting settings: " + settings.dump(), Json{{"settings", settings}});
}

// ------------------------------------------------------------------ editor menus

Json itemJson(const EditorMenuItem& item) {
    Json out = {{"id", item.id}, {"path", item.path}, {"kind", item.kind}, {"language", item.language},
                {"priority", item.priority}, {"targets", item.targets()},
                {"source", {{"file", item.file}, {"line", item.line}}}, {"function", item.function}};
    if (!item.shortcut.empty()) out["shortcut"] = item.shortcut;
    if (!item.icon.empty()) out["icon"] = item.icon;
    if (!item.tooltip.empty()) out["tooltip"] = item.tooltip;
    if (!item.validateFunction.empty()) {
        out["validate"] = item.validateFunction;
        out["enabled"] = EditorScripts::get().isEnabled(item);
    }
    return out;
}

void listEditorMenusTool(const CallPtr& call) {
    const std::string target = call->args().value("target", std::string());
    Json items = Json::array();
    if (target.empty()) {
        for (const EditorMenuItem& item : EditorScripts::get().items()) items.push_back(itemJson(item));
    } else {
        for (const EditorMenuItem* item : EditorScripts::get().itemsFor(target)) items.push_back(itemJson(*item));
    }
    call->ok(std::to_string(items.size()) + " editor script item(s).", Json{{"items", items}});
}

void runEditorMenuTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string key = args.value("id", args.value("path", std::string()));
    const EditorMenuItem* found = key.empty() ? nullptr : EditorScripts::get().find(key);
    if (!found) {
        call->fail(key.empty() ? "id or path is required (list_editor_menus lists them)."
                               : "No editor script item " + key + " (list_editor_menus lists them).");
        return;
    }
    const EditorMenuItem item = *found;

    EditorMenuRunContext context;
    context.menuPath = item.path;
    std::string error;
    SceneProject* scene = args.contains("scene") ? McpRefs::resolveScene(project, args["scene"], error) : project->getSelectedScene();
    if (!scene) {
        call->fail(error.empty() ? "No scene is open." : error);
        return;
    }
    context.sceneId = scene->id;
    if (args.contains("selection")) {
        std::vector<Entity> selection;
        if (!McpRefs::resolveEntities(scene, args["selection"], selection, error)) {
            call->fail(error);
            return;
        }
        for (Entity entity : selection) context.selection.push_back(entity);
    } else {
        for (Entity entity : project->getSelectedEntities(scene->id)) context.selection.push_back(entity);
    }
    if (args.contains("assets") && args["assets"].is_array()) {
        for (const Json& path : args["assets"]) {
            if (path.is_string()) context.assets.push_back(path.get<std::string>());
        }
    }
    context.component = args.value("component", item.contextComponent());

    call->detach();
    EditorScripts::get().run(item, context, call, [call, item](EditorMenuRunResult result) {
        Json data = {{"ok", result.ok}, {"id", item.id}, {"undo_name", result.undoName}, {"log", result.log}};
        if (!result.error.empty()) data["error"] = result.error;
        if (!result.undo.is_null()) data["undo_group"] = result.undo;
        std::string text = (result.ok ? "Ran " : "Failed: ") + item.path +
                           (result.undoName.empty() ? std::string(" (no edit)") : " (undo step \"" + result.undoName + "\")");
        if (!result.error.empty()) text += "\n" + result.error;
        for (const std::string& line : result.log) text += "\n" + line;
        if (result.ok) call->ok(text, data);
        else call->fail(text, data);
    });
}

// ------------------------------------------------------------------ eval_expression

void evalExpressionTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string expression = args.value("expression", std::string());
    NumericContext context;
    if (args.contains("unit") && args["unit"].is_string() && !NumericExpression::parseUnit(args["unit"].get<std::string>(), context.unit)) {
        call->fail("unit is none, meters, degrees, seconds or pixels.");
        return;
    }
    context.integer = args.value("integer", false);
    context.objectIndex = args.value("object_index", 0);
    context.objectCount = std::max(1, args.value("object_count", 1));
    context.seed = args.value("seed", 0u);
    const NumericResult result = NumericExpression::evaluate(expression, args.value("current", 0.0), context);
    if (!result.ok) {
        // an expression that does not evaluate is an answer, not a tool error
        call->ok(result.error, Json{{"error", result.error}, {"position", result.position}});
        return;
    }
    Json data = {{"value", result.value}};
    if (result.perObject) data["per_object"] = true;
    char text[64];
    std::snprintf(text, sizeof(text), "%.17g", result.value);
    call->ok(text, data);
}

}

void registerScriptingTools(McpRegistry& registry) {
    using namespace schema;

    {
        ToolDef tool;
        tool.name = "reload_scripts";
        tool.activity = ToolActivity::Control;
        tool.title = "Reload Scripts";
        tool.description =
            "While playing: swap the new code of changed Lua scripts (and C++ when its reload is on) into the running "
            "game, fields kept, init not re-run, onReload(info) called; a script with an error keeps its old code. "
            "Edit mode: lua is a no-op, cpp builds. Returns reloaded [{path, instances, properties, dropped_callbacks}] "
            "and kept_old [{path, findings}].";
        tool.inputSchema = object({
            {"paths", array(string(""), "Project-relative scripts; default: every script the game runs.")},
            {"language", withDefault(stringEnum("", {"lua", "cpp", "all"}), "all")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 1, 3600), 300)}
        });
        tool.allowedWhilePlaying = true;
        tool.handler = reloadScriptsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_script_status";
        tool.title = "Get Script Status";
        tool.description =
            "The toolbar badge's state: lua (ok, reloading, error), cpp (none, stale, building + fraction, up_to_date, "
            "failed + using_last_good_build, refused), editor_cpp, findings [{file, line, message}], the last C++ "
            "library's toolchain fingerprint {ok, mismatches [{field, editor, library, severity}]}, and settings.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getScriptStatusTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_script_settings";
        tool.title = "Set Script Settings";
        tool.description =
            "Preferences > Scripting: changes_while_playing, reload_cpp_during_play (off by default); External Tools > "
            "External Script Editor (custom: a command with {file} {line} {column} {project}). Returns settings.";
        tool.inputSchema = object({
            {"changes_while_playing", stringEnum("Script Changes While Playing.", {"reload", "after_stop", "stop_and_reload"})},
            {"reload_cpp_during_play", boolean("Reload C++ Scripts During Play.")},
            {"external_script_editor", string("builtin, vscode, visualstudio, rider, clion, xcode, sublime, zed, system, custom.")},
            {"external_script_editor_command", string("custom's command.")}
        });
        tool.activity = ToolActivity::Control;
        tool.handler = setScriptSettingsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "list_editor_menus";
        tool.title = "List Editor Menus";
        tool.description =
            "Menu items, context-menu items and toolbar buttons the project's editor scripts declare (Lua ---@menu / "
            "---@toolbar in Editor/ folders, C++ JE_MENU_ITEM): [{id, path, kind, priority, shortcut, source{file,line}, "
            "enabled?}]. Found by reading the files; nothing runs.";
        tool.inputSchema = object({
            {"target", stringEnum("Only those a menu shows.", {"main", "gameobject", "assets", "component", "sceneview", "toolbar"})}
        });
        tool.annotations.readOnly = true;
        tool.handler = listEditorMenusTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "run_editor_menu";
        tool.title = "Run Editor Menu";
        tool.description =
            "Run an editor script's item as a click would: its edits (Editor.call of tools) become one undo step named "
            "after it. selection/scene default to the editor's. Returns {ok, undo_name, log}.";
        tool.inputSchema = object({
            {"id", string("From list_editor_menus.")},
            {"path", string("Or the item's path, \"Tools/Snap To Grid\".")},
            {"scene", sceneRef()},
            {"selection", entities()},
            {"assets", array(string(""), "Project-relative paths (Assets/... items).")},
            {"component", string("CONTEXT/<Component>/... items.")}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = runEditorMenuTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "eval_expression";
        tool.title = "Eval Expression";
        tool.description =
            "What a numeric field makes of typed text: math (2^3^2, sqrt, min, clamp, lerp, pi), units on fields that have "
            "one (25cm, 90deg, 250ms, 12px), +=/-=/*=/= on current, L(a,b)/R(a,b) over object_count. {value} or "
            "{error, position}.";
        tool.inputSchema = object({
            {"expression", string("")},
            {"current", withDefault(number("The field's value (relative ops)."), 0)},
            {"unit", withDefault(stringEnum("", {"none", "meters", "degrees", "seconds", "pixels"}), "none")},
            {"integer", withDefault(boolean("An integer field: rounds half away from zero."), false)},
            {"object_index", withDefault(integer(""), 0)},
            {"object_count", withDefault(integer(""), 1)},
            {"seed", withDefault(integer("R()"), 0)}
        }, {"expression"});
        tool.annotations.readOnly = true;
        tool.handler = evalExpressionTool;
        registry.addTool(std::move(tool));
    }
}

}
