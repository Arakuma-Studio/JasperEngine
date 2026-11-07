// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R45, §6.13 "The preview queue")
// - Purpose: the one arbiter of every Title Art preview of the editor (S80's Project window
//   thumbnails and Inspector preview, S82's canvas layer thumbnails, hovers and Add palette,
//   S84's library tiles, S86's material and environment pickers, S87's language thumbnails), so
//   their main-thread work together stays within one per-frame budget (default 4 ms) and previews
//   never block a frame.
// - Scheduling: runFrame(budgetMs) runs on the main thread once per editor frame (from
//   App::engineRender). It steps jobs by priority (Hover, Layers, Tiles, Thumbnails, Prefetch, in
//   that order), visible items before hidden ones within a priority, oldest request first; each
//   step is asked to fit what is left of the budget (a TitleRenderJob step runs whole steps, at
//   least one). A step predicted to exceed the whole budget runs alone in a frame. Predictions are
//   measured ones (TitleArtPreviewJob teaches a TitleStepCosts model what each kind of step took),
//   and the queue checks its own clock after every step: once the frame's time is spent nothing
//   more starts. A frame that went over (an alone step, a step dearer than predicted) is followed
//   by ceil(spent / budget) - 1 idle frames, so previews average within the budget.
// - Side work: what a submitter does on the main thread for its previews outside the jobs
//   (loading the document a request keys by, encoding a finished thumbnail) is charge()d to the
//   frame; runFrame spends only what is left, and frameLeftMs() tells a submitter whether to start
//   more side work this frame.
// - Stats (R45 reporting): working frames, frames over budget, the worst frame and step (its
//   label), and the p95 of the last WINDOW working frames, for get_title's `previews` and S82's
//   performance readout.
// - Requests: request() takes a content key (submitters key by GUID, size, source hash and bake
//   version; requests with the same key share one job), an owner (a window: cancelAll(owner) when
//   it closes) and a factory that makes the job, so device loss (a new context id) drops every
//   job and re-requests it from its factory. A finished job stays until cancelled; result()
//   hands it back for the submitter to read (its texture, its image).
// - Thread-safety: main thread.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // One preview's work, stepped by the queue (TitleArtPreviewRender wraps a TitleRenderJob)
    class TitlePreviewJob {
    public:
        virtual ~TitlePreviewJob() = default;
        // the predicted cost of the next step, us (0 when done)
        virtual uint32_t nextStepCostUs() const = 0;
        // runs steps within about `budgetUs` (at least one); true when the preview is complete
        virtual bool step(uint32_t budgetUs) = 0;
        // a job that cannot finish (its error is the submitter's to show)
        virtual bool failed() const { return false; }
        // the longest piece of work the last step() ran, measured: its label and ms (stats)
        virtual std::string longestStepLabel() const { return {}; }
        virtual double longestStepMs() const { return 0.0; }
    };

    enum class TitlePreviewPriority : uint8_t { Hover, Layers, Tiles, Thumbnails, Prefetch };
    const char* titlePreviewPriorityName(TitlePreviewPriority priority);

    enum class TitlePreviewState : uint8_t { Unknown, Queued, Running, Done, Failed };

    using TitlePreviewOwner = uint64_t;
    using TitlePreviewFactory = std::function<std::unique_ptr<TitlePreviewJob>()>;

    struct TitlePreviewStats {
        size_t queued = 0;          // items with work left
        size_t done = 0;
        uint64_t frames = 0;
        uint64_t steps = 0;
        double budgetMs = 0;        // the last runFrame's budget
        double lastFrameMs = 0;     // the last frame's time: its steps and the side work charged to it
        double maxFrameMs = 0;      // the most a frame spent since resetStats()
        uint64_t workingFrames = 0; // frames that ran a step or were charged side work
        uint64_t overBudgetFrames = 0;  // working frames over the budget
        uint64_t aloneFrames = 0;   // frames that ran one step predicted over the budget alone
        uint64_t restFrames = 0;    // idle frames after a frame that went over
        uint64_t restarts = 0;      // jobs made again after a device loss
        double sideWorkMs = 0;      // charge()d in total
        double worstStepMs = 0;     // the longest single piece of work, measured
        std::string worstStepLabel;
    };

    class TitlePreviewQueue {
    public:
        // `clockMs`: a monotonic clock in ms (tests pass a fake one); `contextId`: the GPU context id
        // (SystemRender::getContextId), whose change means device loss
        explicit TitlePreviewQueue(std::function<double()> clockMs = {}, std::function<uint64_t()> contextId = {});
        ~TitlePreviewQueue();

        // the editor's queue (App::engineRender runs it)
        static TitlePreviewQueue& editor();

        uint64_t request(TitlePreviewPriority priority, const std::string& key, TitlePreviewFactory factory,
                         TitlePreviewOwner owner = 0, bool visible = true);
        void setVisible(uint64_t id, bool visible);
        void cancel(uint64_t id);
        void cancelAll(TitlePreviewOwner owner);

        void runFrame(double budgetMs = 4.0);
        // main-thread work done for previews outside their jobs, counted in this frame's budget
        void charge(double ms);
        // what is left of this frame's budget after the side work charged so far
        double frameLeftMs(double budgetMs = 4.0) const { return budgetMs - charged; }

        TitlePreviewState state(uint64_t id) const;
        // the finished job of a request (null until it is Done)
        TitlePreviewJob* result(uint64_t id) const;
        // drops every job and makes it again from its factory (also done by runFrame when the
        // context id changes)
        void deviceLost();

        const TitlePreviewStats& stats() const { return counters; }
        // the p95 of the last WINDOW working frames' times (0 before the first)
        static constexpr size_t WINDOW = 200;
        double p95FrameMs() const;
        size_t windowFrames() const { return recent.size(); }
        void resetStats();

    private:
        struct Item {
            TitlePreviewPriority priority = TitlePreviewPriority::Prefetch;
            std::string key;
            TitlePreviewFactory factory;
            std::unique_ptr<TitlePreviewJob> job;
            TitlePreviewState state = TitlePreviewState::Queued;
            uint64_t order = 0;
            std::map<uint64_t, TitlePreviewOwner> requests;     // request id -> owner
            std::map<uint64_t, bool> visibility;                // request id -> visible
            bool visible() const;
        };
        std::function<double()> clock;
        std::function<uint64_t()> context;
        uint64_t lastContext = 0;
        std::vector<std::unique_ptr<Item>> items;
        std::map<uint64_t, Item*> byRequest;
        uint64_t nextRequest = 1;
        uint64_t nextOrder = 1;
        int restFrames = 0;
        double charged = 0;
        TitlePreviewStats counters;
        std::vector<double> recent;     // working frames' times, a ring of WINDOW
        size_t recentNext = 0;

        Item* pick() const;
        void drop(Item* item);
        void refreshCounts();
    };

}
