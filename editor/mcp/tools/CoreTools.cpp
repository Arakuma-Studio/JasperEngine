// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Core: batch, get_job, cancel_job, undo / redo, editor_status, respond_dialog
#include "McpTools.h"

#include "mcp/McpAutoSync.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"

#include "App.h"
#include "EditorVersion.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "util/FileUtils.h"
#include "window/CodeEditor.h"
#include "window/ProjectWindow.h"

#include <atomic>
#include <map>
#include <memory>

namespace jasperengine::editor::mcp {

namespace {

constexpr size_t MAX_BATCH_CALLS = 200;

// ------------------------------------------------------------------ batch

struct BatchState {
    CallPtr call;
    Json calls;
    bool stopOnError = true;
    bool undoGroup = false;
    size_t next = 0;
    Json results = Json::array();
    size_t succeeded = 0;
    size_t failed = 0;
    bool stopped = false;
    std::vector<ToolImage> images;
    // undo_group: each history's position when the batch started
    std::map<uint32_t, size_t> sceneMarks;
    size_t projectMark = 0;
};

void beginUndoGroup(BatchState& state, Project* project) {
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        state.sceneMarks[scene->id] = CommandHandle::get(scene->id)->mark();
    }
    state.projectMark = project->getProjectCommandHistory()->mark();
}

// One undo step per history the batch changed. Scenes the batch created start at 0.
Json endUndoGroup(BatchState& state, Project* project) {
    Json grouped = Json::array();
    bool complete = true;
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        auto it = state.sceneMarks.find(scene->id);
        const size_t mark = it != state.sceneMarks.end() ? it->second : 0;
        CommandHistory* history = CommandHandle::get(scene->id);
        if (history->position() <= mark) continue;
        if (history->groupSince(mark)) grouped.push_back(scene->id);
        else complete = false;
    }
    if (!project->getProjectCommandHistory()->groupSince(state.projectMark)) complete = false;
    return Json{{"scene_ids", grouped}, {"complete", complete}};
}

void finishBatch(const std::shared_ptr<BatchState>& state) {
    Project* project = state->call->ctx().project();
    Json data = {
        {"results", state->results},
        {"succeeded", state->succeeded},
        {"failed", state->failed},
        {"stopped_early", state->stopped}
    };
    if (state->undoGroup) {
        data["undo_group"] = endUndoGroup(*state, project);
    }

    std::string text = "batch: " + std::to_string(state->succeeded) + " ok, " + std::to_string(state->failed) + " failed";
    if (state->stopped) text += ", stopped at the first error";
    text += ".";
    for (const Json& item : state->results) {
        text += "\n" + std::to_string(item["index"].get<size_t>()) + ". " + item.value("tool", std::string("?")) +
                (item.value("ok", false) ? ": " : ": ERROR: ") + item.value("text", std::string());
    }

    // The batch itself ran: failures are in results[i].ok (and the text)
    ToolResult result = ToolResult::success(text, data);
    result.images = std::move(state->images);
    state->call->reply(std::move(result));
}

void runBatchStep(const std::shared_ptr<BatchState>& state);

void recordBatchResult(const std::shared_ptr<BatchState>& state, size_t index, const std::string& tool, const ToolResult& result) {
    Json item = {{"index", index}, {"tool", tool}, {"ok", !result.isError}, {"text", result.text}};
    if (!result.structured.is_null()) item["structured"] = result.structured;
    state->results.push_back(item);
    for (const ToolImage& image : result.images) state->images.push_back(image);
    if (result.isError) {
        state->failed++;
        if (state->stopOnError) state->stopped = true;
    } else {
        state->succeeded++;
    }
}

void runBatchStep(const std::shared_ptr<BatchState>& state) {
    McpServer& server = state->call->server();
    while (true) {
        if (state->stopped || state->next >= state->calls.size() || state->call->cancelled()) {
            if (state->call->cancelled() && state->next < state->calls.size()) state->stopped = true;
            finishBatch(state);
            return;
        }
        const size_t index = state->next++;
        const Json& item = state->calls[index];
        const std::string tool = item.is_object() ? item.value("tool", std::string()) : std::string();
        Json args = item.is_object() && item.contains("arguments") ? item["arguments"] : Json::object();
        if (args.is_null()) args = Json::object();

        if (tool.empty() || !args.is_object()) {
            recordBatchResult(state, index, tool, ToolResult::error("Each call needs {\"tool\": name, \"arguments\": {...}}."));
            continue;
        }
        if (tool == "batch") {
            recordBatchResult(state, index, tool, ToolResult::error("batch can't be nested."));
            continue;
        }

        // 0 = running, 1 = completed inside runNested, 2 = runNested returned first
        auto phase = std::make_shared<std::atomic<int>>(0);
        server.runNested(state->call, tool, args, [state, index, tool, phase](const ToolResult& result) {
            recordBatchResult(state, index, tool, result);
            int expected = 0;
            if (phase->compare_exchange_strong(expected, 1)) return;
            // Completed later (a deferred call): continue on the main thread
            App& app = state->call->ctx().app();
            if (app.isMainThread()) runBatchStep(state);
            else app.enqueueMainThreadTask([state]() { runBatchStep(state); });
        });
        int expected = 0;
        if (phase->compare_exchange_strong(expected, 2)) {
            // The nested call deferred; its completion resumes the batch
            return;
        }
    }
}

void batchTool(const CallPtr& call) {
    const Json& args = call->args();
    if (!args.contains("calls") || !args["calls"].is_array() || args["calls"].empty()) {
        call->fail("calls must be a non-empty array of {\"tool\": name, \"arguments\": {...}}.");
        return;
    }
    if (args["calls"].size() > MAX_BATCH_CALLS) {
        call->fail("A batch takes at most " + std::to_string(MAX_BATCH_CALLS) + " calls.");
        return;
    }
    auto state = std::make_shared<BatchState>();
    state->call = call;
    state->calls = args["calls"];
    state->stopOnError = args.value("stop_on_error", true);
    state->undoGroup = args.value("undo_group", false);
    if (state->undoGroup) beginUndoGroup(*state, call->ctx().project());
    call->detach();
    runBatchStep(state);
}

// ------------------------------------------------------------------ jobs

void getJobTool(const CallPtr& call) {
    McpJobs& jobs = call->ctx().jobs();
    const std::string id = call->args().value("job_id", "");
    if (id.empty()) {
        Json list = Json::array();
        for (const JobPtr& job : jobs.list()) list.push_back(job->toJson(5));
        call->ok(std::to_string(list.size()) + " job(s).", Json{{"jobs", list}});
        return;
    }
    JobPtr job = jobs.find(id);
    if (!job) {
        std::string known;
        for (const JobPtr& other : jobs.list()) known += " " + other->id();
        call->fail("No job " + id + "." + (known.empty() ? " There are no jobs." : " Jobs:" + known + "."));
        return;
    }
    const int logLines = call->args().value("log_lines", 40);
    Json data = job->toJson(logLines);
    call->ok(job->title() + ": " + jobStatusName(job->status()) +
             (job->message().empty() ? std::string() : " - " + job->message()), data);
}

void cancelJobTool(const CallPtr& call) {
    const std::string id = call->args().value("job_id", "");
    JobPtr job = call->ctx().jobs().find(id);
    if (!job) {
        call->fail("No job " + id + ". Call get_job without job_id to list jobs.");
        return;
    }
    if (job->finished()) {
        call->ok("Job " + id + " already finished (" + jobStatusName(job->status()) + ").", job->toJson(5));
        return;
    }
    job->requestCancel();
    call->ok("Cancellation requested for job " + id + "; poll get_job until it reports cancelled.", job->toJson(5));
}

// ------------------------------------------------------------------ undo / redo

void undoRedo(const CallPtr& call, bool undo) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string scope = args.value("scope", "scene");
    const int steps = std::max(1, std::min(100, args.value("steps", 1)));
    const char* verb = undo ? "undo" : "redo";

    CommandHistory* history = nullptr;
    Json data = {{"scope", scope}};
    if (scope == "project") {
        history = project->getProjectCommandHistory();
    } else if (scope == "scene") {
        std::string error;
        SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!scene) {
            call->fail(error);
            return;
        }
        history = CommandHandle::get(scene->id);
        data["scene"] = McpRefs::sceneJson(project, *scene);
    } else {
        call->fail("scope must be \"scene\" (GameObject and component edits) or \"project\" (asset file operations).");
        return;
    }

    int done = 0;
    bool refused = false;
    for (; done < steps; done++) {
        if (undo ? !history->canUndo() : !history->canRedo()) break;
        // the command itself can refuse (it would make a child scene cycle)
        if (!(undo ? history->undo() : history->redo())) {
            refused = true;
            break;
        }
    }
    if (scope == "project" && call->ctx().projectWindow()) {
        call->ctx().projectWindow()->refreshCurrentDirectory();
    }
    data["steps"] = done;
    data["can_undo"] = history->canUndo();
    data["can_redo"] = history->canRedo();
    const std::string refusal = std::string("The next step cannot be ") + (undo ? "undone" : "redone") +
        " now: it would make a child scene cycle (read_console has the scenes).";
    if (refused) data["refused"] = true;
    if (done == 0) {
        call->fail(refused ? refusal : std::string("Nothing to ") + verb + " in the " + scope + " history.", data);
        return;
    }
    call->ok(std::string(undo ? "Undid " : "Redid ") + std::to_string(done) + " step(s) in the " + scope + " history." +
             (refused ? " Stopped there: " + refusal : std::string()), data);
}

// ------------------------------------------------------------------ editor_status

void editorStatusTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    App& app = ctx.app();
    McpServer& server = ctx.server();

    Json scenes = Json::array();
    Json unsavedScenes = Json::array();
    bool playing = false;
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        Json item = McpRefs::sceneJson(project, *scene);
        item["selected"] = scene->id == project->getSelectedSceneId();
        item["start_scene"] = scene->id == project->getStartSceneId();
        if (project->isPrefabStage(scene->id)) item["prefab_stage"] = true;
        item["entity_count"] = scene->entities.size();
        scenes.push_back(item);
        if (scene->isModified) unsavedScenes.push_back(scene->id);
        if (scene->playState != ScenePlayState::STOPPED) playing = true;
    }

    Json jobs = Json::array();
    for (const JobPtr& job : ctx.jobs().list()) {
        if (!job->finished()) jobs.push_back(job->toJson(0));
    }

    const McpServerStatus status = server.status();
    Json sessions = Json::array();
    for (const McpSession& session : status.sessions) {
        sessions.push_back({{"transport", session.transport}, {"client", session.clientName},
                            {"client_version", session.clientVersion}, {"protocol_version", session.protocolVersion}});
    }

    Json dialog = nullptr;
    EditorDialogInfo info;
    if (app.getOpenDialog(info)) {
        dialog = {{"kind", info.kind}, {"title", info.title}, {"message", info.message}, {"choices", info.choices}};
    }

    const SceneProject* selected = project->getSelectedScene();
    Json data = {
        {"editor", {{"version", JASPERENGINE_EDITOR_VERSION}}},
        {"project", {
            {"name", project->getName()},
            {"path", FileUtils::pathToGenericUtf8(project->getProjectPath())},
            {"temporary", project->isTempProject()},
            {"selected_scene_id", project->getSelectedSceneId()},
            {"start_scene_id", project->getStartSceneId()}
        }},
        {"scenes", scenes},
        {"playing", playing},
        {"unsaved", {
            {"scene_ids", unsavedScenes},
            {"code_editor", ctx.codeEditor() && ctx.codeEditor()->hasUnsavedChanges()},
            {"temporary_project", project->isTempUnsavedProject()}
        }},
        {"jobs", jobs},
        {"waiting_calls", status.pendingCalls},
        {"auto_sync", McpAutoSync::get().stats()},
        {"dialog", dialog},
        {"mcp", {
            {"server", McpServer::SERVER_NAME},
            {"stdio", status.stdio},
            {"http_port", status.httpRunning ? Json(status.httpPort) : Json(nullptr)},
            {"sessions", sessions}
        }}
    };

    std::string text = "Project \"" + project->getName() + "\" at " + FileUtils::pathToGenericUtf8(project->getProjectPath()) +
                       (project->isTempProject() ? " (temporary, never saved)" : "") + ". " +
                       std::to_string(scenes.size()) + " scene(s)";
    if (selected) text += ", selected: " + std::to_string(selected->id) + " \"" + selected->name + "\"";
    text += playing ? ". Playing." : ".";
    if (!unsavedScenes.empty()) text += " Unsaved scenes: " + unsavedScenes.dump() + ".";
    if (!jobs.empty()) text += " Running jobs: " + std::to_string(jobs.size()) + ".";
    if (!dialog.is_null()) text += " Open dialog: \"" + info.title + "\".";
    call->ok(text, data);
}

// ------------------------------------------------------------------ respond_dialog

void respondDialogTool(const CallPtr& call) {
    App& app = call->ctx().app();
    EditorDialogInfo dialog;
    if (!app.getOpenDialog(dialog)) {
        call->ok("No dialog is open.");
        return;
    }
    const std::string choice = call->args().value("choice", "");
    std::string error;
    if (!app.answerDialog(choice, error)) {
        call->fail(error, Json{{"dialog", {{"kind", dialog.kind}, {"title", dialog.title},
                                           {"message", dialog.message}, {"choices", dialog.choices}}}});
        return;
    }
    // The answer is pressed on the next frame; report what the editor shows after it
    call->afterFrames(2, [call, dialog, choice]() {
        App& app = call->ctx().app();
        EditorDialogInfo next;
        Json data = {{"answered", {{"title", dialog.title}, {"choice", choice}}}};
        std::string text = "Answered \"" + dialog.title + "\" with " + choice + ".";
        if (app.getOpenDialog(next)) {
            data["dialog"] = {{"kind", next.kind}, {"title", next.title}, {"message", next.message}, {"choices", next.choices}};
            text += " Now open: \"" + next.title + "\": " + next.message;
        }
        call->ok(text, data);
    });
}

} // namespace

void registerCoreTools(McpRegistry& registry) {
    using namespace schema;

    // The agent guide as a resource too (DocsTools may replace it with a richer one)
    {
        ResourceDef guide;
        guide.uri = "jasper://guide";
        guide.name = "guide";
        guide.title = "JasperEngine agent guide";
        guide.description = "How to drive the JasperEngine editor through MCP (the initialize instructions).";
        guide.mimeType = "text/markdown";
        guide.read = [](const std::string& uri) {
            ResourceResult result;
            result.contents.push_back({uri, "text/markdown", mcpServerInstructions(), {}});
            return result;
        };
        registry.addResource(std::move(guide));
    }

    {
        ToolDef tool;
        tool.name = "batch";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Batch";
        tool.description =
            "Run tool calls in order in one round-trip: results [{ok, text, structured}], succeeded, failed (the "
            "batch itself doesn't fail: check failed). undo_group: one undo step per scene; undo/redo or Play "
            "start/stop inside prevent it. Not nestable.";
        tool.inputSchema = object({
            {"calls", Json{
                {"type", "array"},
                {"minItems", 1},
                {"maxItems", MAX_BATCH_CALLS},
                {"items", object({
                    {"tool", string("")},
                    {"arguments", anyObject("")}
                }, {"tool"})}
            }},
            {"stop_on_error", withDefault(boolean(""), true)},
            {"undo_group", withDefault(boolean(""), false)}
        }, {"calls"});
        tool.handler = batchTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_job";
        tool.title = "Get Job";
        tool.description =
            "Status of a long operation (build, compile, play startup): status, progress 0-1|null, message, log "
            "tail, result when done. Without job_id: running and recent jobs.";
        tool.inputSchema = object({
            {"job_id", string("Omit to list jobs.")},
            {"log_lines", withDefault(integer("", 0, 200), 40)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getJobTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "cancel_job";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Cancel Job";
        tool.description =
            "Ask a running job to stop; poll get_job until it is cancelled (or it finished first).";
        tool.inputSchema = object({{"job_id", string("")}}, {"job_id"});
        tool.annotations.idempotent = true;
        tool.handler = cancelJobTool;
        registry.addTool(std::move(tool));
    }

    for (const bool undo : {true, false}) {
        ToolDef tool;
        tool.name = undo ? "undo" : "redo";
        tool.title = undo ? "Undo" : "Redo";
        tool.description = undo
            ? "Undo like Ctrl+Z. scope scene: one scene's edits (a step per edit call; a batch with undo_group is "
              "one); project: asset file operations. Returns steps undone and whether more undo/redo is possible."
            : "Redo what undo undid, like Ctrl+Y; same scopes. Any new edit clears the redo history.";
        tool.inputSchema = object({
            {"scope", withDefault(stringEnum("", {"scene", "project"}), "scene")},
            {"scene", sceneRef()},
            {"steps", withDefault(integer("", 1, 100), 1)}
        });
        tool.handler = [undo](CallPtr call) { undoRedo(call, undo); };
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "editor_status";
        tool.title = "Editor Status";
        tool.description =
            "Editor overview: project, open scenes (id, name, path, selected, start, modified, play state, prefab "
            "stage), unsaved changes, jobs, pending dialog, MCP sessions. Call it first.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = editorStatusTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "respond_dialog";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Respond to Dialog";
        tool.description =
            "Click a button of the open editor dialog (editor_status; edit tools are refused meanwhile). The message "
            "says what yes/no mean (e.g. yes = Save, no = Discard Changes); save dialogs take cancel.";
        tool.inputSchema = object({
            {"choice", stringEnum("", {"ok", "yes", "no", "cancel"})}
        }, {"choice"});
        tool.handler = respondDialogTool;
        registry.addTool(std::move(tool));
    }
}

}
