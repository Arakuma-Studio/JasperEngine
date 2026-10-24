// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Core: batch, get_job, cancel_job, undo / redo, editor_status, respond_dialog,
// get_job_system_stats, set_job_system_config, get_memory_stats
#include "McpTools.h"

#include "mcp/McpAutoSync.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/McpUndo.h"

#include "App.h"
#include "EditorVersion.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "command/type/ThreadingSettingsCmd.h"
#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"
#include "memory/MemoryStats.h"
#include "window/CodeEditor.h"
#include "window/ProjectWindow.h"

#include <algorithm>
#include <cmath>
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
    // undo_group: each history's position when the batch started, kept from trimming
    std::map<uint32_t, size_t> sceneMarks;
    size_t projectMark = 0;
    std::vector<std::unique_ptr<UndoGroupScope>> trimHolds;
    std::string undoName;
};

void beginUndoGroup(BatchState& state, Project* project) {
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        CommandHistory* history = CommandHandle::get(scene->id);
        state.sceneMarks[scene->id] = history->mark();
        state.trimHolds.push_back(std::make_unique<UndoGroupScope>(history));
    }
    state.projectMark = project->getProjectCommandHistory()->mark();
    state.trimHolds.push_back(std::make_unique<UndoGroupScope>(project->getProjectCommandHistory()));
}

// One undo step per history the batch changed, linked so one Ctrl+Z undoes the whole batch
// (S24 R25). Scenes the batch created start at 0.
Json endUndoGroup(BatchState& state, Project* project) {
    Json grouped = Json::array();
    bool complete = true;
    const std::string label = !state.undoName.empty() ? state.undoName
        : "Batch (" + std::to_string(state.calls.size()) + " calls)";
    std::vector<CommandHistory*> histories;
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        auto it = state.sceneMarks.find(scene->id);
        const size_t mark = it != state.sceneMarks.end() ? it->second : 0;
        CommandHistory* history = CommandHandle::get(scene->id);
        if (history->position() <= mark) continue;
        if (history->groupSince(mark, label)) {
            grouped.push_back(scene->id);
            histories.push_back(history);
        } else {
            complete = false;
        }
    }
    CommandHistory* projectHistory = project->getProjectCommandHistory();
    if (projectHistory->position() > state.projectMark) {
        if (projectHistory->groupSince(state.projectMark, label)) histories.push_back(projectHistory);
        else complete = false;
    } else if (!projectHistory->groupSince(state.projectMark)) {
        complete = false;
    }
    state.trimHolds.clear();
    Json data = {{"scene_ids", grouped}, {"complete", complete}};
    if (histories.size() > 1) {
        data["link_id"] = UndoTimeline::link(histories);
    }
    const UndoEntry* newest = nullptr;
    for (CommandHistory* history : histories) {
        const UndoEntry* top = history->top();
        if (top && (!newest || top->info.id > newest->info.id)) newest = top;
    }
    if (newest) {
        data["undo"] = {{"id", newest->info.id}, {"name", newest->name()}};
    }
    return data;
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
    state->undoName = args.value("undo_name", std::string());
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

// One step of a single history (scope scene or project), with what it took
bool historyStep(CommandHistory* history, bool undo, std::vector<UndoTimeline::Step>& steps, bool project, uint32_t sceneId) {
    const UndoEntry* next = undo ? history->top() : history->nextRedo();
    if (!next) return false;
    UndoTimeline::Step step{next->info.id, next->name(), project, sceneId};
    if (!(undo ? history->undo() : history->redo())) return false;
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
                if (!historyStep(history, undo, done, scope == "project", sceneId)) break;
            }
        } else {
            UndoTimeline::jumpTo(sceneId, target, &why, &done);
        }
    } else {
        for (int i = 0; i < steps; i++) {
            if (history) {
                if (!historyStep(history, undo, done, scope == "project", sceneId)) break;
            } else if (!(undo ? UndoTimeline::undo(sceneId, &why, &done) : UndoTimeline::redo(sceneId, &why, &done))) {
                break;
            }
        }
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
        call->fail(why.rfind("Busy", 0) == 0 ? why : std::string("Nothing to ") + verb + " in the " + scope + " history.", data);
        return;
    }
    std::string names;
    for (const UndoTimeline::Step& step : done) {
        if (!names.empty()) names += ", ";
        names += "\"" + step.name + "\"";
    }
    call->ok(std::string(undo ? "Undid " : "Redid ") + std::to_string(done.size()) + " step(s) in the " + scope +
             " history: " + names + ".", data);
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
            {"path", project->getProjectPath().generic_string()},
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

    std::string text = "Project \"" + project->getName() + "\" at " + project->getProjectPath().generic_string() +
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
            "stage), unsaved changes, jobs, pending dialog, MCP sessions. Call it first.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = editorStatusTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_job_system_stats";
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
