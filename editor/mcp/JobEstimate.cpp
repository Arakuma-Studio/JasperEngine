// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JobEstimate.h"

#include "Out.h"
#include "util/FileUtils.h"
#include "util/ImpostorBaker.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;
namespace fs = std::filesystem;

namespace {

    double roundTenth(double value) {
        return std::round(value * 10.0) / 10.0;
    }

    double medianOf(std::vector<double> values) {
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        return n % 2 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
    }

    // one read-modify-write of a history file at a time (jobs finish on worker threads too)
    std::mutex& historyMutex() {
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

}

Json JobEstimate::toJson() const {
    Json out = {{"seconds", roundTenth(seconds)}, {"basis", basis}, {"confidence", confidence}};
    if (bytes > 0) out["bytes"] = bytes;
    return out;
}

// ---------------------------------------------------------------- the models

double jasperengine::editor::mcp::jobmodel::recordSeconds(int64_t frames, int width, int height, bool movie) {
    const double pixels = static_cast<double>(std::max(width, 1)) * static_cast<double>(std::max(height, 1));
    const double count = static_cast<double>(std::max<int64_t>(frames, 0));
    // entering Play, then per frame an editor frame, the readback and the encode (a movie also
    // pipes to FFmpeg and decodes once to check)
    return (movie ? 2.0 : 1.5) + count * (0.01 + pixels * (movie ? 5e-8 : 4e-8));
}

double jasperengine::editor::mcp::jobmodel::buildSeconds(BuildKind kind, uint64_t files, uint64_t bytes) {
    const double f = static_cast<double>(files);
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    switch (kind) {
        case BuildKind::Source: return 2.0 + f * 0.002 + mb / 200.0;
        case BuildKind::Prebuilt: return 3.0 + f * 0.004 + mb / 40.0;
        case BuildKind::CompiledDesktop: return 60.0 + f * 0.004 + mb / 40.0;
        case BuildKind::CompiledWeb: return 180.0 + f * 0.004 + mb / 40.0;
    }
    return 3.0;
}

double jasperengine::editor::mcp::jobmodel::impostorSeconds(int impostors, int views, int resolution) {
    const double res = static_cast<double>(std::max(resolution, 1));
    int columns = 4, rows = 2;
    ImpostorBaker::atlasGrid(views, columns, rows);
    const double atlasPixels = columns * rows * res * res;
    // the model's load, a frame and a readback per view and pass, the two atlases' PNG encodes
    const double perImpostor = 0.5 + 2.0 * std::max(views, 1) * (1.0 / 60.0 + res * res * 1e-8) + 2.0 * atlasPixels * 3e-8;
    return std::max(impostors, 0) * perImpostor;
}

double jasperengine::editor::mcp::jobmodel::titleExportSeconds(size_t layers, int canvasWidth, int canvasHeight, size_t outputs) {
    const double pixels = static_cast<double>(std::max(canvasWidth, 1)) * static_cast<double>(std::max(canvasHeight, 1));
    const double out = static_cast<double>(std::max<size_t>(outputs, 1));
    return 0.2 + out * (0.1 + static_cast<double>(std::max<size_t>(layers, 1)) * pixels * 2e-8);
}

double jasperengine::editor::mcp::jobRemainingSeconds(double estimated, double elapsed, double progress) {
    const double byEstimate = std::max(0.0, estimated - elapsed);
    if (!(progress > 0.0)) return byEstimate;
    if (progress >= 1.0) return 0.0;
    // early the estimate knows more than a few percent of progress; late the progress wins
    const double byProgress = elapsed * (1.0 - progress) / progress;
    return std::max(0.0, (1.0 - progress) * byEstimate + progress * byProgress);
}

std::string jasperengine::editor::mcp::formatJobDuration(double seconds) {
    const long long s = std::max(0LL, static_cast<long long>(std::llround(seconds)));
    char text[32];
    if (s < 60) std::snprintf(text, sizeof(text), "%lld s", s);
    else if (s < 3600) std::snprintf(text, sizeof(text), "%lld min", (s + 30) / 60);
    else std::snprintf(text, sizeof(text), "%lld h %02lld min", s / 3600, (s % 3600) / 60);
    return text;
}

// ---------------------------------------------------------------- the history

JobHistory JobHistory::parse(const std::string& text) {
    JobHistory history;
    const Json root = Json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains("kinds") || !root["kinds"].is_object()) return history;
    for (auto it = root["kinds"].begin(); it != root["kinds"].end(); ++it) {
        if (!it->is_array()) continue;
        for (const Json& sample : *it) {
            if (!sample.is_object() || !sample.contains("seconds") || !sample.contains("model") || !sample["seconds"].is_number() ||
                !sample["model"].is_number()) continue;
            history.add(it.key(), sample["seconds"].get<double>(), sample["model"].get<double>());
        }
    }
    return history;
}

std::string JobHistory::serialize() const {
    Json kindsJson = Json::object();
    for (const auto& [kind, list] : kinds) {
        Json samples = Json::array();
        for (const Sample& s : list) samples.push_back({{"seconds", s.seconds}, {"model", s.model}});
        kindsJson[kind] = samples;
    }
    return Json{{"version", 1}, {"kinds", kindsJson}}.dump(1) + "\n";
}

void JobHistory::add(const std::string& kind, double seconds, double model) {
    if (kind.empty() || !std::isfinite(seconds) || !std::isfinite(model) || seconds <= 0.0 || model <= 0.0) return;
    std::vector<Sample>& list = kinds[kind];
    list.push_back({seconds, model});
    if (list.size() > kKeep) list.erase(list.begin(), list.end() - static_cast<std::ptrdiff_t>(kKeep));
}

const std::vector<JobHistory::Sample>& JobHistory::samples(const std::string& kind) const {
    static const std::vector<Sample> none;
    const auto it = kinds.find(kind);
    return it == kinds.end() ? none : it->second;
}

bool JobHistory::ratio(const std::string& kind, double& out) const {
    const std::vector<Sample>& list = samples(kind);
    if (list.empty()) return false;
    std::vector<double> ratios;
    for (const Sample& s : list) ratios.push_back(s.seconds / s.model);
    out = std::clamp(medianOf(ratios), kMinRatio, kMaxRatio);
    return true;
}

JobEstimate JobHistory::estimate(const std::string& kind, double modelSeconds, uint64_t bytes) const {
    JobEstimate e;
    e.modelSeconds = std::max(modelSeconds, 0.0);
    e.seconds = e.modelSeconds;
    e.bytes = bytes;
    double r = 1.0;
    if (!ratio(kind, r)) return e;
    const std::vector<Sample>& list = samples(kind);
    e.basis = "history";
    e.samples = list.size();
    e.seconds = e.modelSeconds * r;
    std::vector<double> ratios;
    for (const Sample& s : list) ratios.push_back(s.seconds / s.model);
    std::sort(ratios.begin(), ratios.end());
    const double median = medianOf(ratios);
    // a clamped median means the model is far off for this project: little to trust
    if (median < kMinRatio || median > kMaxRatio) {
        e.confidence = "low";
    } else if (ratios.size() >= 5 && ratios[ratios.size() * 3 / 4] <= 1.5 * ratios[ratios.size() / 4]) {
        e.confidence = "high";
    } else {
        e.confidence = "medium";
    }
    return e;
}

fs::path JobHistory::fileFor(const fs::path& projectPath) {
    return projectPath / ".jasperengine" / "user" / "job-history.json";
}

JobHistory JobHistory::load(const fs::path& file) {
    std::lock_guard<std::mutex> lock(historyMutex());
    std::ifstream in(file, std::ios::binary);
    if (!in) return JobHistory();
    std::stringstream text;
    text << in.rdbuf();
    return parse(text.str());
}

bool JobHistory::record(const fs::path& file, const std::string& kind, double seconds, double model) {
    std::lock_guard<std::mutex> lock(historyMutex());
    JobHistory history;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            std::stringstream text;
            text << in.rdbuf();
            history = parse(text.str());
        }
    }
    history.add(kind, seconds, model);
    std::error_code ec;
    if (!fs::is_directory(file.parent_path(), ec)) {
        safewrite::EditorInternal makeDir(file.parent_path());
        FileUtils::createDirectories(file.parent_path(), ec);
    }
    // the editor's own per-user file: no tool names it (util/SafeWrite.h)
    safewrite::EditorInternal ownHistory(file);
    if (FileUtils::writeFileReplacing(file, history.serialize())) return true;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) Out::warning("Could not write the job history %s: estimates use the cost models only.", FileUtils::pathToUtf8(file).c_str());
    return false;
}
