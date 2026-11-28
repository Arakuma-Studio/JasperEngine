// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/GameTests.h"
#include "test/TestOptions.h"

#include "Project.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "mcp/McpValues.h"
#include "util/FileUtils.h"
#include "util/ImageDiff.h"
#include "util/SafeWrite.h"
#include "util/Sha256.h"
#include "vcs/VcsService.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::gametests {

namespace {

    constexpr size_t KEPT_RUNS = 20;
    constexpr const char* LEDGER_FORMAT = "jasperengine-test-ledger";

    bool gameTestsReadText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream text;
        text << in.rdbuf();
        out = text.str();
        return true;
    }

    std::string caseDir(const std::string& caseId) {
        return gametest::caseIdPath(caseId);
    }

}

fs::path findRoot(const fs::path& projectDir) {
    std::error_code ec;
    for (const char* name : ROOT_NAMES) {
        const fs::path dir = projectDir / name;
        if (fs::is_regular_file(dir / MARKER, ec)) return dir;
    }
    return {};
}

bool isTestRoot(const fs::path& projectDir, const fs::path& dir) {
    const fs::path root = findRoot(projectDir);
    if (root.empty()) return false;
    std::error_code ec;
    return fs::equivalent(root, dir, ec);
}

bool loadSettings(const fs::path& root, gametest::Settings& out, std::string& error) {
    out = gametest::Settings();
    std::string text;
    if (!gameTestsReadText(root / MARKER, text)) return true;
    Json json;
    try {
        const YAML::Node node = YAML::Load(text);
        if (!node.IsNull() && !node.IsMap()) {
            error = "tests.yaml is not a map of settings (version: 1, fixed_fps: 60...)";
            return false;
        }
        json = node.IsNull() ? Json::object() : mcp::McpValues::yamlToJson(node);
    } catch (const std::exception& e) {
        error = std::string("tests.yaml: ") + e.what();
        return false;
    }
    if (!gametest::Settings::fromJson(json, out, error)) return false;
    static std::set<std::string> warned;
    for (const std::string& key : out.unknownKeys) {
        if (warned.insert(key).second) Out::warning("tests.yaml: unknown key \"%s\" (kept, not used by this editor)", key.c_str());
    }
    return true;
}

std::vector<gametest::ScannedFile> scan(const fs::path& root, const gametest::Settings& settings) {
    return gametest::TestScan::scan(root, settings);
}

std::string folderToCreate(const fs::path& projectDir, std::vector<std::string>& blocking) {
    blocking.clear();
    const fs::path tests = projectDir / "Tests";
    std::error_code ec;
    if (!fs::is_directory(tests, ec) || fs::is_regular_file(tests / MARKER, ec)) return "Tests";
    for (fs::recursive_directory_iterator it(tests, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (blocking.size() < 5) blocking.push_back(FileUtils::pathToGenericUtf8(it->path().lexically_relative(projectDir)));
        else break;
    }
    return blocking.empty() ? "Tests" : "GameTests";
}

std::string defaultTestsYaml() {
    return "# The project's tests (docs/game-tests.md). Every key is optional.\n"
           "version: 1\n"
           "targets: [editor]        # editor, windows, linux, macos, android, ios, web\n"
           "fixed_fps: 60\n"
           "resolution: 1280x720\n"
           "seed: 1\n"
           "timeout_frames: 18000\n"
           "wait_frames: 600\n"
           "hang_seconds: 600\n"
           "image: {tolerance: 8, max_fraction: 0.001, max_diff: 48, max_mean: 0.25}\n"
           "console_errors: fail     # fail | warn\n"
           "divergence: fail         # fail | warn\n";
}

std::string testScriptTemplate(const std::string& startScene) {
    const std::string scene = startScene.empty() ? std::string() : "    scene = \"" + startScene + "\",\n";
    return "-- A test file: every Test.case is one scenario (docs/game-tests.md).\n"
           "-- Play Mode cases run in a fresh Play session at a fixed frame rate on clean data;\n"
           "-- Edit Mode cases run in the editor without playing and may only read the project.\n"
           "\n"
           "Test.config{\n" +
           scene +
           "    tags = {\"smoke\"},\n"
           "}\n"
           "\n"
           "Test.case(\"The start scene plays for a second without errors\", function(t)\n"
           "    -- waits count game frames and game seconds, never the wall clock\n"
           "    Test.waitGameSeconds(1)\n"
           "    Test.check(t.frame >= 60, \"the game ran its frames\")\n"
           "    -- Test.waitUntil(function() return SceneManager.getActiveScene().name == \"Level1\" end,\n"
           "    --                {gameSeconds = 5, message = \"Level1 never loaded\"})\n"
           "    -- Test.capture(\"start\")   -- compared with Tests/Golden/<case>/start.png\n"
           "end)\n"
           "\n"
           "Test.case(\"Every scene in the build has a camera\", {mode = \"edit\"}, function(t)\n"
           "    for _, s in ipairs(Editor.call(\"list_scenes\", {}).data.scenes or {}) do\n"
           "        Test.check(s.name ~= nil, \"a scene without a name\")\n"
           "    end\n"
           "end)\n";
}

fs::path findGolden(const fs::path& root, const std::string& caseId, const std::string& name, const std::string& platform,
                               const std::string& backend) {
    const fs::path dir = root / "Golden" / caseDir(caseId);
    std::error_code ec;
    for (const std::string& file : {name + "." + platform + "-" + backend + ".png", name + "." + backend + ".png", name + ".png"}) {
        if (fs::is_regular_file(dir / file, ec)) return dir / file;
    }
    return {};
}

fs::path acceptPath(const fs::path& root, const std::string& caseId, const std::string& name, const std::string& platform,
                               const std::string& backend, bool forTarget) {
    const fs::path dir = root / "Golden" / caseDir(caseId);
    if (forTarget) return dir / (name + "." + platform + "-" + backend + ".png");
    const fs::path existing = findGolden(root, caseId, name, platform, backend);
    return existing.empty() ? dir / (name + ".png") : existing;
}

fs::path resultsDir(const fs::path& projectDir) {
    return projectDir / ".jasperengine" / "test-results";
}

namespace {

    bool writePng(const fs::path& path, const texsynth::Image& image, std::string& error) {
        std::vector<uint8_t> png;
        if (!texsynth::encodePng(image, png)) {
            error = "cannot encode " + FileUtils::pathToUtf8(path);
            return false;
        }
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        const io::WriteResult written = io::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(png.data()), png.size()), options);
        if (!written.ok()) {
            error = written.message;
            return false;
        }
        return true;
    }

}

auto compareCapture(const fs::path& projectDir, const fs::path& root, const std::string& caseId, const std::string& name,
                               const texsynth::Image& actual, const std::string& platform, const std::string& backend,
                               const gametest::ImageTolerance& tolerance, const Json& options) -> Json {
    Json capture = {{"name", name}};
    const fs::path actualPath = captureActualPath(projectDir, caseId, name);
    std::string error;
    safewrite::EditorInternal own(resultsDir(projectDir));
    if (!writePng(actualPath, actual, error)) {
        capture["status"] = "failed";
        capture["error"] = error;
        return capture;
    }
    capture["actual"] = FileUtils::pathToGenericUtf8(actualPath.lexically_relative(resultsDir(projectDir)));
    capture["size"] = Json::array({actual.width, actual.height});
    const fs::path golden = findGolden(root, caseId, name, platform, backend);
    if (golden.empty()) {
        capture["status"] = "new";
        capture["golden"] = FileUtils::pathToGenericUtf8(acceptPath(root, caseId, name, platform, backend, false).lexically_relative(root));
        return capture;
    }
    capture["golden"] = FileUtils::pathToGenericUtf8(golden.lexically_relative(root));
    texsynth::Image loaded;
    if (!texsynth::loadImage(golden, loaded, error)) {
        capture["status"] = "failed";
        capture["error"] = "the golden does not load: " + error;
        return capture;
    }
    const texsynth::Image expected = toRgba8Image(loaded);
    if (expected.width != actual.width || expected.height != actual.height) {
        capture["status"] = "failed";
        capture["error"] = "capture is " + std::to_string(actual.width) + "x" + std::to_string(actual.height) + ", golden is " +
                           std::to_string(expected.width) + "x" + std::to_string(expected.height) + " (resolution changed?)";
        return capture;
    }
    ImageDiffOptions diff;
    diff.premultiplied = true;
    diff.tolerance = options.value("tolerance", tolerance.tolerance);
    diff.maxFraction = options.value("maxFraction", tolerance.maxFraction);
    diff.maxDiff = options.value("maxDiff", tolerance.maxDiff);
    diff.maxMean = options.value("maxMean", tolerance.maxMean);
    if (options.contains("region") && options["region"].is_object()) {
        const Json& r = options["region"];
        diff.crop = ImageDiffRect{static_cast<int>(r.value("x", 0.0)), static_cast<int>(r.value("y", 0.0)), static_cast<int>(r.value("w", 0.0)),
                                  static_cast<int>(r.value("h", 0.0))};
    }
    const ImageDiffResult result = diffImages(actual, expected, diff);
    capture["status"] = result.ok ? "passed" : "failed";
    capture["diff"] = {{"max", result.max},
                       {"mean", std::round(result.mean * 10000.0) / 10000.0},
                       {"fraction_over", result.fractionOver},
                       {"bbox", result.bbox.empty() ? Json(nullptr) : Json::array({result.bbox.x, result.bbox.y, result.bbox.width, result.bbox.height})}};
    if (!result.ok) {
        char text[160];
        std::snprintf(text, sizeof(text), "differs from the golden: max %d, mean %.3f, %.3f %% of pixels over the tolerance", result.max, result.mean,
                      result.fractionOver * 100.0);
        capture["error"] = text;
        const fs::path panelPath = actualPath.parent_path() / (name + ".diff.png");
        if (writePng(panelPath, diffPanel(actual, expected, diff), error))
            capture["panel"] = FileUtils::pathToGenericUtf8(panelPath.lexically_relative(resultsDir(projectDir)));
    }
    return capture;
}

fs::path captureActualPath(const fs::path& projectDir, const std::string& caseId, const std::string& name) {
    return resultsDir(projectDir) / "captures" / caseDir(caseId) / (name + ".png");
}

std::vector<std::string> listSnapshots(const fs::path& projectDir) {
    std::vector<std::string> out;
    const fs::path root = findRoot(projectDir);
    if (root.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(root / "Data", ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = FileUtils::pathToUtf8(it->path().filename());
        if (it->is_directory(ec) && gametest::isSnapshotName(name)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool copyTree(const fs::path& from, const fs::path& to, std::string& error, uint64_t* bytes) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        error = "cannot create " + FileUtils::pathToUtf8(to) + ": " + ec.message();
        return false;
    }
    uint64_t total = 0;
    if (!fs::is_directory(from, ec)) {
        if (bytes) *bytes = 0;
        return true;
    }
    for (fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path target = to / it->path().lexically_relative(from);
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(target.parent_path(), ec);
            // a new file in an empty folder: nothing is overwritten
            fs::copy_file(it->path(), target, ec);
            total += it->file_size(ec);
        }
        if (ec) {
            error = "cannot copy " + FileUtils::pathToUtf8(it->path()) + ": " + ec.message();
            return false;
        }
    }
    if (bytes) *bytes = total;
    return true;
}

uint64_t treeBytes(const fs::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

namespace {
    bool includeTestsForcedFlag = false;
}

void setIncludeTestsForced(bool forced) {
    includeTestsForcedFlag = forced;
}

bool includeTestsForced() {
    return includeTestsForcedFlag;
}

bool exportTests(const fs::path& projectDir, bool development, bool includeTests, const fs::path& contentRoot, std::string& warning,
                            std::string& error) {
    warning.clear();
    if (!includeTests) return true;
    if (!development) {
        warning = INCLUDE_TESTS_RELEASE_WARNING;
        return true;
    }
    const fs::path root = findRoot(projectDir);
    if (root.empty()) {
        warning = std::string("Include Tests: ") + NO_ROOT_TEXT;
        return true;
    }
    gametest::Settings settings;
    if (!loadSettings(root, settings, error)) return false;
    const fs::path out = contentRoot / "tests";
    std::error_code ec;
    fs::remove_all(out, ec);
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path relative = it->path().lexically_relative(root);
        const std::string first = relative.begin() != relative.end() ? relative.begin()->generic_string() : std::string();
        // goldens stay with the project: imports compare the captures
        if (it.depth() == 0 && it->is_directory(ec) && first == "Golden") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) {
            fs::create_directories(out / relative, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories((out / relative).parent_path(), ec);
            fs::copy_file(it->path(), out / relative, ec);
        }
        if (ec) {
            error = "Include Tests: cannot copy " + FileUtils::pathToGenericUtf8(relative) + ": " + ec.message();
            return false;
        }
    }
    // the snapshots' files: a player copies them by name (a content pack has no folders to list)
    Json data = Json::object();
    for (const std::string& snapshot : listSnapshots(projectDir)) {
        Json files = Json::array();
        const fs::path dir = root / "Data" / snapshot;
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back(FileUtils::pathToGenericUtf8(it->path().lexically_relative(dir)));
        data[snapshot] = files;
    }
    Json index = {{"settings", settings.toJson()}, {"files", gametest::TestScan::listFiles(root)}, {"data", data}};
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult written = io::writeFileAtomic(out / "index.json", index.dump(1), options);
    if (!written.ok()) {
        error = "Include Tests: " + written.message;
        return false;
    }
    return true;
}

auto revisionOf(Project* project, const fs::path& root, const std::string& file) -> Json {
    if (project) {
        const vcs::VcsService& vcs = project->getVcs();
        if (vcs.active() && vcs.snapshot() && !vcs.snapshot()->branch.headOid.empty()) {
            const std::string oid = vcs.snapshot()->branch.headOid;
            const bool dirty = !vcs.snapshot()->entries.empty();
            return Json{{"vcs", vcs.caps().provider.empty() ? std::string("vcs") : vcs.caps().provider}, {"id", oid.substr(0, 12)}, {"dirty", dirty}};
        }
    }
    std::string hash;
    sha256File(root / fs::path(file), hash);
    return Json{{"vcs", "none"}, {"id", hash}, {"dirty", false}};
}

std::string targetName(const gametest::Target& target) {
    return target.editor ? "editor" : target.platform;
}

bool readLedger(const fs::path& projectDir, Ledger& out, std::string& note) {
    out = Ledger();
    const fs::path file = resultsDir(projectDir) / "ledger.json";
    std::string text;
    if (!gameTestsReadText(file, text)) return true;
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object()) {
        note = "the ledger was unreadable and starts again";
        return true;
    }
    const int version = json.value("version", 0);
    if (version > Ledger::VERSION) {
        // made by a newer editor (R51): kept aside, a new one starts
        std::error_code ec;
        safewrite::EditorInternal own(resultsDir(projectDir));
        fs::rename(file, resultsDir(projectDir) / ("ledger.v" + std::to_string(version) + ".json"), ec);
        note = "ledger.json version " + std::to_string(version) + " was made by a newer editor: moved to ledger.v" +
               std::to_string(version) + ".json, a new one starts";
        return true;
    }
    if (json.contains("cases") && json["cases"].is_object()) out.cases = json["cases"];
    return true;
}

bool writeLedger(const fs::path& projectDir, const Ledger& ledger, std::string& error) {
    const Json json = {{"format", LEDGER_FORMAT}, {"version", Ledger::VERSION}, {"cases", ledger.cases}};
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    safewrite::EditorInternal own(resultsDir(projectDir));
    const io::WriteResult written = io::writeFileAtomic(resultsDir(projectDir) / "ledger.json", json.dump(1), options);
    if (!written.ok()) {
        error = written.message;
        return false;
    }
    return true;
}

void recordRun(Ledger& ledger, const gametest::RunResult& run, Project* project, const fs::path& root) {
    const std::string target = targetName(run.run.target);
    const std::string now = gametest::isoTimeUtc();
    std::map<std::string, Json> revisions;
    for (const gametest::CaseResult& r : run.cases) {
        if (!revisions.count(r.spec.file)) revisions[r.spec.file] = revisionOf(project, root, r.spec.file);
        Json& entry = ledger.cases[r.spec.id][target];
        if (!entry.is_object()) entry = Json::object();
        entry["status"] = gametest::statusName(r.status);
        entry["last_run"] = run.run.started.empty() ? now : run.run.started;
        entry["backend"] = run.run.target.backend;
        entry["editor_version"] = run.run.editorVersion;
        entry["revision"] = revisions[r.spec.file];
        if (r.status == gametest::Status::Passed) {
            entry["last_pass"] = entry["last_run"];
            entry["pass_revision"] = revisions[r.spec.file];
        }
    }
}

std::vector<std::string> unverifiedTargets(const Ledger& ledger, const std::string& caseId, const std::vector<std::string>& targets,
                                                      const Json& currentRevision) {
    std::vector<std::string> out;
    const Json* entries = ledger.cases.contains(caseId) ? &ledger.cases[caseId] : nullptr;
    for (const std::string& target : targets) {
        bool verified = false;
        if (entries && entries->contains(target)) {
            const Json& entry = (*entries)[target];
            const Json pass = entry.value("pass_revision", Json());
            verified = pass.is_object() && !currentRevision.value("dirty", false) && pass.value("id", std::string()) == currentRevision.value("id", std::string()) &&
                       !pass.value("dirty", false);
        }
        if (!verified) out.push_back(target);
    }
    return out;
}

bool writeRunDocument(const fs::path& projectDir, const gametest::RunResult& run, fs::path& written, std::string& error) {
    const fs::path runs = resultsDir(projectDir) / "runs";
    written = runs / (run.run.id + ".json");
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    safewrite::EditorInternal own(resultsDir(projectDir));
    const io::WriteResult result = io::writeFileAtomic(written, gametest::resultsJson(run).dump(1), options);
    if (!result.ok()) {
        error = result.message;
        return false;
    }
    // the last 20 runs (their ids sort by time)
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(runs, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->path().extension() == ".json") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i + KEPT_RUNS < files.size(); i++) fs::remove(files[i], ec);
    // the pointer failing leaves the previous run as the last one: failed_only reads that
    io::writeFileAtomic(resultsDir(projectDir) / "last-run.txt", run.run.id + "\n", options);
    return true;
}

bool readLastRun(const fs::path& projectDir, gametest::RunResult& out) {
    std::string id;
    if (!gameTestsReadText(resultsDir(projectDir) / "last-run.txt", id)) return false;
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
    std::string text;
    if (id.empty() || !gameTestsReadText(resultsDir(projectDir) / "runs" / (id + ".json"), text)) return false;
    const Json json = Json::parse(text, nullptr, false);
    std::string error;
    return gametest::resultsFromJson(json, out, error);
}

}  // namespace jasperengine::editor::gametests
