// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R45, R50, R51, §6.13)
// - Purpose: the previews of `.titleart` documents in the editor (the Project window's
//   thumbnails, the Inspector's preview), each a TitleRenderJob stepped by the one
//   TitlePreviewQueue within its per-frame budget, read back to the CPU when done.
// - Keys: GUID, size, the document's source hash and TITLE_ART_BAKE_VERSION (content + engine
//   version, contract decision 11), so an edit, an engine change or another size is another
//   preview, and equal requests share one job. Device loss is the queue's (it re-requests).
// - Thumbnails (TitleArtThumbnailer): the fresh main output's PNG when there is one, else a
//   render, written as a PNG to the thumbnail path the Project window reads.
// - Budget (R45): a preview's work is three kinds of pieces: building the job, its render steps
//   and the readback. Each is measured on the main thread and teaches one model
//   (titleArtPreviewCosts) that predicts the next piece, and a step() starts a piece only while it
//   fits what is left of its budget (the first piece of a call always runs). The work a request
//   does before its job exists (loading the document its key needs) and the thumbnailer's PNG
//   encode are charged to the queue's frame; the thumbnailer starts such work only while the frame
//   has budget left.
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitlePreviewQueue.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // one preview render, stepped by TitlePreviewQueue
    class TitleArtPreviewJob : public TitlePreviewJob {
    public:
        TitleArtPreviewJob(Project* project, titleart::RenderRequest request);
        uint32_t nextStepCostUs() const override;
        bool step(uint32_t budgetUs) override;
        bool failed() const override { return !failure.empty(); }
        std::string longestStepLabel() const override { return longestLabel; }
        double longestStepMs() const override { return longestMs; }

        const std::string& error() const { return failure; }
        // the finished render (straight RGBA8)
        const TitleRenderedImage& image() const { return result; }

    private:
        Project* project = nullptr;
        titleart::RenderRequest request;
        std::unique_ptr<titleart::TitleRenderJob> job;
        TitleRenderedImage result;
        std::string failure;
        bool finished = false;
        std::string longestLabel;
        double longestMs = 0.0;

        uint32_t readbackEstimateUs() const;
        void noteLongest(const std::string& label, double ms);
    };

    // what the pieces of the editor's previews cost on this machine, learned as they run
    titleart::TitleStepCosts& titleArtPreviewCosts();

    // a preview of a document at most `side` pixels on its larger side; 0 with `error` when the
    // document does not load (or the renderer is unavailable)
    uint64_t requestTitleArtPreview(Project* project, const std::filesystem::path& document, int side, TitlePreviewPriority priority,
                                    TitlePreviewOwner owner, bool visible, std::string& error);

    // the Project window's Title Art thumbnails
    class TitleArtThumbnailer {
    public:
        explicit TitleArtThumbnailer(TitlePreviewOwner owner) : owner(owner) {}
        ~TitleArtThumbnailer();
        // `done(ok)` once the PNG at `thumbnail` is written (or could not be)
        void request(Project* project, const std::filesystem::path& document, const std::filesystem::path& thumbnail, int side,
                     std::function<void(bool)> done);
        // main thread, every frame: hands finished previews to their callbacks
        void process();
        bool busy() const { return !pending.empty(); }
        void cancelAll();

    private:
        struct Pending {
            std::filesystem::path document;
            std::filesystem::path thumbnail;
            int side = 128;
            uint64_t id = 0;
            std::function<void(bool)> done;
        };
        TitlePreviewOwner owner;
        Project* project = nullptr;
        std::vector<Pending> pending;
    };

    // a fresh output's PNG of a document as a thumbnail image (the larger side `side`); false when
    // the main output is not fresh
    bool titleArtFreshOutputImage(Project* project, const std::filesystem::path& document, int side, texsynth::Image& out);

}
