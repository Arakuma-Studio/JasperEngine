// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Core: batch, get_job, cancel_job, undo / redo, editor_status, respond_dialog, tool_groups,
// get_job_system_stats, set_job_system_config, get_memory_stats
#include "McpTools.h"
#include "window/StatusBar.h"

#include "mcp/McpAutoSync.h"
#include "reload/HotReloadService.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpDialogs.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolGroups.h"
#include "mcp/McpUndo.h"
#include "util/ScriptStatus.h"

#include "App.h"
#include "search/SearchPalette.h"
#include "widget/ColorPickerWindow.h"
#include "widget/CurveField.h"
#include "EditorVersion.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "command/type/ThreadingSettingsCmd.h"
#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"
#include "memory/MemoryStats.h"
#include "physics/PlacementSession.h"
#include "util/FileUtils.h"
#include "util/SystemFonts.h"
#include "AppSettings.h"
#include "window/CodeEditor.h"
#include "window/ProjectWindow.h"
#include "window/AssetPanels.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <map>
#include <memory>
#include <set>

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
    // undo_group: each history's position when the batch started, kept from trimming
    McpUndo::Group undo;
    std::string undoName;
};

void finishBatch(const std::shared_ptr<BatchState>& state) {
    Project* project = state->call->ctx().project();
    Json data = {
        {"results", state->results},
        {"succeeded", state->succeeded},
        {"failed", state->failed},
        {"stopped_early", state->stopped}
    };
    if (state->undoGroup) {
        const std::string label = !state->undoName.empty() ? state->undoName
            : "Batch (" + std::to_string(state->calls.size()) + " calls)";
        data["undo_group"] = McpUndo::endGroup(state->undo, project, label);
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
            else app.enqueueMainThreadTask([state]() { McpWriteScope writeScope; runBatchStep(state); });
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
    state->undoName = args.value("undo_name", std::string());
    if (state->undoGroup) McpUndo::beginGroup(state->undo, call->ctx().project());
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

// One step of a single history (scope scene or project), with what it took
// What an undo/redo call learns of the steps it takes (the sandbox hardening): why one was
// refused, the files a taken one could not put back
struct StepReport {
    std::vector<std::filesystem::path> projectRoots;   // the project's trusted roots
    std::string refusedWhy;
    std::vector<std::string> problems;
};

// A step the GUI made (no write root of its own): its files must stay inside the project too,
// once links are resolved (Command::escapeFor); it then runs in this call's write scope
bool escapesProject(const Command* next, bool undo, StepReport& report) {
    if (!next) return false;
    const std::string escape = next->escapeFor(report.projectRoots, undo);
    if (escape.empty()) return false;
    report.refusedWhy = escape;
    return true;
}

bool historyStep(CommandHistory* history, bool undo, std::vector<UndoTimeline::Step>& steps, bool project, uint32_t sceneId,
                 StepReport& report) {
    const UndoEntry* next = undo ? history->top() : history->nextRedo();
    if (!next) return false;
    if (escapesProject(next->cmd.get(), undo, report)) return false;
    UndoTimeline::Step step{next->info.id, next->name(), project, sceneId};
    // the command itself can refuse (it would make a child scene cycle; a file it changes
    // resolves out of its write root through a link): the history keeps its place
    if (!(undo ? history->undo() : history->redo())) {
        report.refusedWhy = history->lastRefusal();
        return false;
    }
    if (!history->lastUndoProblem().empty()) report.problems.push_back(history->lastUndoProblem());
    steps.push_back(step);
    return true;
}

void undoRedo(const CallPtr& call, bool undo) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string scope = args.value("scope", "scene");
    const int steps = std::max(1, std::min(100, args.value("steps", 1)));
    const char* verb = undo ? "undo" : "redo";

    CommandHistory* history = nullptr;
    uint32_t sceneId = NULL_PROJECT_SCENE;
    Json data = {{"scope", scope}};
    if (scope == "project") {
        history = project->getProjectCommandHistory();
    } else if (scope == "scene" || scope == "timeline") {
        std::string error;
        SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!scene) {
            call->fail(error);
            return;
        }
        sceneId = scene->id;
        if (scope == "scene") history = CommandHandle::get(scene->id);
        data["scene"] = McpRefs::sceneJson(project, *scene);
        // a Scene view Physics Placement gesture or settle holds preview poses in the scene: an
        // undo cancels it instead (CommandHistory::beforeChange), a redo is refused
        if (placementSceneBusy(project, scene->id)) {
            if (!undo) {
                data["steps"] = 0;
                call->fail("Redo refused: a Physics Placement gesture is running in the Scene view of \"" + scene->name +
                           "\" (its preview poses are in the scene). Let it finish, or undo to cancel it, then redo.", data);
                return;
            }
            // the history's undo asks the gesture first (CommandHistory::beforeChange) and moves nothing
            CommandHistory* sceneHistory = CommandHandle::get(scene->id);
            sceneHistory->undo();
            data["steps"] = 0;
            data["undone"] = Json::array();
            data["redone"] = Json::array();
            data["cancelled_gesture"] = true;
            data["can_undo"] = history ? history->canUndo() : UndoTimeline::canUndo(sceneId);
            data["can_redo"] = history ? history->canRedo() : UndoTimeline::canRedo(sceneId);
            call->ok("Cancelled the running Physics Placement gesture instead of undoing (nothing was undone): its objects "
                     "are back where they were before it.", data);
            return;
        }
    } else {
        call->fail("scope must be \"timeline\" (the scene's and the asset steps in order, like Ctrl+Z), \"scene\" "
                   "(GameObject and component edits) or \"project\" (asset file operations).");
        return;
    }

    // The step undo/redo takes next in the scope
    auto nextId = [&]() -> uint64_t {
        if (history) {
            const UndoEntry* next = undo ? history->top() : history->nextRedo();
            return next ? next->info.id : 0;
        }
        UndoTimeline::EntryView next = undo ? UndoTimeline::nextUndo(sceneId) : UndoTimeline::nextRedo(sceneId);
        return next ? next.entry->info.id : 0;
    };
    auto inScope = [&](uint64_t id) -> const UndoEntry* {
        if (history) return history->find(id);
        UndoTimeline::EntryView view = UndoTimeline::find(sceneId, id);
        return view.entry;
    };
    auto applied = [&](uint64_t id) {
        if (history) return history->isApplied(id);
        UndoTimeline::EntryView view = UndoTimeline::find(sceneId, id);
        return view && view.applied;
    };

    // expect_top_id: change nothing unless the next step is that one (an agent undoing only its own call)
    if (args.contains("expect_top_id") && !args["expect_top_id"].is_null()) {
        const uint64_t expected = args["expect_top_id"].get<uint64_t>();
        const uint64_t actual = nextId();
        if (actual != expected) {
            data["next_id"] = actual == 0 ? Json(nullptr) : Json(actual);
            call->fail(std::string("The next step to ") + verb + " is " +
                       (actual == 0 ? std::string("none") : std::to_string(actual)) + ", not " + std::to_string(expected) +
                       ": nothing changed. get_undo_history lists the steps.", data);
            return;
        }
    }

    std::vector<UndoTimeline::Step> done;
    std::string why;
    bool stopped = false;   // a step did not move: refused (busy, or by the command) or nothing left
    StepReport report;
    report.projectRoots = McpRefs::trustedRoots(project);
    // the timeline's next step: checked as historyStep checks its own, its history's report read after
    auto timelineStep = [&](bool undoing) {
        UndoTimeline::EntryView next = undoing ? UndoTimeline::nextUndo(sceneId) : UndoTimeline::nextRedo(sceneId);
        if (next && escapesProject(next.entry->cmd.get(), undoing, report)) return false;
        const size_t before = done.size();
        const bool moved = undoing ? UndoTimeline::undo(sceneId, &why, &done) : UndoTimeline::redo(sceneId, &why, &done);
        for (size_t i = before; i < done.size(); i++) {
            CommandHistory* stepHistory = done[i].project ? project->getProjectCommandHistory() : CommandHandle::get(done[i].sceneId);
            if (stepHistory && !stepHistory->lastUndoProblem().empty()) report.problems.push_back(stepHistory->lastUndoProblem());
        }
        if (!moved && why.empty() && next && next.history && !next.history->lastRefusal().empty()) {
            report.refusedWhy = next.history->lastRefusal();
        }
        return moved;
    };
    if (args.contains("to_id") && !args["to_id"].is_null()) {
        const uint64_t target = args["to_id"].get<uint64_t>();
        if (!inScope(target)) {
            call->fail("No step " + std::to_string(target) + " in the " + scope + " history (get_undo_history lists them).", data);
            return;
        }
        if (undo != applied(target)) {
            call->fail("Step " + std::to_string(target) + (undo ? " is not applied: redo to it instead." : " is applied already: undo to it instead."), data);
            return;
        }
        if (history) {
            // undo until it is the newest applied step, or redo until it is applied
            while (undo ? (history->top() && history->top()->info.id != target) : !history->isApplied(target)) {
                if (!historyStep(history, undo, done, scope == "project", sceneId, report)) {
                    stopped = true;
                    break;
                }
            }
        } else {
            stopped = !UndoTimeline::jumpTo(sceneId, target, &why, &done);
        }
    } else {
        for (int i = 0; i < steps; i++) {
            if (history) {
                if (!historyStep(history, undo, done, scope == "project", sceneId, report)) {
                    stopped = true;
                    break;
                }
            } else if (!timelineStep(undo)) {
                stopped = true;
                break;
            }
        }
    }
    if (history && why.empty() && report.refusedWhy.empty() && !CommandHistory::lastBusyRefusal().empty()) {
        why = "Busy: " + CommandHistory::lastBusyRefusal();
    }
    const bool busy = why.rfind("Busy", 0) == 0;
    // the next step is there but did not move: the command refused (it would make a child scene cycle)
    const bool refused = stopped && !busy && nextId() != 0;
    const std::string refusal = std::string("The next step cannot be ") + (undo ? "undone" : "redone") + " now: " +
        (report.refusedWhy.empty() ? std::string("it would make a child scene cycle (read_console has the scenes).") : report.refusedWhy);
    if (refused) data["refused"] = true;
    if (refused && !report.refusedWhy.empty()) data["refusal"] = report.refusedWhy;
    // a step taken whose files could not all be put back (a link planted after its check)
    std::string problemText;
    if (!report.problems.empty()) {
        data["problems"] = report.problems;
        for (const std::string& problem : report.problems) problemText += std::string(undo ? " Undo incomplete: " : " Redo incomplete: ") + problem + ".";
    }
    bool touchedProject = false;
    Json list = Json::array();
    for (const UndoTimeline::Step& step : done) {
        list.push_back(McpUndo::stepJson(step));
        touchedProject = touchedProject || step.project;
    }
    // the timeline refreshes the Project window itself
    if (history && touchedProject && call->ctx().projectWindow()) {
        call->ctx().projectWindow()->refreshCurrentDirectory();
    }
    data["steps"] = done.size();
    data["undone"] = undo ? list : Json::array();
    data["redone"] = undo ? Json::array() : list;
    if (history) {
        data["can_undo"] = history->canUndo();
        data["can_redo"] = history->canRedo();
    } else {
        data["can_undo"] = UndoTimeline::canUndo(sceneId);
        data["can_redo"] = UndoTimeline::canRedo(sceneId);
    }
    if (done.empty()) {
        std::string empty = std::string("Nothing to ") + verb + " in the " + scope + " history.";
        // An asset step (a graph file, an import) is newer than the scene's: Ctrl+Z would take it
        if (scope == "scene" && (undo ? UndoTimeline::canUndo(sceneId) : UndoTimeline::canRedo(sceneId))) {
            empty += std::string(" The last step is an asset edit: ") + verb + " scope timeline takes it, as Ctrl+Z does.";
        }
        call->fail(busy ? why : refused ? refusal : empty, data);
        return;
    }
    std::string names;
    for (const UndoTimeline::Step& step : done) {
        if (!names.empty()) names += ", ";
        names += "\"" + step.name + "\"";
    }
    if (!report.problems.empty()) {
        call->fail(std::string(undo ? "Undid " : "Redid ") + std::to_string(done.size()) + " step(s) in the " + scope + " history: " +
                   names + "." + problemText + " Remove the link, then check those files.", data);
        return;
    }
    call->ok(std::string(undo ? "Undid " : "Redid ") + std::to_string(done.size()) + " step(s) in the " + scope +
             " history: " + names + "." + (refused ? " Stopped there: " + refusal : std::string()), data);
}

// ------------------------------------------------------------------ get_undo_history

void getUndoHistoryTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string scope = args.value("scope", "timeline");
    const int limit = std::max(1, std::min(500, args.value("limit", 30)));
    const bool includeSelection = args.value("include_selection", false);
    const bool includeRedo = args.value("include_redo", true);

    uint32_t sceneId = NULL_PROJECT_SCENE;
    Json data = {{"scope", scope}};
    if (scope != "project") {
        std::string error;
        SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!scene) {
            call->fail(error);
            return;
        }
        sceneId = scene->id;
        data["scene"] = McpRefs::sceneJson(project, *scene);
    }
    if (scope != "timeline" && scope != "scene" && scope != "project") {
        call->fail("scope must be \"timeline\", \"scene\" or \"project\".");
        return;
    }

    std::vector<UndoTimeline::EntryView> views = UndoTimeline::entries(sceneId, includeSelection);
    Json entries = Json::array();
    std::string text;
    int listed = 0;
    for (auto it = views.rbegin(); it != views.rend() && listed < limit; ++it) {
        if (scope == "scene" && it->project) continue;
        if (scope == "project" && !it->project) continue;
        if (!includeRedo && !it->applied) continue;
        entries.push_back(McpUndo::entryJson(*it));
        text += "\n" + std::to_string(it->entry->info.id) + (it->applied ? "  " : " (redo) ") + it->entry->name() +
                (it->project ? " [project]" : "");
        listed++;
    }
    UndoTimeline::EntryView nextUndo = UndoTimeline::nextUndo(sceneId);
    UndoTimeline::EntryView nextRedo = UndoTimeline::nextRedo(sceneId);
    data["entries"] = entries;
    data["undo_name"] = nextUndo ? Json(nextUndo.entry->name()) : Json(nullptr);
    data["redo_name"] = nextRedo ? Json(nextRedo.entry->name()) : Json(nullptr);
    data["can_undo"] = UndoTimeline::canUndo(sceneId);
    data["can_redo"] = UndoTimeline::canRedo(sceneId);
    data["memory"] = {{"used", CommandHistory::totalBytes()}, {"limit", CommandHistory::memoryLimit}};
    if (CommandHistory* history = UndoTimeline::sceneHistory(sceneId); history && history->isInPlaySession()) {
        data["play_floor_id"] = history->getPlayStartId();
    }
    call->ok(std::to_string(listed) + " step(s), newest first" + (listed ? ":" : ".") + text, data);
}

// ------------------------------------------------------------------ job system and memory

Json jobSystemStatsJson(int windowFrames) {
    const jobs::Stats stats = jobs::system().stats(windowFrames);
    Json lanes = Json::object();
    for (size_t lane = 0; lane < jobs::kLaneCount; lane++) {
        lanes[jobs::laneName((jobs::Lane)lane)] = {{"queued", stats.lanes[lane].queued}, {"running", stats.lanes[lane].running}};
    }
    Json channels = Json::array();
    for (const jobs::ChannelStats& channel : stats.channels) {
        channels.push_back({{"name", channel.name}, {"queued", channel.queued}, {"running", channel.running}, {"cap", channel.cap}});
    }
    const AssetIOService::IOStats io = AssetIOService::instance().stats();
    Json queuedByPriority = Json::object();
    for (size_t level = 0; level < kIOPriorityCount; level++) {
        queuedByPriority[ioPriorityName((IOPriority)level)] = io.queuedByPriority[level];
    }
    Json reads = Json::array();
    for (const AssetIOService::ReadInfo& read : io.inFlight) {
        reads.push_back({{"id", read.id}, {"path", read.path}, {"priority", ioPriorityName(read.priority)}, {"claimed", read.claimed}});
    }
    Json perWorker = Json::array();
    for (const jobs::WorkerStats& worker : stats.perWorker) {
        perWorker.push_back({{"busy_pct", std::round(worker.busyPct * 10.0) / 10.0}, {"jobs", worker.jobs}});
    }
    return {
        {"workers", stats.workers},
        {"single_thread", stats.singleThread},
        {"lanes", lanes},
        {"gate", {{"max", stats.gate.max}, {"taken", stats.gate.taken}, {"heavy_max", stats.gate.heavyMax},
                  {"heavy_taken", stats.gate.heavyTaken}, {"reserve", stats.gate.reserve},
                  {"peak", stats.gate.peakTaken}, {"heavy_peak", stats.gate.peakHeavyTaken}}},
        {"channels", channels},
        {"blocking_threads", stats.blockingThreads},
        {"io", {{"queued_by_priority", queuedByPriority}, {"in_flight", reads}, {"readers", io.readers}}},
        {"per_worker", perWorker},
        {"window_frames", stats.windowFrames},
        {"asset_jobs", stats.assetJobs},
        {"totals", {{"executed", stats.totals.executed}, {"stolen", stats.totals.stolen},
                    {"cancelled", stats.totals.cancelled}, {"failed", stats.totals.failed},
                    {"heap_envelopes", stats.totals.heapEnvelopes}}},
    };
}

void getJobSystemStatsTool(const CallPtr& call) {
    const int window = std::max(1, std::min(120, call->args().value("window_frames", 60)));
    Json data = jobSystemStatsJson(window);
    call->ok("Job system: " + std::to_string(data["workers"].get<int>()) + " worker(s), Background gate " +
             std::to_string(data["gate"]["taken"].get<int>()) + "/" + std::to_string(data["gate"]["max"].get<int>()) +
             ", " + std::to_string(data["asset_jobs"].get<uint32_t>()) + " asset job(s) queued or running.", data);
}

void setJobSystemConfigTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    ThreadingProjectSettings settings = project->getThreadingSettings();
    if (args.contains("reserved_frame_workers")) settings.reservedFrameWorkers = args["reserved_frame_workers"].get<int>();
    if (args.contains("web_pump_budget_ms")) settings.webPumpBudgetMs = args["web_pump_budget_ms"].get<float>();
    if (settings == project->getThreadingSettings()) {
        call->ok("Nothing changed.", jobSystemStatsJson(1));
        return;
    }
    // the settings path: project.yaml, live on the editor's job system, one project undo step
    project->getProjectCommandHistory()->addCommand(new ThreadingSettingsCmd(project, settings));
    Json data = jobSystemStatsJson(1);
    call->ok("Threading settings applied (undo with scope project). Background gate max is now " +
             std::to_string(data["gate"]["max"].get<int>()) + ".", data);
}

void getMemoryStatsTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string prefix = args.value("prefix", "");
    const std::string residency = args.value("residency", "");
    const std::string sort = args.value("sort", "current");
    std::vector<memory::StatsEntry> entries = memory::snapshot();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const memory::StatsEntry& entry) {
        return entry.name.compare(0, prefix.size(), prefix) != 0
            || (!residency.empty() && residency != memory::residencyName(entry.residency));
    }), entries.end());
    std::sort(entries.begin(), entries.end(), [&sort](const memory::StatsEntry& a, const memory::StatsEntry& b) {
        if (sort == "name") return a.name < b.name;
        if (sort == "peak") return a.peak > b.peak;
        if (sort == "capacity") return a.capacity > b.capacity;
        return a.current > b.current;
    });
    Json records = Json::array();
    std::map<std::string, uint64_t> totals;
    for (const memory::StatsEntry& entry : entries) {
        records.push_back({{"name", entry.name}, {"kind", memory::kindName(entry.kind)},
                           {"residency", memory::residencyName(entry.residency)}, {"current", entry.current},
                           {"peak", entry.peak}, {"capacity", entry.capacity}, {"count", entry.count}});
        totals[memory::residencyName(entry.residency)] += entry.current;
    }
    Json totalsJson = Json::object();
    for (const char* name : {"Cpu", "Gpu", "Mapped"}) totalsJson[name] = totals[name];
    call->ok(std::to_string(records.size()) + " memory record(s).", Json{{"records", records}, {"totals", totalsJson}});
}


// ------------------------------------------------------------------ editor_status

// The file watcher and the hot reload (S05 R31)
Json fileWatcherJson() {
    const FileWatchService::Stats stats = FileWatchService::get().stats();
    HotReloadService& reloads = HotReloadService::get();
    return {
        {"backend", stats.backend.empty() ? Json(nullptr) : Json(stats.backend)},
        {"state", FileWatchService::stateName(stats.state)},
        {"auto_refresh", reloads.getAutoRefresh()},
        {"roots", stats.roots},
        {"watches", stats.watches},
        {"events", stats.events},
        {"coalesced", stats.coalesced},
        {"echoes_suppressed", stats.echoesSuppressed},
        {"overflows", stats.overflows},
        {"pending", stats.pending + reloads.pendingExternalChanges() + McpAutoSync::get().heldAtCallStart()},
        {"reloading", reloads.reloadingCount()},
        {"reloads", reloads.reloadCount()},
        {"last_reload", reloads.lastReloadJson()},
        {"poll_interval_ms", stats.pollInterval.count()}
    };
}

// The system fonts the editor's fonts fall back to for CJK and emoji (SystemFonts)
Json editorFontsJson() {
    static const char* const states[] = {"idle", "searching", "ready", "none"};
    Json fallbacks = Json::array();
    for (const SystemFonts::Font& font : SystemFonts::fonts()) {
        fallbacks.push_back(Json{{"file", font.path}, {"face", font.faceIndex}, {"covers", font.covers}});
    }
    return Json{{"state", states[static_cast<int>(SystemFonts::state())]},
                {"cjk_font_language", AppSettings::getCjkFontLanguage()},
                {"effective_language", SystemFonts::languageTag(SystemFonts::effectiveLanguage())},
                {"fallbacks", fallbacks}};
}

void reportEditorStatus(const CallPtr& call) {
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
        dialog = dialogJson(info);
    }

    // What the asset windows show (S04), with the Project window's clickable rects in window
    // pixels (UI automation: a test right-clicks an entry for real)
    auto rectJson = [](const ProjectWindow::UiRect& r) { return Json::array({r.x, r.y, r.w, r.h}); };
    Json ui = Json::object();
    if (ProjectWindow* projectWindow = ctx.projectWindow()) {
        const ProjectWindow::UiState& state = projectWindow->getUiState();
        Json entries = Json::array();
        for (const auto& [name, rect] : state.entries) entries.push_back({{"name", name}, {"rect", rectJson(rect)}});
        Json menu = Json::array();
        for (const auto& [label, rect] : state.contextMenu) menu.push_back({{"label", label}, {"rect", rectJson(rect)}});
        Json tree = Json::array();
        for (const ProjectWindow::UiTreeNode& node : state.tree) {
            tree.push_back({{"path", node.path}, {"rect", rectJson(node.rect)}, {"arrow", Json::array({node.arrowX, node.arrowY})},
                            {"open", node.open}});
        }
        ui["project_window"] = {{"drawn", state.drawn}, {"directory", state.directory}, {"search", state.search},
                                {"selected", state.selected}, {"search_box", rectJson(state.searchBox)},
                                {"tree", tree}, {"entries", entries}, {"context_menu", menu}};
    }
    if (AssetReferencesWindow* references = app.getAssetReferencesWindow()) {
        Json referrers = Json::array();
        for (const DepEdge& edge : references->getResults()) {
            referrers.push_back({{"referrer", project->getAssetIndex().referrerPath(edge.referrer)}, {"field", edge.field}});
        }
        ui["references"] = {{"open", references->isOpen()}, {"asset", references->getTargetPath()},
                            {"guid", references->getTargetGuid().isNull() ? Json(nullptr) : Json(references->getTargetGuid().str())},
                            {"results", referrers}};
    }
    if (MissingAssetsWindow* missing = app.getMissingAssetsWindow()) {
        ui["missing_assets"] = {{"open", missing->isOpen()}};
    }
    if (SearchPalette* palette = app.getSearchPalette()) {
        const SearchPalette::UiState state = palette->uiState();
        ui["search_palette"] = {{"open", state.open}, {"query", state.query}, {"results", state.results}, {"selected", state.selected}};
    }
    {
        const ui::CurveWindow::UiState state = ui::CurveWindow::get().uiState();
        ui["curve_window"] = {{"open", state.open}, {"focused", state.focused}, {"selected_keys", state.selectedKeys}, {"clipboard_keys", state.clipboardKeys},
                              {"view", {{"t0", state.view.t0}, {"t1", state.view.t1}, {"v0", state.view.v0}, {"v1", state.view.v1}}}};
        const ui::ColorPickerWindow& picker = ui::ColorPickerWindow::get();
        ui["color_picker"] = {{"open", picker.isOpen()}, {"focused", picker.isFocused()}, {"sampling", picker.isSampling()}};
    }

    const SceneProject* selected = project->getSelectedScene();
    // the Edit menu's labels come from the same calls (UndoTimeline::undoLabel)
    const uint32_t undoScene = selected ? selected->id : NULL_PROJECT_SCENE;
    UndoTimeline::EntryView nextUndo = UndoTimeline::nextUndo(undoScene);
    UndoTimeline::EntryView nextRedo = UndoTimeline::nextRedo(undoScene);
    Json undoStatus = {
        {"undo_name", nextUndo ? Json(nextUndo.entry->name()) : Json(nullptr)},
        {"redo_name", nextRedo ? Json(nextRedo.entry->name()) : Json(nullptr)},
        {"undo_label", UndoTimeline::undoLabel(undoScene)},
        {"redo_label", UndoTimeline::redoLabel(undoScene)}
    };
    Json data = {
        {"undo", undoStatus},
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
        {"file_watcher", fileWatcherJson()},
        {"editor_fonts", editorFontsJson()},
        // the status bar's short message ("Preparing picking for Rock...", S26)
        {"status_message", StatusBar::getTransientMessage()},
        {"scripts", ScriptStatus::get().toJson()},
        {"frame_ms", app.takeFrameStats()},
        {"dialog", dialog},
        {"ui", ui},
        // S25: what the layouts and the palette change
        {"windows", {{"focused", app.focusedWindowName()}, {"open", app.openPanels()}}},
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

void editorStatusTool(const CallPtr& call) {
    // a modal opened by the last frame is drawn by the next: a status sent right after it says so
    if (EditorModal::settling()) {
        call->ctx().app().requestRedraw();
        call->afterFrames(1, [call]() { reportEditorStatus(call); });
        return;
    }
    reportEditorStatus(call);
}

// ------------------------------------------------------------------ tool_groups

// Adds (on) or removes the groups args[key] names; false (call failed) on a bad name
bool changeToolGroups(const CallPtr& call, const char* key, bool on, std::set<std::string>& enabled) {
    const Json& args = call->args();
    if (!args.contains(key)) return true;
    if (!args[key].is_array()) {
        call->fail(std::string(key) + " must be an array of group names.");
        return false;
    }
    for (const Json& item : args[key]) {
        const std::string name = item.is_string() ? item.get<std::string>() : item.dump();
        if (!item.is_string() || !findToolGroup(name)) {
            std::string valid;
            for (const ToolGroupInfo& group : toolGroups()) valid += (valid.empty() ? "" : ", ") + group.name;
            call->fail("Unknown tool group " + name + ". Groups: " + valid + ".");
            return false;
        }
        if (name == CORE_TOOL_GROUP) {
            if (!on) {
                call->fail("core is always listed and can't be disabled.");
                return false;
            }
            continue;
        }
        if (on) enabled.insert(name);
        else enabled.erase(name);
    }
    return true;
}

void toolGroupsTool(const CallPtr& call) {
    McpServer& server = call->server();
    const SessionPtr session = call->session();
    std::set<std::string> enabled = server.sessionToolGroups(session);
    const std::set<std::string> before = enabled;
    if (!changeToolGroups(call, "enable", true, enabled) || !changeToolGroups(call, "disable", false, enabled)) return;
    if (enabled != before) {
        if (!session) {
            call->fail("This call has no client session whose tool listing could change.");
            return;
        }
        server.setSessionToolGroups(session, enabled);
    }

    Json groups = Json::array();
    size_t listedTools = 0;
    std::string listed;
    for (const ToolGroupInfo& group : toolGroups()) {
        Json names = Json::array();
        size_t chars = 0;
        for (const ToolDef& def : server.registry().tools()) {
            if (McpRegistry::groupOf(def) != group.name) continue;
            names.push_back(def.name);
            chars += McpRegistry::toolChars(def);
        }
        const bool on = group.name == CORE_TOOL_GROUP || enabled.count(group.name) > 0;
        if (on) {
            listedTools += names.size();
            listed += (listed.empty() ? "" : ", ") + group.name;
        }
        groups.push_back({{"name", group.name}, {"description", group.description}, {"tools", names},
                          {"chars", chars}, {"enabled", on}});
    }

    std::string text = "tools/list shows " + listed + " (" + std::to_string(listedTools) + " tools).";
    Json data = {{"groups", groups}, {"list_changed", false}};
    if (enabled != before) {
        const bool sent = call->notifyClient(Json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
        data["list_changed"] = sent;
        text += sent ? " Sent notifications/tools/list_changed: re-list tools."
                     : " This client can't receive notifications/tools/list_changed (HTTP without text/event-stream): "
                       "call tools/list again, or call the tools by name (also in batch).";
    }
    for (const Json& group : groups) {
        text += "\n" + group["name"].get<std::string>() + (group["enabled"].get<bool>() ? " [listed]" : "") + ": " +
                std::to_string(group["tools"].size()) + " tools, " + std::to_string(group["chars"].get<size_t>()) +
                " chars. " + group["description"].get<std::string>();
    }
    call->ok(text, data);
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
            "batch itself doesn't fail: check failed). undo_group: one linked undo step (one Ctrl+Z) over the "
            "histories it changed, named undo_name; undo/redo or Play start/stop inside prevent it. Not nestable.";
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
            {"undo_group", withDefault(boolean(""), false)},
            {"undo_name", string("undo_group's step name (default \"Batch (N calls)\").")}
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
            ? "Undo. scope timeline (recommended): exactly Ctrl+Z, the scene's and the asset steps in order; scene: "
              "one scene's edits; project: asset file operations. to_id: until that step is the newest applied; "
              "expect_top_id: refuse unless the next step is that id. Returns undone [{id, name}]."
            : "Redo what undo undid, like Ctrl+Y; same scopes, to_id (until applied) and expect_top_id. Any new edit "
              "clears the redo history.";
        tool.inputSchema = object({
            {"scope", withDefault(stringEnum("", {"timeline", "scene", "project"}), "scene")},
            {"scene", sceneRef()},
            {"steps", withDefault(integer("", 1, 100), 1)},
            {"to_id", integer("A step id from get_undo_history.")},
            {"expect_top_id", integer("Refuse unless the next step is this id.")}
        });
        tool.handler = [undo](CallPtr call) { undoRedo(call, undo); };
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_undo_history";
        tool.group = "editor_ux";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Get Undo History";
        tool.description =
            "Undo steps newest first: {id, name, type, history (project|scene id), origin {kind, client, tool}, time, "
            "bytes, applied, link_id}, the next undo/redo names, memory. scope timeline: what Ctrl+Z walks.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"scope", withDefault(stringEnum("", {"timeline", "scene", "project"}), "timeline")},
            {"limit", withDefault(integer("", 1, 500), 30)},
            {"include_selection", withDefault(boolean("Selection steps too."), false)},
            {"include_redo", withDefault(boolean(""), true)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getUndoHistoryTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "editor_status";
        tool.title = "Editor Status";
        tool.description =
            "Editor overview: project, open scenes (id, name, path, selected, start, modified, play state, prefab "
            "stage), unsaved changes, jobs, pending dialog, MCP sessions, file watcher, frame times. Call it first.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = editorStatusTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_job_system_stats";
        tool.group = "diagnostics";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Get Job System Stats";
        tool.description = "Job system: workers, lanes, Background gate, channels, asset I/O, per-worker busy % over window_frames, totals.";
        tool.inputSchema = object({{"window_frames", withDefault(integer("", 1, 120), 60)}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getJobSystemStatsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_job_system_config";
        tool.group = "diagnostics";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Set Job System Config";
        tool.description = "Player > Threading, applied live and saved: reserved_frame_workers (-1 Auto), web_pump_budget_ms. Undo: scope project.";
        tool.inputSchema = object({
            {"reserved_frame_workers", integer("", -1, 4)},
            {"web_pump_budget_ms", number("", 0.5, 50.0)}
        });
        tool.handler = setJobSystemConfigTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_memory_stats";
        tool.group = "diagnostics";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Get Memory Stats";
        tool.description = "Memory records {name, kind, residency, current, peak, capacity, count} in bytes, totals by residency.";
        tool.inputSchema = object({
            {"prefix", string("e.g. Texture.")},
            {"residency", stringEnum("", {"Cpu", "Gpu", "Mapped"})},
            {"sort", withDefault(stringEnum("", {"current", "peak", "capacity", "name"}), "current")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getMemoryStatsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "respond_dialog";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Respond to Dialog";
        tool.description =
            "Answer the open editor dialog or file picker (editor_status.dialog): values fill its fields, then choice "
            "presses a button. A refusal is an error and the dialog stays open.";
        tool.inputSchema = object({
            {"choice", string("A word of dialog.choices.")},
            {"values", anyObject("{field: value} of dialog.fields; a file picker takes path.")}
        });
        tool.handler = respondDialogTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "tool_groups";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Tool Groups";
        tool.description =
            "List the tool groups and what each holds; enable/disable lists a group's tools in this session (core "
            "always is).";
        tool.inputSchema = object({
            {"enable", array(string(""), "")},
            {"disable", array(string(""), "")}
        });
        // changes what this client lists, never the project
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = toolGroupsTool;
        registry.addTool(std::move(tool));
    }
}

}
