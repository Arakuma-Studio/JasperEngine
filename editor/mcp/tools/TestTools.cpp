// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// run_tests (docs/specs/project-test-runner.md R42, §6.6): the project's tests as a job, their
// static listing, a player's results imported, captures accepted as goldens.

#include "McpTools.h"

#include "App.h"
#include "Project.h"
#include "io/AtomicFile.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "test/GameTests.h"
#include "test/TestRun.h"
#include "util/Base64.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;
namespace fs = std::filesystem;
namespace gt = jasperengine::gametest;

constexpr size_t MAX_FAILURES = 20;

std::string relative(Project* project, const fs::path& path) {
    return project->normalizeToProjectRelative(path).generic_string();
}

const char* statusWord(gt::Status status) {
    switch (status) {
        case gt::Status::Passed: return "PASS";
        case gt::Status::Failed: return "FAIL";
        case gt::Status::Skipped: return "SKIP";
        case gt::Status::Error: return "ERROR";
        case gt::Status::Hung: return "HUNG";
    }
    return "ERROR";
}

std::string countsText(const gt::Counts& c) {
    std::string text = std::to_string(c.passed) + " passed, " + std::to_string(c.failed) + " failed, " + std::to_string(c.skipped) + " skipped";
    if (c.error) text += ", " + std::to_string(c.error) + " error";
    if (c.hung) text += ", " + std::to_string(c.hung) + " hung";
    if (c.excludedEdit) text += ", " + std::to_string(c.excludedEdit) + " Edit Mode excluded";
    return text;
}

// a path an agent gave: project-relative, or absolute (a file a device run left elsewhere)
bool readablePath(Project* project, const std::string& given, fs::path& out, std::string& error) {
    const fs::path path = FileUtils::pathFromUtf8(given);
    if (path.is_absolute()) {
        out = path;
        std::error_code ec;
        if (!fs::is_regular_file(out, ec)) {
            error = given + ": no such file";
            return false;
        }
        return true;
    }
    return McpRefs::resolveProjectPath(project, given, out, error, true);
}

bool writeJunit(Project* project, const std::string& given, const gt::RunResult& result, std::string& written, std::string& error) {
    fs::path out = FileUtils::pathFromUtf8(given);
    if (!out.is_absolute() && !McpRefs::resolveProjectWritePath(project, given, out, error)) return false;
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult w = io::writeFileAtomic(out, gt::junitXml(result), options);
    if (!w.ok()) {
        error = "junit: " + w.message;
        return false;
    }
    written = FileUtils::pathToGenericUtf8(out);
    return true;
}

// what a finished run (or an import) returns: text, structuredContent, at most one image
ToolResult outcomeResult(Project* project, const gametests::RunOutcome& outcome, const Json& args) {
    const gt::RunResult& run = outcome.result;
    std::string text;
    Json failures = Json::array();
    std::string firstCapture;   // "<id>/<name>"
    Json firstCaptureJson;
    for (const gt::CaseResult& r : run.cases) {
        std::string line = std::string(statusWord(r.status)) + " " + r.spec.id;
        if (!r.failures.empty()) {
            const gt::Failure& f = r.failures.front();
            line += "  " + f.file + ":" + std::to_string(f.line) + ": " + f.message;
        } else if (r.status == gt::Status::Skipped) {
            line += "  (" + r.skipReason + ")";
        }
        text += line + "\n";
        for (const gt::Failure& f : r.failures) {
            if (failures.size() >= MAX_FAILURES) break;
            failures.push_back({{"id", r.spec.id}, {"status", gt::statusName(r.status)}, {"kind", f.kind}, {"message", f.message},
                                {"file", f.file}, {"line", f.line}, {"frame", f.frame}});
        }
        for (const Json& capture : r.captures) {
            const std::string status = capture.value("status", std::string());
            if (firstCapture.empty() && (status == "failed" || status == "new")) {
                firstCapture = r.spec.id + "/" + capture.value("name", std::string());
                firstCaptureJson = capture;
            }
        }
    }
    text += countsText(run.counts) + (outcome.cancelled ? " (cancelled)" : "") + ".";
    Json unverified = Json::object();
    std::set<std::string> unverifiedTargets;
    for (const auto& [id, targets] : outcome.unverified) {
        unverified[id] = targets;
        for (const std::string& t : targets) unverifiedTargets.insert(t);
    }
    Json data = {{"run_id", run.run.id},
                 {"counts", gt::countsJson(run.counts)},
                 {"failures", failures},
                 {"unverified", unverified},
                 {"results_file", outcome.resultsFile.empty() ? Json(nullptr) : Json(relative(project, outcome.resultsFile))}};
    if (outcome.cancelled) data["cancelled"] = true;
    if (!outcome.notes.empty()) data["notes"] = outcome.notes;
    std::vector<std::string> next;
    if (!firstCapture.empty()) {
        const fs::path results = gametests::resultsDir(project->getProjectPath());
        const fs::path root = gametests::findRoot(project->getProjectPath());
        const std::string actual = relative(project, results / FileUtils::pathFromUtf8(firstCaptureJson.value("actual", std::string())));
        const std::string golden = relative(project, root / FileUtils::pathFromUtf8(firstCaptureJson.value("golden", std::string())));
        next.push_back("Look: diff_images {a: \"" + actual + "\", b: \"" + golden + "\"}; if the change is intended: run_tests {action: \"accept\", captures: [\"" +
                       firstCapture + "\"]}.");
    }
    // the editor's own target is verified by this run; the others need their players
    unverifiedTargets.erase(gametests::targetName(run.run.target));
    if (!unverifiedTargets.empty()) {
        std::string list;
        for (const std::string& t : unverifiedTargets) list += (list.empty() ? "" : ", ") + t;
        next.push_back("Unverified on " + list + ": run the Development build there with --run-tests, then run_tests {action: \"import\", results_file}.");
    }
    if (!next.empty()) {
        std::string joined;
        for (const std::string& n : next) joined += (joined.empty() ? "" : " ") + n;
        data["next"] = joined;
    }
    const std::string junit = args.value("junit", std::string());
    if (!junit.empty()) {
        std::string written, error;
        if (writeJunit(project, junit, run, written, error)) data["junit"] = written;
        else data["junit_error"] = error;
    }
    ToolResult result = ToolResult::success(text, data);
    // the first failed capture's panel (a | b | diff)
    if (!firstCaptureJson.is_null() && firstCaptureJson.contains("panel")) {
        const fs::path panel = gametests::resultsDir(project->getProjectPath()) / FileUtils::pathFromUtf8(firstCaptureJson.value("panel", std::string()));
        std::ifstream in(panel, std::ios::binary);
        std::ostringstream bytes;
        bytes << in.rdbuf();
        const std::string png = bytes.str();
        if (!png.empty()) result.images.push_back({Base64::encode(reinterpret_cast<const uint8_t*>(png.data()), png.size()), "image/png"});
    }
    return result;
}

gametests::RunRequest requestOf(const Json& args) {
    gametests::RunRequest request;
    request.filter = args.value("filter", std::string());
    request.mode = args.value("mode", std::string("all"));
    request.failedOnly = args.value("failed_only", false);
    request.keepData = args.value("keep_data", false);
    return request;
}

void listTests(const CallPtr& call, Project* project) {
    gametests::RunRequest request = requestOf(call->args());
    gametests::Selection selection = gametests::selectCases(project, request);
    if (selection.error != gametests::SelectError::None) {
        call->fail(selection.message);
        return;
    }
    gametests::Ledger ledger;
    std::string note;
    gametests::readLedger(project->getProjectPath(), ledger, note);
    Json cases = Json::array();
    std::string text;
    std::map<std::string, Json> revisions;
    for (const gt::CaseSpec& spec : selection.cases) {
        if (!revisions.count(spec.file)) revisions[spec.file] = gametests::revisionOf(project, selection.root, spec.file);
        Json item = {{"id", spec.id}, {"file", spec.file}, {"line", spec.line}, {"mode", gt::modeName(spec.mode)}, {"tags", spec.tags}};
        const std::vector<std::string> missing = gametests::unverifiedTargets(ledger, spec.id, selection.settings.targets, revisions[spec.file]);
        if (!missing.empty()) item["unverified"] = missing;
        if (!spec.skip.empty()) item["skip"] = spec.skip;
        cases.push_back(item);
        text += std::string(spec.mode == gt::Mode::Edit ? "edit " : "play ") + spec.id + "  (line " + std::to_string(spec.line) + ")\n";
    }
    text += std::to_string(selection.cases.size()) + " case(s) in " + std::to_string(selection.files.size()) + " file(s).";
    call->ok(text, Json{{"cases", cases}, {"root", relative(project, selection.root)}, {"targets", selection.settings.targets}});
}

// AC10: a Development player of this computer built with Include Tests (build_project, prebuilt),
// run with --run-tests (run_build), its results imported as a device's are
void runDesktop(const CallPtr& call, Project* project, const gametests::RunRequest& request) {
    gametests::Selection selection = gametests::selectCases(project, request);
    if (selection.error != gametests::SelectError::None) {
        call->fail(selection.message);
        return;
    }
    if (request.mode == "edit") {
        call->fail("A player runs Play Mode cases only: run Edit Mode cases on the editor (target: \"editor\").");
        return;
    }
    const Json args = call->args();
    JobPtr job = call->ctx().jobs().create("run_tests", "Run tests on a desktop player");
    job->setProgress(-1.0, "Building the Development player with the tests");
    auto images = std::make_shared<std::vector<ToolImage>>();
    gametests::setIncludeTestsForced(true);
    const Json build = {{"mode", "desktop"}, {"method", "prebuilt"}, {"development", true}, {"wait", true}, {"timeout_seconds", 3600}};
    McpServer::get().runNested(call, "build_project", build, [call, project, job, args, request, images](const ToolResult& built) {
        gametests::setIncludeTestsForced(false);
        if (built.isError) {
            job->fail("build_project: " + built.text);
            return;
        }
        job->setProgress(-1.0, "Running the player with --run-tests");
        Json runArgs = Json::array({"--run-tests"});
        if (!request.filter.empty()) runArgs.insert(runArgs.end(), {"--test-filter", request.filter});
        if (request.keepData) runArgs.push_back("--keep-test-data");
        // no run_build screenshot: the run ends the player before any frame it would name (exit 4)
        const Json run = {{"headless", true}, {"seconds", 300}, {"log_lines", 40}, {"screenshot", false}, {"args", runArgs}};
        McpServer::get().runNested(call, "run_build", run, [project, job, args, images](const ToolResult& ran) {
            const Json data = ran.structured.is_object() ? ran.structured : Json::object();
            // the player names its results file in its log ("[test] results <path>", R25's default place)
            std::string results;
            {
                std::ifstream log(FileUtils::pathFromUtf8(data.value("log_file", std::string())), std::ios::binary);
                std::string line;
                while (std::getline(log, line)) {
                    const size_t at = line.find("[test] results ");
                    if (at != std::string::npos) results = line.substr(at + 15);
                }
                while (!results.empty() && (results.back() == '\r' || results.back() == ' ')) results.pop_back();
            }
            const Json exitCode = data.value("exit_code", Json());
            if (results.empty()) {
                job->fail("The player wrote no results (exit code " + exitCode.dump() + "; 2: no test runner, 9: no tests in the build): " +
                          ran.text.substr(0, 2000));
                return;
            }
            gametests::RunOutcome outcome;
            std::string error;
            if (!gametests::importResults(project, FileUtils::pathFromUtf8(results), outcome, error)) {
                job->fail("The player's results " + results + ": " + error);
                return;
            }
            ToolResult result = outcomeResult(project, outcome, args);
            result.structured["player_exit_code"] = exitCode;
            result.structured["player_results"] = results;
            *images = result.images;
            job->setImages(result.images);
            job->succeed(result.text + " (player exit code " + exitCode.dump() + ")", result.structured);
        });
    });
    const bool wait = args.value("wait", true);
    const double timeout = std::clamp(args.value("timeout_seconds", 600.0), 1.0, 86400.0);
    replyWhenJobDone(call, job, wait, timeout, [images](const McpJob& done) {
        ToolResult result = done.status() == JobStatus::Failed ? ToolResult::error(done.message(), done.result())
                                                               : ToolResult::success(done.message(), done.result());
        result.images = *images;
        return result;
    });
}

void runTests(const CallPtr& call, Project* project) {
    const Json& args = call->args();
    const std::string target = args.value("target", std::string("editor"));
    if (target == "web" || target == "android" || target == "ios") {
        call->fail("This editor runs editor and desktop targets; for " + target +
                   ": build with Include Tests, run the player with --run-tests (or build-and-run-on-device), then import.");
        return;
    }
    gametests::TestRun& run = gametests::TestRun::get();
    if (run.running()) {
        call->fail("A test run is in progress: get_job " + run.jobId + " or cancel_job.");
        return;
    }
    if (project->isAnyScenePlaying() || project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
        call->fail("Stop Play first: tests start their own sessions (play {action: \"stop\"}).");
        return;
    }
    const gametests::RunRequest request = requestOf(args);
    if (target == "desktop") {
        runDesktop(call, project, request);
        return;
    }
    JobPtr job = call->ctx().jobs().create("run_tests", "Run tests");
    job->setProgress(-1.0, "Starting");
    job->setCancelHandler([]() { gametests::TestRun::get().cancel(); });
    std::string error;
    auto images = std::make_shared<std::vector<ToolImage>>();
    const bool started = run.start(call, call->ctx().app(), project, request, [job, project, args, images](const gametests::RunOutcome& outcome) {
        ToolResult result = outcomeResult(project, outcome, args);
        *images = result.images;
        job->setImages(result.images);
        if (outcome.cancelled) job->markCancelled(result.text);
        else job->succeed(result.text, result.structured);
    }, error);
    if (!started) {
        job->fail(error);
        call->fail(error);
        return;
    }
    if (!job->finished()) run.jobId = job->id();
    // progress for get_job while it runs
    struct Progress {
        static void tick(JobPtr job, McpContext* ctx) {
            if (job->finished()) return;
            gametests::TestRun& r = gametests::TestRun::get();
            const size_t planned = std::max<size_t>(1, r.casesPlanned());
            job->setProgress(static_cast<double>(r.casesDone()) / static_cast<double>(planned),
                             std::to_string(r.casesDone()) + " of " + std::to_string(planned) + (r.currentCase().empty() ? "" : ": " + r.currentCase()));
            ctx->app().enqueueMainThreadTask([job, ctx]() { tick(job, ctx); });
        }
    };
    Progress::tick(job, &call->ctx());
    const bool wait = args.value("wait", true);
    const double timeout = std::clamp(args.value("timeout_seconds", 600.0), 1.0, 86400.0);
    replyWhenJobDone(call, job, wait, timeout, [images](const McpJob& done) {
        ToolResult result = done.status() == JobStatus::Failed ? ToolResult::error(done.message(), done.result())
                                                               : ToolResult::success(done.message(), done.result());
        result.images = *images;
        return result;
    });
}

void importTests(const CallPtr& call, Project* project) {
    const std::string given = call->args().value("results_file", std::string());
    if (given.empty()) {
        call->fail("import needs results_file: the results JSON a player wrote (--test-results), its captures/ beside it.");
        return;
    }
    fs::path file;
    std::string error;
    if (!readablePath(project, given, file, error)) {
        call->fail(error);
        return;
    }
    gametests::RunOutcome outcome;
    if (!gametests::importResults(project, file, outcome, error)) {
        call->fail("results_file: " + error);
        return;
    }
    call->reply(outcomeResult(project, outcome, call->args()));
}

void acceptTests(const CallPtr& call, Project* project) {
    const Json& args = call->args();
    std::vector<std::string> captures;
    if (args.contains("captures") && args["captures"].is_array())
        for (const Json& c : args["captures"]) if (c.is_string()) captures.push_back(c.get<std::string>());
    if (captures.empty()) {
        call->fail("accept needs captures: [\"<case id>/<name>\"] from the run's failures, or [\"all\"] for every new or failed capture of the last run.");
        return;
    }
    std::vector<std::string> written;
    std::string error;
    if (!gametests::acceptCaptures(project, captures, args.value("for_target", false), written, error)) {
        call->fail(error);
        return;
    }
    std::string text = "Accepted " + std::to_string(written.size()) + " golden(s) as one undo step:";
    for (const std::string& w : written) text += "\n" + w;
    call->ok(text, Json{{"written", written}});
}

void runTestsTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    if (!project) {
        call->fail("No project is open.");
        return;
    }
    const std::string action = call->args().value("action", std::string("run"));
    if (action == "list") listTests(call, project);
    else if (action == "import") importTests(call, project);
    else if (action == "accept") acceptTests(call, project);
    else if (action == "run") runTests(call, project);
    else call->fail("action is run, list, import or accept.");
}

}

void registerTestTools(McpRegistry& registry) {
    ToolDef tool;
    tool.name = "run_tests";
    tool.title = "Run Tests";
    tool.description =
        "Run the project's tests (Tests/*.test.lua) as a job: Edit Mode and Play Mode cases, each Play case in a fresh session at a "
        "fixed frame rate on clean data. Returns counts, failures (file:line, frame), unverified targets and next. list: cases "
        "without running them. import: a player's results file. accept: make captures the goldens. A filter matching nothing is an error.";
    tool.inputSchema = object({
        {"action", withDefault(stringEnum("", {"run", "list", "import", "accept"}), "run")},
        {"filter", string("Substring of \"<file>#<case>\", or tag:<t>; \",\" = or.")},
        {"mode", withDefault(stringEnum("", {"all", "edit", "play"}), "all")},
        {"target", withDefault(stringEnum("", {"editor", "desktop"}), "editor")},
        {"failed_only", boolean("The last run's failed, error and hung cases.")},
        {"results_file", string("import: a results JSON (its captures/ beside it).")},
        {"captures", array(string(""), "accept: \"<case id>/<name>\", or [\"all\"] (every new or failed capture of the last run).")},
        {"for_target", boolean("accept: the golden of this platform and backend only.")},
        {"junit", string("Also write JUnit XML here.")},
        {"keep_data", boolean("Keep each case's data folder.")},
        {"wait", withDefault(boolean(""), true)},
        {"timeout_seconds", withDefault(number("", 1, 86400), 600)},
    });
    tool.activity = ToolActivity::Control;
    tool.attributesUndo = true;
    tool.group = "testing";
    tool.handler = runTestsTool;
    registry.addTool(std::move(tool));
}

}
