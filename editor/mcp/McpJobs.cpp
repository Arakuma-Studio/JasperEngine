// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpJobs.h"

#include <algorithm>

using namespace jasperengine::editor::mcp;

const char* jasperengine::editor::mcp::jobStatusName(JobStatus status) {
    switch (status) {
        case JobStatus::Running: return "running";
        case JobStatus::Succeeded: return "succeeded";
        case JobStatus::Failed: return "failed";
        case JobStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

McpJob::McpJob(std::string id, std::string kind, std::string title)
    : jobId(std::move(id))
    , jobKind(std::move(kind))
    , jobTitle(std::move(title))
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
    std::lock_guard<std::mutex> lock(mutex);
    if (jobStatus != JobStatus::Running) return;
    jobStatus = JobStatus::Succeeded;
    jobProgress = 1.0;
    jobMessage = message;
    jobResult = std::move(result);
    endTime = std::chrono::steady_clock::now();
}

void McpJob::fail(const std::string& message, Json result) {
    std::lock_guard<std::mutex> lock(mutex);
    if (jobStatus != JobStatus::Running) return;
    jobStatus = JobStatus::Failed;
    jobMessage = message;
    jobResult = std::move(result);
    endTime = std::chrono::steady_clock::now();
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

JobPtr McpJobs::create(const std::string& kind, const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex);
    auto job = std::make_shared<McpJob>("job-" + std::to_string(nextId++), kind, title);
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

void jasperengine::editor::mcp::replyWhenJobDone(const CallPtr& call, const JobPtr& job, bool wait, double timeoutSeconds,
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
