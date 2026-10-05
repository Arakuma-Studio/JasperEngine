// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpCall.h"
#include "McpJson.h"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    enum class JobStatus {
        Running,
        Succeeded,
        Failed,
        Cancelled
    };

    const char* jobStatusName(JobStatus status);

    // What a running job holds exclusively. McpServer refuses the tool calls (and the editor
    // the human actions) that would race it; see ToolActivity in McpRegistry.h.
    enum class JobBusy {
        None,           // nothing (play startup, previews...)
        Export,         // export / build player / generate shaders: reads the project from a worker thread
        ScriptCompile   // C++ script build in the project's internal build directory
    };

    const char* jobBusyName(JobBusy busy);

    // A long operation (export/build players, C++ compile, play startup) an agent can poll
    // with get_job and stop with cancel_job. Every method is thread-safe: workers report
    // progress and finish the job from their own thread.
    class McpJob {
    public:
        McpJob(std::string id, std::string kind, std::string title, JobBusy busy = JobBusy::None);

        const std::string& id() const { return jobId; }
        const std::string& kind() const { return jobKind; }
        const std::string& title() const { return jobTitle; }
        JobBusy busy() const { return jobBusy; }

        JobStatus status() const;
        bool finished() const { return status() != JobStatus::Running; }

        // progress in [0,1], or < 0 when unknown
        void setProgress(double progress, const std::string& message = {});
        // One line of output; the last 200 lines are kept
        void log(const std::string& line);

        // Finishing; only the first call counts.
        void succeed(const std::string& message, Json result = Json::object());
        void fail(const std::string& message, Json result = Json::object());
        void markCancelled(const std::string& message = "Cancelled.");

        // cancel_job: sets cancelRequested() and runs the cancel handler (on the calling
        // thread, usually the main one). The job is not finished until its owner says so,
        // unless no cancel handler was set, in which case it is marked cancelled at once.
        void requestCancel();
        bool cancelRequested() const;
        void setCancelHandler(std::function<void()> handler);

        // {id, kind, title, status, progress, message, elapsed_seconds, log_tail, result}
        Json toJson(int logLines = 40) const;
        std::string message() const;
        Json result() const;

    private:
        mutable std::mutex mutex;
        std::string jobId;
        std::string jobKind;
        std::string jobTitle;
        JobBusy jobBusy = JobBusy::None;
        JobStatus jobStatus = JobStatus::Running;
        double jobProgress = -1.0;
        std::string jobMessage;
        std::deque<std::string> logLines;
        Json jobResult = Json::object();
        bool cancelFlag = false;
        std::function<void()> cancelHandler;
        std::chrono::steady_clock::time_point startTime;
        std::chrono::steady_clock::time_point endTime;
    };

    using JobPtr = std::shared_ptr<McpJob>;

    class McpJobs {
    public:
        // busy: what the job holds exclusively while it runs (McpServer's busy policy)
        JobPtr create(const std::string& kind, const std::string& title, JobBusy busy = JobBusy::None);
        JobPtr find(const std::string& id) const;
        // The oldest running job holding `busy`, or null
        JobPtr runningBusy(JobBusy busy) const;
        // Running jobs first, then the most recent finished ones (finished jobs are pruned
        // past 50).
        std::vector<JobPtr> list() const;
        size_t runningCount() const;

    private:
        mutable std::mutex mutex;
        std::vector<JobPtr> jobs;
        unsigned nextId = 1;
    };

    // The `wait` / `timeout_seconds` pattern for tools that start a job: when wait is
    // false, or the job is still running after timeoutSeconds, the call returns
    // {job_id, status:"running", ...} and the agent polls get_job. Otherwise onFinished
    // builds the result from the finished job (default: its message + toJson()).
    // Forwards job progress as notifications/progress. Main thread.
    void replyWhenJobDone(const CallPtr& call, const JobPtr& job, bool wait, double timeoutSeconds,
                          std::function<ToolResult(const McpJob&)> onFinished = nullptr);

}
