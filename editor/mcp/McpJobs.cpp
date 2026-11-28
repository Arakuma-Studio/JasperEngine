// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpJobs.h"

#include "Out.h"
#include "util/FileUtils.h"
#include "util/ImpostorBaker.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;

const char* jobStatusName(JobStatus status) {
    switch (status) {
        case JobStatus::Running: return "running";
        case JobStatus::Succeeded: return "succeeded";
        case JobStatus::Failed: return "failed";
        case JobStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* jobBusyName(JobBusy busy) {
    switch (busy) {
        case JobBusy::None: return "none";
        case JobBusy::Export: return "export";
        case JobBusy::ScriptCompile: return "script_compile";
    }
    return "none";
}

McpJob::McpJob(std::string id, std::string kind, std::string title, JobBusy busy)
    : jobId(std::move(id))
    , jobKind(std::move(kind))
    , jobTitle(std::move(title))
    , jobBusy(busy)
    , startTime(std::chrono::steady_clock::now()) {
}

JobStatus McpJob::status() const {
    std::lock_guard<std::mutex> lock(mutex);
    return jobStatus;
}

void McpJob::setProgress(double progress, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    jobProgress = progress;
    if (!message.empty()) jobMessage = message;
}

void McpJob::log(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    logLines.push_back(line);
    while (logLines.size() > 200) logLines.pop_front();
}

void McpJob::succeed(const std::string& message, Json result) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (jobStatus != JobStatus::Running) return;
        jobStatus = JobStatus::Succeeded;
        jobProgress = 1.0;
        jobMessage = message;
        endTime = std::chrono::steady_clock::now();
        addReport(result, std::chrono::duration<double>(endTime - startTime).count());
        jobResult = std::move(result);
    }
    recordHistory();
}

void McpJob::fail(const std::string& message, Json result) {
    std::lock_guard<std::mutex> lock(mutex);
    if (jobStatus != JobStatus::Running) return;
    jobStatus = JobStatus::Failed;
    jobMessage = message;
    endTime = std::chrono::steady_clock::now();
    addReport(result, std::chrono::duration<double>(endTime - startTime).count());
    jobResult = std::move(result);
}

void McpJob::setEstimate(JobEstimate estimate, std::string kind, std::filesystem::path file) {
    std::lock_guard<std::mutex> lock(mutex);
    hasEstimate = true;
    historyModel = estimate.modelSeconds;
    jobEstimate = std::move(estimate);
    historyKind = std::move(kind);
    historyFile = std::move(file);
}

void McpJob::estimateFor(const std::filesystem::path& projectPath, const std::string& kind, double modelSeconds, uint64_t bytes) {
    const std::filesystem::path file = JobHistory::fileFor(projectPath);
    setEstimate(JobHistory::load(file).estimate(kind, modelSeconds, bytes), kind, file);
}

void McpJob::setHistoryModel(std::string kind, double modelSeconds) {
    std::lock_guard<std::mutex> lock(mutex);
    historyKind = std::move(kind);
    historyModel = modelSeconds;
}

void McpJob::addFallback(JobFallback fallback) {
    std::lock_guard<std::mutex> lock(mutex);
    if (std::find(fallbacks.begin(), fallbacks.end(), fallback) == fallbacks.end()) fallbacks.push_back(std::move(fallback));
}

bool McpJob::estimate(JobEstimate& out) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (hasEstimate) out = jobEstimate;
    return hasEstimate;
}

double McpJob::progress() const {
    std::lock_guard<std::mutex> lock(mutex);
    return jobProgress;
}

double McpJob::elapsedSeconds() const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto end = jobStatus == JobStatus::Running ? std::chrono::steady_clock::now() : endTime;
    return std::chrono::duration<double>(end - startTime).count();
}

void McpJob::addReport(Json& result, double seconds) const {
    if (!hasEstimate || !result.is_object()) return;
    Json list = Json::array();
    for (const JobFallback& f : fallbacks) list.push_back({{"step", f.step}, {"from", f.from}, {"to", f.to}, {"reason", f.reason}});
    const double estimated = std::round(jobEstimate.seconds * 10.0) / 10.0;
    result["report"] = {{"seconds", std::round(seconds * 10.0) / 10.0},
                        {"estimated_seconds", estimated},
                        {"ratio", estimated > 0.0 ? Json(std::round(seconds / estimated * 100.0) / 100.0) : Json(nullptr)},
                        {"fallbacks", list}};
}

void McpJob::recordHistory() const {
    std::string kind;
    std::filesystem::path file;
    double seconds = 0.0, model = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!hasEstimate || historyFile.empty() || historyKind.empty()) return;
        kind = historyKind;
        file = historyFile;
        model = historyModel;
        seconds = std::chrono::duration<double>(endTime - startTime).count();
    }
    JobHistory::record(file, kind, seconds, model);
}

void McpJob::markCancelled(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (jobStatus != JobStatus::Running) return;
    jobStatus = JobStatus::Cancelled;
    jobMessage = message;
    endTime = std::chrono::steady_clock::now();
}

void McpJob::requestCancel() {
    std::function<void()> handler;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (jobStatus != JobStatus::Running) return;
        cancelFlag = true;
        handler = cancelHandler;
    }
    if (handler) {
        handler();
    } else {
        markCancelled();
    }
}

bool McpJob::cancelRequested() const {
    std::lock_guard<std::mutex> lock(mutex);
    return cancelFlag;
}

void McpJob::setCancelHandler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(mutex);
    cancelHandler = std::move(handler);
}

std::string McpJob::message() const {
    std::lock_guard<std::mutex> lock(mutex);
    return jobMessage;
}

Json McpJob::result() const {
    std::lock_guard<std::mutex> lock(mutex);
    return jobResult;
}

void McpJob::setImages(std::vector<ToolImage> images) {
    std::lock_guard<std::mutex> lock(mutex);
    jobImages = std::move(images);
}

std::vector<ToolImage> McpJob::images() const {
    std::lock_guard<std::mutex> lock(mutex);
    return jobImages;
}

Json McpJob::toJson(int logLines) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto end = jobStatus == JobStatus::Running ? std::chrono::steady_clock::now() : endTime;
    Json out = {
        {"job_id", jobId},
        {"kind", jobKind},
        {"title", jobTitle},
        {"status", jobStatusName(jobStatus)},
        {"message", jobMessage},
        {"elapsed_seconds", std::chrono::duration<double>(end - startTime).count()}
    };
    out["progress"] = jobProgress >= 0.0 ? Json(jobProgress) : Json(nullptr);
    if (hasEstimate) {
        out["estimate"] = jobEstimate.toJson();
        if (jobStatus == JobStatus::Running) {
            const double left = jobRemainingSeconds(jobEstimate.seconds, std::chrono::duration<double>(end - startTime).count(), jobProgress);
            out["remaining_seconds"] = std::round(left * 10.0) / 10.0;
        }
    }
    if (jobBusy != JobBusy::None) out["holds"] = jobBusyName(jobBusy);
    if (cancelFlag) out["cancel_requested"] = true;
    Json tail = Json::array();
    const size_t count = std::min(this->logLines.size(), static_cast<size_t>(std::max(0, logLines)));
    for (size_t i = this->logLines.size() - count; i < this->logLines.size(); i++) {
        tail.push_back(this->logLines[i]);
    }
    out["log_tail"] = tail;
    if (jobStatus != JobStatus::Running) out["result"] = jobResult;
    return out;
}

JobPtr McpJobs::create(const std::string& kind, const std::string& title, JobBusy busy) {
    std::lock_guard<std::mutex> lock(mutex);
    auto job = std::make_shared<McpJob>("job-" + std::to_string(nextId++), kind, title, busy);
    jobs.push_back(job);

    // Keep every running job and the 50 most recent finished ones
    size_t finished = 0;
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        if ((*it)->finished()) finished++;
    }
    for (auto it = jobs.begin(); it != jobs.end() && finished > 50;) {
        if ((*it)->finished()) {
            it = jobs.erase(it);
            finished--;
        } else {
            ++it;
        }
    }
    return job;
}

JobPtr McpJobs::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex);
    for (const JobPtr& job : jobs) {
        if (job->id() == id) return job;
    }
    return nullptr;
}

JobPtr McpJobs::runningBusy(JobBusy busy) const {
    std::lock_guard<std::mutex> lock(mutex);
    for (const JobPtr& job : jobs) {
        if (job->busy() == busy && !job->finished()) return job;
    }
    return nullptr;
}

std::vector<JobPtr> McpJobs::list() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<JobPtr> running;
    std::vector<JobPtr> done;
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        ((*it)->finished() ? done : running).push_back(*it);
    }
    running.insert(running.end(), done.begin(), done.end());
    return running;
}

size_t McpJobs::runningCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    size_t count = 0;
    for (const JobPtr& job : jobs) {
        if (!job->finished()) count++;
    }
    return count;
}

void replyWhenJobDone(const CallPtr& call, const JobPtr& job, bool wait, double timeoutSeconds,
                                                 std::function<ToolResult(const McpJob&)> onFinished) {
    auto finish = [call, job, onFinished]() {
        if (onFinished) {
            call->reply(onFinished(*job));
            return;
        }
        Json data = job->toJson();
        const std::string text = job->title() + ": " + jobStatusName(job->status()) +
            (job->message().empty() ? std::string() : " - " + job->message());
        if (job->status() == JobStatus::Succeeded) call->ok(text, data);
        else call->fail(text, data);
    };
    auto stillRunning = [call, job]() {
        Json data = job->toJson(10);
        call->ok("Job " + job->id() + " (" + job->title() + ") is still running. Poll get_job with job_id \"" +
                 job->id() + "\" until its status is succeeded, failed or cancelled; cancel_job stops it.", data);
    };

    if (job->finished()) {
        finish();
        return;
    }
    // waitUntil reads a timeout <= 0 as "no timeout": a timeout_seconds of 0 (or NaN, or one
    // that overflows int milliseconds) must not make the call wait forever
    if (!(timeoutSeconds > 0.0)) wait = false;
    timeoutSeconds = std::min(timeoutSeconds, 24.0 * 3600.0);
    if (!wait) {
        stillRunning();
        return;
    }

    auto lastProgress = std::make_shared<double>(-2.0);
    auto lastMessage = std::make_shared<std::string>();
    call->waitUntil([call, job, lastProgress, lastMessage]() {
        if (call->cancelled()) {
            // The agent gave up on the request, not on the job: it keeps running
            return true;
        }
        Json state = job->toJson(0);
        const double progress = state["progress"].is_number() ? state["progress"].get<double>() : -1.0;
        const std::string message = state.value("message", "");
        if (progress != *lastProgress || message != *lastMessage) {
            *lastProgress = progress;
            *lastMessage = message;
            call->progress(progress >= 0.0 ? progress : 0.0, progress >= 0.0 ? 1.0 : 0.0, message);
        }
        return job->finished();
    }, static_cast<int>(timeoutSeconds * 1000.0), [job, finish, stillRunning](bool timedOut) {
        if (timedOut || !job->finished()) stillRunning();
        else finish();
    });
}

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

// ---------------------------------------------------------------- S99: the cost models

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

double jobRemainingSeconds(double estimated, double elapsed, double progress) {
    const double byEstimate = std::max(0.0, estimated - elapsed);
    if (!(progress > 0.0)) return byEstimate;
    if (progress >= 1.0) return 0.0;
    // early the estimate knows more than a few percent of progress; late the progress wins
    const double byProgress = elapsed * (1.0 - progress) / progress;
    return std::max(0.0, (1.0 - progress) * byEstimate + progress * byProgress);
}

std::string formatJobDuration(double seconds) {
    const long long s = std::max(0LL, static_cast<long long>(std::llround(seconds)));
    char text[32];
    if (s < 60) std::snprintf(text, sizeof(text), "%lld s", s);
    else if (s < 3600) std::snprintf(text, sizeof(text), "%lld min", (s + 30) / 60);
    else std::snprintf(text, sizeof(text), "%lld h %02lld min", s / 3600, (s % 3600) / 60);
    return text;
}

// ---------------------------------------------------------------- S99: the job history

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

}  // namespace jasperengine::editor::mcp
