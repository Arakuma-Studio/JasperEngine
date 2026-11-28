// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/TestRun.h"

#include "App.h"
#include "Out.h"
#include "Project.h"
#include "test/EditModeTests.h"
#include "test/TestRunHost.h"
#include "EditorVersion.h"
#include "util/FileUtils.h"
#include "util/ImageDiff.h"
#include "util/ProjectFileWrites.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::gametests {

namespace {

    double testRunNow() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    Json runRevision(Project* project) {
        const Json revision = revisionOf(project, project->getProjectPath(), "project.yaml");
        if (revision.value("vcs", std::string()) == "none") return Json{{"vcs", "none"}, {"id", ""}, {"dirty", false}};
        return revision;
    }

    std::string projectName(Project* project) {
        return FileUtils::pathToUtf8(project->getProjectPath().filename());
    }

    // ledger, unverified targets, the run's document
    void record(Project* project, const fs::path& root, const gametest::Settings& settings, RunOutcome& outcome) {
        const fs::path projectDir = project->getProjectPath();
        Ledger ledger;
        std::string note;
        readLedger(projectDir, ledger, note);
        if (!note.empty()) outcome.notes.push_back(note);
        recordRun(ledger, outcome.result, project, root);
        std::string error;
        if (!writeLedger(projectDir, ledger, error)) outcome.notes.push_back("the ledger was not written: " + error);
        if (!writeRunDocument(projectDir, outcome.result, outcome.resultsFile, error))
            outcome.notes.push_back("the results document was not written: " + error);
        outcome.targets = settings.targets;
        std::map<std::string, Json> revisions;
        for (const gametest::CaseResult& r : outcome.result.cases) {
            if (!revisions.count(r.spec.file)) revisions[r.spec.file] = revisionOf(project, root, r.spec.file);
            std::vector<std::string> missing = unverifiedTargets(ledger, r.spec.id, settings.targets, revisions[r.spec.file]);
            if (!missing.empty()) outcome.unverified[r.spec.id] = std::move(missing);
        }
    }

    bool testRunReadText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream text;
        text << in.rdbuf();
        out = text.str();
        return true;
    }

}

gametest::Target editorTarget(const gametest::Settings& settings) {
    gametest::Target target;
    target.platform = gametest::hostPlatformName();
    target.arch = gametest::hostArchName();
    target.backend = gametest::backendName();
    target.editor = true;
    target.width = settings.width;
    target.height = settings.height;
    return target;
}

std::string nothingMatchedText(const RunRequest& request) {
    return "No test matched \"" + request.filter + "\" (mode " + request.mode + (request.failedOnly ? ", failed_only" : "") +
           "): run_tests {action: \"list\"} shows the case ids.";
}

Selection selectCases(Project* project, const RunRequest& request) {
    Selection out;
    out.root = findRoot(project->getProjectPath());
    if (out.root.empty()) {
        out.error = SelectError::NoRoot;
        out.message = NO_ROOT_TEXT;
        return out;
    }
    if (!loadSettings(out.root, out.settings, out.message)) {
        out.error = SelectError::Settings;
        return out;
    }
    out.files = scan(out.root, out.settings);
    std::set<std::string> failed;
    if (request.failedOnly) {
        gametest::RunResult last;
        if (readLastRun(project->getProjectPath(), last)) {
            for (const gametest::CaseResult& r : last.cases)
                if (r.status == gametest::Status::Failed || r.status == gametest::Status::Error || r.status == gametest::Status::Hung) failed.insert(r.spec.id);
        }
    }
    for (const gametest::CaseSpec& spec : gametest::TestScan::cases(out.files)) {
        if (request.mode == "edit" && spec.mode != gametest::Mode::Edit) continue;
        if (request.mode == "play" && spec.mode != gametest::Mode::Play) continue;
        if (request.failedOnly && !failed.count(spec.id)) continue;
        if (!gametest::matchesFilter(spec, request.filter)) continue;
        out.cases.push_back(spec);
    }
    if (out.cases.empty()) {
        out.error = SelectError::NothingMatched;
        out.message = nothingMatchedText(request);
    }
    return out;
}

TestRun& TestRun::get() {
    static TestRun run;
    return run;
}

size_t TestRun::casesDone() const {
    return editResults.size() + (runner ? runner->finished() : 0);
}

size_t TestRun::casesPlanned() const {
    return active ? selection.cases.size() : 0;
}

std::string TestRun::currentCase() const {
    return runner ? runner->currentCase() : std::string();
}

bool TestRun::start(const std::shared_ptr<mcp::McpCall>& call, App& a, Project* p, const RunRequest& r,
                    std::function<void(const RunOutcome&)> callback, std::string& error) {
    if (active) {
        error = "A test run is in progress" + (jobId.empty() ? std::string(": cancel it in the Test Runner") : ": get_job " + jobId + " or cancel_job");
        return false;
    }
    if (p->isAnyScenePlaying() || p->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
        error = "Stop Play first: tests start their own sessions (play {action: \"stop\"}).";
        return false;
    }
    Selection selected = selectCases(p, r);
    if (selected.error != SelectError::None) {
        error = selected.message;
        return false;
    }
    project = p;
    app = &a;
    request = r;
    selection = std::move(selected);
    done = std::move(callback);
    target = editorTarget(selection.settings);
    runId = gametest::newRunId();
    started = gametest::isoTimeUtc();
    startedSeconds = testRunNow();
    editResults.clear();
    cancelRequested = false;
    active = true;

    // the last run's actual images make way for this run's
    {
        std::error_code ec;
        safewrite::EditorInternal own(resultsDir(project->getProjectPath()));
        fs::remove_all(resultsDir(project->getProjectPath()) / "captures", ec);
    }

    std::vector<gametest::CaseSpec> editCases, playCases;
    for (const gametest::CaseSpec& spec : selection.cases) (spec.mode == gametest::Mode::Edit ? editCases : playCases).push_back(spec);
    if (!editCases.empty()) editResults = runEditModeCases(call, project, selection.root, selection.settings, target, editCases);
    if (playCases.empty()) {
        finish(false);
        return true;
    }
    host = std::make_unique<TestRunHost>(a, project, selection.root, selection.settings, target, request.keepData);
    runner = std::make_unique<gametest::TestRunner>();
    gametest::RunOptions options;
    options.settings = selection.settings;
    options.target = target;
    options.runId = runId;
    options.project = projectName(project);
    options.filter = request.filter;
    options.mode = request.mode;
    runner->start(playCases, *host, options);
    return true;
}

void TestRun::tick() {
    if (!active || !runner) return;
    host->enforce();
    if (!runner->frame()) finish(false);
}

void TestRun::cancel() {
    if (!active) return;
    cancelRequested = true;
    if (runner && runner->isRunning()) {
        runner->cancel();
        // the session ends over the next frames; tick finishes the run
        return;
    }
    finish(true);
}

void TestRun::finish(bool cancelledRun) {
    auto outcome = std::make_unique<RunOutcome>();
    gametest::RunResult& result = outcome->result;
    if (runner) result = runner->result();
    outcome->cancelled = cancelledRun || cancelRequested;
    // the selection's order: Edit Mode and Play Mode cases as listed, computed names after
    std::vector<gametest::CaseResult> ordered;
    std::map<std::string, const gametest::CaseResult*> byId;
    for (const gametest::CaseResult& r : editResults) byId[r.spec.id] = &r;
    for (const gametest::CaseResult& r : result.cases) byId[r.spec.id] = &r;
    std::set<std::string> placed;
    for (const gametest::CaseSpec& spec : selection.cases) {
        auto it = byId.find(spec.id);
        if (it == byId.end() || placed.count(spec.id)) continue;
        ordered.push_back(*it->second);
        placed.insert(spec.id);
    }
    for (const gametest::CaseResult& r : result.cases)
        if (!placed.count(r.spec.id)) ordered.push_back(r);
    result.cases = std::move(ordered);
    result.counts = gametest::countResults(result.cases, 0);
    result.run.id = runId;
    result.run.project = projectName(project);
    result.run.target = target;
    result.run.revision = runRevision(project);
    result.run.editorVersion = JASPERENGINE_EDITOR_VERSION;
    result.run.engineVersion = JASPERENGINE_EDITOR_VERSION;
    result.run.started = started;
    result.run.wallSeconds = testRunNow() - startedSeconds;
    result.run.filter = request.filter;
    result.run.mode = request.mode;
    record(project, selection.root, selection.settings, *outcome);

    if (host) host->restore();
    host.reset();
    runner.reset();
    active = false;
    jobId.clear();
    lastOutcome = std::move(outcome);
    lastRevision++;
    auto callback = std::move(done);
    done = nullptr;
    if (callback) callback(*lastOutcome);
}

bool importResults(Project* project, const fs::path& file, RunOutcome& out, std::string& error) {
    out = RunOutcome();
    const fs::path root = findRoot(project->getProjectPath());
    if (root.empty()) {
        error = NO_ROOT_TEXT;
        return false;
    }
    gametest::Settings settings;
    if (!loadSettings(root, settings, error)) return false;
    std::string text;
    if (!testRunReadText(file, text)) {
        error = "cannot read " + FileUtils::pathToUtf8(file);
        return false;
    }
    const Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        error = FileUtils::pathToUtf8(file) + " is not JSON";
        return false;
    }
    gametest::RunResult run;
    if (!gametest::resultsFromJson(json, run, error)) return false;
    const std::string name = FileUtils::pathToUtf8(project->getProjectPath().filename());
    if (!run.run.project.empty() && run.run.project != name)
        out.notes.push_back("the results are of project \"" + run.run.project + "\", this one is \"" + name + "\"");
    // case ids this project does not list are kept (new), and named
    std::set<std::string> known;
    for (const gametest::CaseSpec& spec : gametest::TestScan::cases(scan(root, settings))) known.insert(spec.id);
    std::vector<std::string> unknown;
    for (const gametest::CaseResult& r : run.cases)
        if (!known.count(r.spec.id)) unknown.push_back(r.spec.id);
    if (!unknown.empty()) out.notes.push_back(std::to_string(unknown.size()) + " case id(s) this project does not list: " + unknown.front());

    // pending captures: compared now, from the PNGs beside the document
    {
        std::error_code ec;
        safewrite::EditorInternal own(resultsDir(project->getProjectPath()));
        fs::remove_all(resultsDir(project->getProjectPath()) / "captures", ec);
    }
    for (gametest::CaseResult& r : run.cases) {
        bool captureFailed = false;
        for (Json& capture : r.captures) {
            if (capture.value("status", std::string()) != "pending") {
                captureFailed = captureFailed || capture.value("status", std::string()) == "failed" || capture.value("status", std::string()) == "new";
                continue;
            }
            const std::string captureName = capture.value("name", std::string());
            const fs::path png = file.parent_path() / FileUtils::pathFromUtf8(capture.value("actual", std::string()));
            texsynth::Image loaded;
            std::string loadError;
            if (!texsynth::loadImage(png, loaded, loadError)) {
                capture["status"] = "failed";
                capture["error"] = "the capture's PNG does not load (" + FileUtils::pathToUtf8(png) + "): " + loadError;
            } else {
                capture = compareCapture(project->getProjectPath(), root, r.spec.id, captureName, toRgba8Image(loaded), run.run.target.platform,
                                         run.run.target.backend, settings.image, Json::object());
            }
            const std::string status = capture.value("status", std::string());
            if (status == "failed" || status == "new") {
                captureFailed = true;
                r.failures.push_back({"capture", "capture " + captureName + ": " + capture.value("error", std::string(status == "new" ? "no golden yet" : "differs")),
                                      r.spec.file, r.spec.line, r.frames});
            }
        }
        if (captureFailed && r.status == gametest::Status::Passed) r.status = gametest::Status::Failed;
    }
    run.counts = gametest::countResults(run.cases, run.counts.excludedEdit);
    out.result = run;
    record(project, root, settings, out);
    return true;
}

bool acceptCaptures(Project* project, const std::vector<std::string>& captures, bool forTarget, std::vector<std::string>& written,
                               std::string& error) {
    const fs::path projectDir = project->getProjectPath();
    const fs::path root = findRoot(projectDir);
    if (root.empty()) {
        error = NO_ROOT_TEXT;
        return false;
    }
    gametest::RunResult last;
    if (!readLastRun(projectDir, last)) {
        error = "No run to accept from: run_tests first (accept takes the captures of the last run).";
        return false;
    }
    const bool all = captures.size() == 1 && captures[0] == "all";
    std::vector<WriteFilesCmd::Item> items;
    std::set<std::string> wanted(captures.begin(), captures.end());
    std::set<std::string> found;
    for (const gametest::CaseResult& r : last.cases) {
        for (const Json& capture : r.captures) {
            const std::string name = capture.value("name", std::string());
            const std::string key = r.spec.id + "/" + name;
            const std::string status = capture.value("status", std::string());
            if (all ? (status != "new" && status != "failed") : !wanted.count(key)) continue;
            const fs::path actual = captureActualPath(projectDir, r.spec.id, name);
            std::string bytes;
            if (!testRunReadText(actual, bytes)) {
                if (!all) {
                    error = "No actual image for " + key + " (" + FileUtils::pathToUtf8(actual) + "): run the case again.";
                    return false;
                }
                continue;
            }
            found.insert(key);
            const fs::path golden = acceptPath(root, r.spec.id, name, last.run.target.platform, last.run.target.backend, forTarget);
            items.push_back({golden, bytes});
            written.push_back(FileUtils::pathToGenericUtf8(golden.lexically_relative(projectDir)));
        }
    }
    if (!all) {
        for (const std::string& key : wanted) {
            if (!found.count(key)) {
                error = "No capture \"" + key + "\" in the last run: captures are \"<case id>/<name>\" (the failures' captures list them).";
                return false;
            }
        }
    }
    if (items.empty()) {
        error = "No new or failed capture in the last run to accept.";
        return false;
    }
    return ProjectFileWrites::writeUndoable(project, std::move(items), error, nullptr, nullptr, {}, false,
                                            written.size() == 1 ? "Accept Image " + fs::path(written[0]).filename().string()
                                                                : "Accept " + std::to_string(written.size()) + " Images");
}

}  // namespace jasperengine::editor::gametests
