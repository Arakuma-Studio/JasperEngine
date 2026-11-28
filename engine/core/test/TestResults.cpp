// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/TestResults.h"

#include "Engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <random>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

using namespace jasperengine;

namespace jasperengine::gametest {

namespace {

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string trim(const std::string& text) {
        size_t begin = 0, end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return text.substr(begin, end - begin);
    }

    double round3(double value) {
        return std::round(value * 1000.0) / 1000.0;
    }

    bool readStatus(const std::string& name, Status& out) {
        static const std::pair<const char*, Status> names[] = {{"passed", Status::Passed}, {"failed", Status::Failed},
                                                              {"skipped", Status::Skipped}, {"error", Status::Error},
                                                              {"hung", Status::Hung}};
        for (const auto& [text, status] : names) {
            if (name == text) {
                out = status;
                return true;
            }
        }
        return false;
    }

    std::string xmlEscape(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&apos;"; break;
                default:
                    // XML 1.0 has no other control characters than tab and the line ends
                    if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') out += ' ';
                    else out += c;
            }
        }
        return out;
    }

}

const char* modeName(Mode mode) {
    return mode == Mode::Edit ? "edit" : "play";
}

const char* statusName(Status status) {
    switch (status) {
        case Status::Passed: return "passed";
        case Status::Failed: return "failed";
        case Status::Skipped: return "skipped";
        case Status::Error: return "error";
        case Status::Hung: return "hung";
    }
    return "error";
}

Json Settings::toJson() const {
    return Json{{"version", version},
                {"targets", targets},
                {"fixed_fps", fixedFps},
                {"resolution", std::to_string(width) + "x" + std::to_string(height)},
                {"seed", seed},
                {"timeout_frames", timeoutFrames},
                {"wait_frames", waitFrames},
                {"hang_seconds", hangSeconds},
                {"image", {{"tolerance", image.tolerance}, {"max_fraction", image.maxFraction}, {"max_diff", image.maxDiff},
                           {"max_mean", image.maxMean}}},
                {"console_errors", consoleErrorsFail ? "fail" : "warn"},
                {"divergence", divergenceFail ? "fail" : "warn"}};
}

bool Settings::fromJson(const Json& json, Settings& out, std::string& error) {
    out = Settings();
    if (json.is_null()) return true;
    if (!json.is_object()) {
        error = "tests.yaml is not a map of settings (version: 1, fixed_fps: 60...)";
        return false;
    }
    static const char* const KNOWN[] = {"version", "targets", "fixed_fps", "resolution", "seed", "timeout_frames", "wait_frames",
                                        "hang_seconds", "image", "console_errors", "divergence"};
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (std::find_if(std::begin(KNOWN), std::end(KNOWN), [&](const char* key) { return it.key() == key; }) == std::end(KNOWN))
            out.unknownKeys.push_back(it.key());
    }
    auto integerOf = [&](const char* key, int64_t low, int64_t high, int64_t& value) {
        if (!json.contains(key)) return true;
        const Json& v = json[key];
        if (!v.is_number_integer() || v.get<int64_t>() < low || v.get<int64_t>() > high) {
            error = std::string("tests.yaml ") + key + " must be a whole number from " + std::to_string(low) + " to " + std::to_string(high);
            return false;
        }
        value = v.get<int64_t>();
        return true;
    };
    int64_t version = VERSION;
    if (!integerOf("version", 1, 1000000, version)) return false;
    if (version > VERSION) {
        error = "tests.yaml version " + std::to_string(version) + " needs a newer editor";
        return false;
    }
    out.version = static_cast<int>(version);
    if (json.contains("targets")) {
        static const char* const TARGETS[] = {"editor", "windows", "linux", "macos", "android", "ios", "web"};
        const Json& targets = json["targets"];
        if (!targets.is_array()) {
            error = "tests.yaml targets is a list: [editor, android, web]";
            return false;
        }
        out.targets.clear();
        for (const Json& t : targets) {
            const std::string name = t.is_string() ? t.get<std::string>() : std::string();
            if (std::find_if(std::begin(TARGETS), std::end(TARGETS), [&](const char* k) { return name == k; }) == std::end(TARGETS)) {
                error = "tests.yaml targets: unknown target \"" + name + "\" (editor, windows, linux, macos, android, ios, web)";
                return false;
            }
            out.targets.push_back(name);
        }
    }
    int64_t fps = out.fixedFps;
    if (!integerOf("fixed_fps", 1, 1000, fps)) return false;
    out.fixedFps = static_cast<int>(fps);
    if (json.contains("resolution")) {
        int w = 0, h = 0;
        const std::string text = json["resolution"].is_string() ? json["resolution"].get<std::string>() : std::string();
        if (std::sscanf(text.c_str(), "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 16384 || h > 16384) {
            error = "tests.yaml resolution is WIDTHxHEIGHT, for example 1280x720 (got \"" + text + "\")";
            return false;
        }
        out.width = w;
        out.height = h;
    }
    int64_t seed = static_cast<int64_t>(out.seed);
    if (!integerOf("seed", 0, INT64_MAX, seed)) return false;
    out.seed = static_cast<uint64_t>(seed);
    if (!integerOf("timeout_frames", 1, INT64_MAX, out.timeoutFrames)) return false;
    if (!integerOf("wait_frames", 1, INT64_MAX, out.waitFrames)) return false;
    if (json.contains("hang_seconds")) {
        if (!json["hang_seconds"].is_number() || json["hang_seconds"].get<double>() <= 0.0) {
            error = "tests.yaml hang_seconds must be a number of seconds above 0";
            return false;
        }
        out.hangSeconds = json["hang_seconds"].get<double>();
    }
    if (json.contains("image")) {
        const Json& image = json["image"];
        if (!image.is_object()) {
            error = "tests.yaml image is a map: {tolerance: 8, max_fraction: 0.001, max_diff: 48, max_mean: 0.25}";
            return false;
        }
        out.image.tolerance = std::clamp(image.value("tolerance", out.image.tolerance), 0, 255);
        out.image.maxFraction = std::clamp(image.value("max_fraction", out.image.maxFraction), 0.0, 1.0);
        out.image.maxDiff = std::clamp(image.value("max_diff", out.image.maxDiff), 0, 255);
        out.image.maxMean = std::clamp(image.value("max_mean", out.image.maxMean), 0.0, 255.0);
    }
    for (const char* key : {"console_errors", "divergence"}) {
        if (!json.contains(key)) continue;
        const std::string value = json[key].is_string() ? json[key].get<std::string>() : std::string();
        if (value != "fail" && value != "warn") {
            error = std::string("tests.yaml ") + key + " is fail or warn";
            return false;
        }
        (std::string(key) == "console_errors" ? out.consoleErrorsFail : out.divergenceFail) = value == "fail";
    }
    return true;
}

Json caseJson(const CaseResult& r, size_t maxConsole) {
    Json failures = Json::array();
    for (const Failure& f : r.failures)
        failures.push_back({{"kind", f.kind}, {"message", f.message}, {"file", f.file}, {"line", f.line}, {"frame", f.frame}});
    Json console = Json::array();
    for (const ConsoleLine& line : r.console) {
        if (maxConsole > 0 && console.size() >= maxConsole) break;
        Json entry = {{"level", line.level}, {"message", line.message}};
        if (!line.file.empty()) entry["source"] = {{"file", line.file}, {"line", line.line}};
        console.push_back(std::move(entry));
    }
    return Json{{"id", r.spec.id},
                {"file", r.spec.file},
                {"line", r.spec.line},
                {"name", r.spec.name},
                {"mode", modeName(r.spec.mode)},
                {"status", statusName(r.status)},
                {"tags", r.spec.tags},
                {"frames", r.frames},
                {"game_seconds", round3(r.gameSeconds)},
                {"wall_ms", std::round(r.wallMs)},
                {"skip_reason", r.skipReason},
                {"failures", failures},
                {"captures", r.captures},
                {"console", console},
                {"log", r.log}};
}

Json countsJson(const Counts& c) {
    return Json{{"total", c.total},   {"passed", c.passed}, {"failed", c.failed},           {"skipped", c.skipped},
                {"error", c.error},   {"hung", c.hung},     {"excluded_edit", c.excludedEdit}};
}

Json resultsJson(const RunResult& result) {
    const RunInfo& run = result.run;
    Json cases = Json::array();
    for (const CaseResult& r : result.cases) cases.push_back(caseJson(r));
    return Json{{"format", RESULTS_FORMAT},
                {"version", RunResult::VERSION},
                {"run", {{"id", run.id},
                         {"project", run.project},
                         {"target", {{"platform", run.target.platform},
                                     {"arch", run.target.arch},
                                     {"backend", run.target.backend},
                                     {"editor", run.target.editor},
                                     {"device", run.target.device}}},
                         {"revision", run.revision},
                         {"editor_version", run.editorVersion},
                         {"engine_version", run.engineVersion},
                         {"started", run.started},
                         {"wall_seconds", round3(run.wallSeconds)},
                         {"filter", run.filter},
                         {"mode", run.mode}}},
                {"counts", countsJson(result.counts)},
                {"cases", cases}};
}

bool caseFromJson(const Json& j, CaseResult& out, std::string& error) {
    out = CaseResult();
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string() || !j.contains("status")) {
        error = "a case without id or status";
        return false;
    }
    out.spec.id = j["id"].get<std::string>();
    out.spec.file = j.value("file", std::string());
    out.spec.name = j.value("name", std::string());
    out.spec.line = j.value("line", 0);
    out.spec.mode = j.value("mode", std::string("play")) == "edit" ? Mode::Edit : Mode::Play;
    if (j.contains("tags") && j["tags"].is_array())
        for (const Json& t : j["tags"]) if (t.is_string()) out.spec.tags.push_back(t.get<std::string>());
    if (!j["status"].is_string() || !readStatus(j["status"].get<std::string>(), out.status)) {
        error = "case " + out.spec.id + ": unknown status";
        return false;
    }
    out.frames = j.value("frames", int64_t(0));
    out.gameSeconds = j.value("game_seconds", 0.0);
    out.wallMs = j.value("wall_ms", 0.0);
    out.skipReason = j.value("skip_reason", std::string());
    if (j.contains("failures") && j["failures"].is_array()) {
        for (const Json& f : j["failures"]) {
            if (!f.is_object()) continue;
            out.failures.push_back({f.value("kind", std::string()), f.value("message", std::string()), f.value("file", std::string()),
                                    f.value("line", 0), f.value("frame", int64_t(0))});
        }
    }
    if (j.contains("captures") && j["captures"].is_array()) out.captures = j["captures"];
    if (j.contains("console") && j["console"].is_array()) {
        for (const Json& c : j["console"]) {
            if (!c.is_object()) continue;
            ConsoleLine line{c.value("level", std::string()), c.value("message", std::string()), std::string(), 0};
            if (c.contains("source") && c["source"].is_object()) {
                line.file = c["source"].value("file", std::string());
                line.line = c["source"].value("line", 0);
            }
            out.console.push_back(std::move(line));
        }
    }
    if (j.contains("log") && j["log"].is_array())
        for (const Json& l : j["log"]) if (l.is_string()) out.log.push_back(l.get<std::string>());
    return true;
}

bool resultsFromJson(const Json& json, RunResult& out, std::string& error) {
    out = RunResult();
    if (!json.is_object() || json.value("format", std::string()) != RESULTS_FORMAT) {
        error = "not a test results document (format \"" + std::string(RESULTS_FORMAT) + "\")";
        return false;
    }
    const int version = json.value("version", 0);
    if (version > RunResult::VERSION) {
        error = "results version " + std::to_string(version) + " was made by a newer editor";
        return false;
    }
    if (version < 1) {
        error = "results without a version";
        return false;
    }
    const Json run = json.value("run", Json::object());
    out.run.id = run.value("id", std::string());
    out.run.project = run.value("project", std::string());
    const Json target = run.value("target", Json::object());
    out.run.target.platform = target.value("platform", std::string());
    out.run.target.arch = target.value("arch", std::string());
    out.run.target.backend = target.value("backend", std::string());
    out.run.target.editor = target.value("editor", false);
    out.run.target.device = target.value("device", std::string());
    out.run.revision = run.value("revision", Json::object());
    out.run.editorVersion = run.value("editor_version", std::string());
    out.run.engineVersion = run.value("engine_version", std::string());
    out.run.started = run.value("started", std::string());
    out.run.wallSeconds = run.value("wall_seconds", 0.0);
    out.run.filter = run.value("filter", std::string());
    out.run.mode = run.value("mode", std::string("all"));
    if (!json.contains("cases") || !json["cases"].is_array()) {
        error = "results without cases";
        return false;
    }
    for (const Json& c : json["cases"]) {
        CaseResult r;
        if (!caseFromJson(c, r, error)) return false;
        out.cases.push_back(std::move(r));
    }
    out.counts = countResults(out.cases, json.value("counts", Json::object()).value("excluded_edit", 0));
    return true;
}

Counts countResults(const std::vector<CaseResult>& cases, int excludedEdit) {
    Counts c;
    c.excludedEdit = excludedEdit;
    for (const CaseResult& r : cases) {
        c.total++;
        switch (r.status) {
            case Status::Passed: c.passed++; break;
            case Status::Failed: c.failed++; break;
            case Status::Skipped: c.skipped++; break;
            case Status::Error: c.error++; break;
            case Status::Hung: c.hung++; break;
        }
    }
    return c;
}

std::string junitXml(const RunResult& result) {
    // one suite per file, in the order the files first appear
    std::vector<std::string> files;
    std::map<std::string, std::vector<const CaseResult*>> byFile;
    for (const CaseResult& r : result.cases) {
        if (!byFile.count(r.spec.file)) files.push_back(r.spec.file);
        byFile[r.spec.file].push_back(&r);
    }
    char number[64];
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    const Counts& total = result.counts;
    std::snprintf(number, sizeof(number), "%.3f", result.run.wallSeconds);
    xml += "<testsuites name=\"" + xmlEscape(result.run.project) + "\" tests=\"" + std::to_string(total.total) + "\" failures=\"" +
           std::to_string(total.failed) + "\" errors=\"" + std::to_string(total.error + total.hung) + "\" skipped=\"" +
           std::to_string(total.skipped) + "\" time=\"" + number + "\">\n";
    for (const std::string& file : files) {
        const std::vector<const CaseResult*>& cases = byFile[file];
        Counts c;
        double seconds = 0.0;
        for (const CaseResult* r : cases) {
            seconds += r->wallMs / 1000.0;
            c.total++;
            if (r->status == Status::Failed) c.failed++;
            else if (r->status == Status::Error || r->status == Status::Hung) c.error++;
            else if (r->status == Status::Skipped) c.skipped++;
        }
        std::snprintf(number, sizeof(number), "%.3f", seconds);
        xml += "  <testsuite name=\"" + xmlEscape(file) + "\" tests=\"" + std::to_string(c.total) + "\" failures=\"" +
               std::to_string(c.failed) + "\" errors=\"" + std::to_string(c.error) + "\" skipped=\"" + std::to_string(c.skipped) +
               "\" time=\"" + number + "\">\n";
        for (const CaseResult* r : cases) {
            std::snprintf(number, sizeof(number), "%.3f", r->wallMs / 1000.0);
            xml += "    <testcase classname=\"" + xmlEscape(file) + "\" name=\"" + xmlEscape(r->spec.name) + "\" time=\"" + number + "\">\n";
            const Failure* first = r->failures.empty() ? nullptr : &r->failures.front();
            std::string details;
            for (const Failure& f : r->failures)
                details += f.file + ":" + std::to_string(f.line) + ": [" + f.kind + "] " + f.message + " (frame " + std::to_string(f.frame) + ")\n";
            if (r->status == Status::Failed) {
                xml += "      <failure message=\"" + xmlEscape(first ? first->message : std::string("failed")) + "\" type=\"" +
                       xmlEscape(first ? first->kind : std::string("check")) + "\">" + xmlEscape(details) + "</failure>\n";
            } else if (r->status == Status::Error || r->status == Status::Hung) {
                xml += "      <error message=\"" + xmlEscape(first ? first->message : std::string(statusName(r->status))) + "\" type=\"" +
                       xmlEscape(first ? first->kind : std::string(statusName(r->status))) + "\">" + xmlEscape(details) + "</error>\n";
            } else if (r->status == Status::Skipped) {
                xml += "      <skipped message=\"" + xmlEscape(r->skipReason) + "\"/>\n";
            }
            std::string out;
            for (const std::string& line : r->log) out += line + "\n";
            for (const ConsoleLine& line : r->console) out += "[" + line.level + "] " + line.message + "\n";
            if (!out.empty()) xml += "      <system-out>" + xmlEscape(out) + "</system-out>\n";
            xml += "    </testcase>\n";
        }
        xml += "  </testsuite>\n";
    }
    xml += "</testsuites>\n";
    return xml;
}

bool matchesFilter(const CaseSpec& spec, const std::string& filter) {
    if (trim(filter).empty()) return true;
    const std::string id = lower(spec.id);
    size_t start = 0;
    while (start <= filter.size()) {
        size_t end = filter.find(',', start);
        if (end == std::string::npos) end = filter.size();
        const std::string term = lower(trim(filter.substr(start, end - start)));
        start = end + 1;
        if (term.empty()) continue;
        if (term.rfind("tag:", 0) == 0) {
            const std::string tag = trim(term.substr(4));
            for (const std::string& t : spec.tags)
                if (lower(t) == tag) return true;
        } else if (id.find(term) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string caseIdPath(const std::string& caseId) {
    std::string out;
    for (char c : caseId) {
        if (c == '/' || c == '#') out += "__";
        else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-') out += c;
        else out += '_';
    }
    return out;
}

std::string isoTimeUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

std::string newRunId() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[40];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &tm);
    std::random_device device;
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "-%04x", static_cast<unsigned>(device() & 0xffffu));
    return std::string(text) + suffix;
}

std::string beginLine(int cases, const std::string& runId) {
    return "begin " + Json{{"cases", cases}, {"run", runId}}.dump();
}

std::string caseLine(const CaseResult& result) {
    constexpr size_t MAX_LINE = 16 * 1024;
    const std::string prefix = "case ";
    Json json = caseJson(result, 20);
    std::string line = prefix + json.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (line.size() <= MAX_LINE) return line;
    // the console and the log go first, then the failures' text is cut
    json["console"] = Json::array();
    json["log"] = Json::array();
    line = prefix + json.dump(-1, ' ', false, Json::error_handler_t::replace);
    for (size_t keep = 512; line.size() > MAX_LINE && keep >= 32; keep /= 2) {
        for (Json& f : json["failures"]) {
            std::string message = f.value("message", std::string());
            if (message.size() > keep) f["message"] = message.substr(0, keep) + "...";
        }
        line = prefix + json.dump(-1, ' ', false, Json::error_handler_t::replace);
    }
    if (line.size() > MAX_LINE) {
        json["failures"] = Json::array({json["failures"].empty() ? Json(nullptr) : json["failures"][0]});
        line = prefix + json.dump(-1, ' ', false, Json::error_handler_t::replace);
    }
    return line;
}

std::string summaryLine(const Counts& counts) {
    return "summary " + countsJson(counts).dump();
}

std::string hostPlatformName() {
#if defined(__EMSCRIPTEN__)
    return "web";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return "ios";
#else
    return "macos";
#endif
#elif defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

std::string hostArchName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__wasm__)
    return "wasm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::string backendName() {
    switch (Engine::getGraphicBackend()) {
        case GraphicBackend::GLCORE: return "glcore";
        case GraphicBackend::GLES3: return "gles3";
        case GraphicBackend::D3D11: return "d3d11";
        case GraphicBackend::METAL: return "metal";
        case GraphicBackend::WGPU: return "wgpu";
        case GraphicBackend::VULKAN: return "vulkan";
    }
    return "unknown";
}

}  // namespace jasperengine::gametest
