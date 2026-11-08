// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtPreviewRender.h"

#include "Project.h"
#include "render/FrameReadback.h"
#include "render/SystemRender.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectRegistry.h"
#include "asset/AssetFileWrite.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    texsynth::Image fitTo(const texsynth::Image& source, int side) {
        const texsynth::Image image = texsynth::toRgba8(source);
        const int larger = std::max(image.width, image.height);
        if (larger <= side || larger <= 0) return image;
        const double f = static_cast<double>(larger) / side;
        const int w = std::max(1, static_cast<int>(std::lround(image.width / f))), h = std::max(1, static_cast<int>(std::lround(image.height / f)));
        texsynth::Image out = texsynth::Image::make(w, h, 4, 8);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int x0 = static_cast<int>(x * f), x1 = std::max(x0 + 1, std::min(image.width, static_cast<int>((x + 1) * f)));
                const int y0 = static_cast<int>(y * f), y1 = std::max(y0 + 1, std::min(image.height, static_cast<int>((y + 1) * f)));
                double sum[4] = {0, 0, 0, 0};
                int n = 0;
                for (int sy = y0; sy < y1; sy++) {
                    for (int sx = x0; sx < x1; sx++) {
                        const uint8_t* p = image.pixel(sx, sy);
                        const double a = p[3] / 255.0;
                        for (int k = 0; k < 3; k++) sum[k] += p[k] * a;
                        sum[3] += p[3];
                        n++;
                    }
                }
                uint8_t* d = out.pixel(x, y);
                const double alpha = sum[3] / n;
                for (int k = 0; k < 3; k++) d[k] = alpha > 0 ? static_cast<uint8_t>(std::clamp(sum[k] / n / (alpha / 255.0), 0.0, 255.0)) : 0;
                d[3] = static_cast<uint8_t>(std::lround(alpha));
            }
        }
        return out;
    }

}

namespace {

    // the static estimates of the pieces that are not render steps (the model scales them)
    constexpr uint32_t BUILD_ESTIMATE_US = 500;
    constexpr const char* BUILD_KIND = "preview build";
    constexpr const char* READBACK_KIND = "preview readback";
    // the editor's preview budget a frame (App::engineRender's runFrame): passes predicted over it
    // are cut into tiles
    constexpr uint32_t SPLIT_US = 4000;

    // charges the main-thread time of its scope to the preview queue's frame
    class SideWork {
    public:
        SideWork() : start(titleArtPreviewCosts().nowUs()) {}
        ~SideWork() { TitlePreviewQueue::editor().charge((titleArtPreviewCosts().nowUs() - start) / 1000.0); }
        SideWork(const SideWork&) = delete;
        SideWork& operator=(const SideWork&) = delete;

    private:
        double start;
    };

}

titleart::TitleStepCosts& editor::titleArtPreviewCosts() {
    static titleart::TitleStepCosts* costs = [] {
        auto* made = new titleart::TitleStepCosts();
        // a software rasterizer draws on its own threads, but a pass that samples what it has not
        // drawn yet (or the readback) makes the main thread wait for it: previews wait for the fence
        if (SystemRender::isSoftwareGpu()) made->setGpuGate([] { return !FrameReadback::gpuFencePassed(); }, [] { FrameReadback::fenceGpu(); });
        return made;
    }();
    return *costs;
}

TitleArtPreviewJob::TitleArtPreviewJob(Project* project, titleart::RenderRequest request) : project(project), request(std::move(request)) {
}

uint32_t TitleArtPreviewJob::readbackEstimateUs() const {
    // about 10 ns a pixel: the copy, unpremultiply and encode of RGBA8
    const titleart::TitleImage& image = job->image();
    return std::max<uint32_t>(1, static_cast<uint32_t>(static_cast<double>(std::max(0, image.width)) * std::max(0, image.height) / 100.0));
}

uint32_t TitleArtPreviewJob::nextStepCostUs() const {
    if (finished || failed()) return 0;
    const titleart::TitleStepCosts& costs = titleArtPreviewCosts();
    if (!job) return costs.predictUs(BUILD_KIND, BUILD_ESTIMATE_US);
    if (!job->done()) return std::max<uint32_t>(1, job->nextStepCostUs());
    return costs.predictUs(READBACK_KIND, readbackEstimateUs());
}

void TitleArtPreviewJob::noteLongest(const std::string& label, double ms) {
    if (ms < longestMs) return;
    longestMs = ms;
    longestLabel = label;
}

bool TitleArtPreviewJob::step(uint32_t budgetUs) {
    if (finished) return true;
    if (failed()) return true;
    titleart::TitleStepCosts& costs = titleArtPreviewCosts();
    const double start = costs.nowUs();
    longestLabel.clear();
    longestMs = 0.0;
    ranNothing = true;
    bool first = true;
    // the next piece starts while it fits what is left of the budget, with the queue's margin for
    // a piece dearer than its average (the first always starts)
    const auto fits = [&](uint32_t predictedUs) {
        return first || costs.nowUs() - start + predictedUs * TitlePreviewQueue::VARIANCE_MARGIN <= static_cast<double>(budgetUs);
    };
    if (!job) {
        std::string error;
        const double t0 = costs.nowUs();
        job = TitleArtRenderHost::editor().begin(project, request, error);
        const double us = costs.nowUs() - t0;
        if (!job) {
            failure = error;
            return true;
        }
        job->useCosts(&costs);
        costs.observe(BUILD_KIND, BUILD_ESTIMATE_US, us);
        noteLongest(BUILD_KIND, us / 1000.0);
        ranNothing = false;
        first = false;
    }
    if (!job->done()) {
        // a pass predicted over the whole budget is cut into tiles where it allows (>= 256 px)
        job->splitOversizedSteps(SPLIT_US);
        if (!fits(job->nextStepCostUs())) return false;
        titleart::StepBudget budget;
        // the job measures what it spends and predicts each step: the margin scales what is left
        const double left = (static_cast<double>(budgetUs) - (costs.nowUs() - start)) / TitlePreviewQueue::VARIANCE_MARGIN;
        budget.budgetUs = static_cast<uint32_t>(std::max(1.0, left));
        titleart::StepReport report;
        job->step(budget, &report);
        if (report.steps > 0) {
            noteLongest(report.longestLabel, report.longestUs / 1000.0);
            ranNothing = false;
            first = false;
        }
        if (job->failed()) {
            failure = job->error();
            return true;
        }
        if (!job->done()) return false;
    }
    const uint32_t estimate = readbackEstimateUs();
    // the readback waits for the software GPU's drawing: a later frame reads it
    if (costs.gpuBusy() || !fits(costs.predictUs(READBACK_KIND, estimate))) return false;
    std::string error;
    const double t0 = costs.nowUs();
    const bool read = TitleArtRenderHost::readBack(*job, 8, result, error);
    const double us = costs.nowUs() - t0;
    costs.observe(READBACK_KIND, estimate, us);
    noteLongest(READBACK_KIND, us / 1000.0);
    ranNothing = false;
    if (!read) {
        failure = error;
        return true;
    }
    job.reset();
    finished = true;
    return true;
}

std::string editor::titleArtPreviewKey(const titleart::TitleArtDoc& doc, int side) {
    return doc.guid() + ":" + std::to_string(side) + ":" + titleArtSourceHash(doc) + ":" + std::to_string(titleart::titleArtBakeVersion(doc));
}

uint64_t editor::requestTitleArtPreview(Project* project, std::shared_ptr<const titleart::TitleArtDoc> doc, const std::string& key, int side,
                                        TitlePreviewPriority priority, TitlePreviewOwner owner, bool visible) {
    titleart::RenderRequest request;
    const int larger = std::max(doc->canvasWidth(), doc->canvasHeight());
    request.pxScale = std::min(1.0f, static_cast<float>(side) / static_cast<float>(std::max(1, larger)));
    request.doc = std::move(doc);
    return TitlePreviewQueue::editor().request(priority, key, [project, request]() {
        return std::make_unique<TitleArtPreviewJob>(project, request);
    }, owner, visible);
}

uint64_t editor::requestTitleArtPreview(Project* project, const fs::path& document, int side, TitlePreviewPriority priority,
                                        TitlePreviewOwner owner, bool visible, std::string& error) {
    // the document load and hash the key needs are the preview's main-thread work too
    SideWork charge;
    titleart::TitleArtDoc doc;
    std::string code;
    if (!loadTitleArtForBake(project, document, doc, code, error)) return 0;
    auto snapshot = std::make_shared<const titleart::TitleArtDoc>(std::move(doc));
    const std::string key = titleArtPreviewKey(*snapshot, side);
    return requestTitleArtPreview(project, snapshot, key, side, priority, owner, visible);
}

namespace {

    // the main output's PNG is on disk (else it cannot be fresh, and its status is not worth
    // hashing every font and image the document reads)
    bool mainOutputExists(const fs::path& document, const titleart::TitleArtDoc& doc) {
        auto outputs = doc.root.find("outputs");
        if (outputs == doc.root.end() || !outputs->is_array() || outputs->empty()) return false;
        std::error_code ec;
        return fs::is_regular_file(titleArtOutputPath(document, outputs->front()), ec);
    }

    // a worker's part of a thumbnail: the document parsed (unless it was open) and its preview key
    TitleArtThumbnailer::Prepared prepareThumbnail(fs::path document, std::shared_ptr<const titleart::TitleArtDoc> open, int side) {
        TitleArtThumbnailer::Prepared out;
        if (!open) {
            std::string text;
            if (!assetio::readFile(document, text)) return out;
            titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
            if (!parsed.ok) return out;
            open = std::make_shared<const titleart::TitleArtDoc>(std::move(parsed.doc));
        }
        out.doc = open;
        out.mayBeFresh = mainOutputExists(document, *open);
        if (!out.mayBeFresh) out.key = titleArtPreviewKey(*open, side);
        return out;
    }

    bool writeThumbnail(const texsynth::Image& image, const fs::path& thumbnail) {
        std::vector<uint8_t> png;
        std::error_code ec;
        FileUtils::createDirectories(thumbnail.parent_path(), ec);
        return texsynth::encodePng(image, png) && FileUtils::writeFileReplacing(thumbnail, std::string(png.begin(), png.end()));
    }

}

bool editor::titleArtFreshOutputImage(Project* project, const fs::path& document, int side, texsynth::Image& out) {
    titleart::TitleArtDoc doc;
    std::string code, error;
    if (!loadTitleArtForBake(project, document, doc, code, error)) return false;
    if (!mainOutputExists(document, doc)) return false;
    const TitleBakeStatus status = titleArtStatusOf(project, document, doc);
    if (status.outputs.empty() || status.outputs.front().status != TitleOutputStatus::Fresh) return false;
    texsynth::Image png;
    if (!texsynth::loadImage(status.outputs.front().file, png, error)) return false;
    out = fitTo(png, side);
    return true;
}

TitleArtThumbnailer::~TitleArtThumbnailer() {
    cancelAll();
}

void TitleArtThumbnailer::request(Project* forProject, const fs::path& document, const fs::path& thumbnail, int side, std::function<void(bool)> done) {
    project = forProject;
    Pending item;
    item.document = document;
    item.thumbnail = thumbnail;
    item.side = side;
    item.done = std::move(done);
    pending.push_back(std::move(item));
}

void TitleArtThumbnailer::process() {
    for (size_t i = 0; i < pending.size();) {
        Pending& item = pending[i];
        bool finished = false, ok = false;
        if (item.state == Pending::State::New) {
            // the parse and the key's hash run on a worker; an open document is copied here (the
            // open documents are the main thread's)
            std::shared_ptr<const titleart::TitleArtDoc> open;
            std::shared_ptr<TitleArtDocument> document = TitleArtDocument::find(item.document);
            if (document && (!document->editable() || !document->loadError().empty())) {
                finished = true;
            } else {
                if (document) {
                    SideWork charge;
                    open = std::make_shared<const titleart::TitleArtDoc>(document->doc());
                }
                item.preparing = std::async(std::launch::async, prepareThumbnail, item.document, open, item.side);
                item.state = Pending::State::Preparing;
            }
        }
        if (item.state == Pending::State::Preparing && item.preparing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            item.prepared = item.preparing.get();
            if (item.prepared.doc) item.state = Pending::State::Prepared;
            else finished = true;
        }
        // a baked document's status and PNG load on the main thread, charged to the frame and only
        // while it has budget left
        if (item.state == Pending::State::Prepared && item.prepared.mayBeFresh && TitlePreviewQueue::editor().frameLeftMs() > 0) {
            texsynth::Image image;
            bool fresh = false;
            {
                SideWork charge;
                fresh = titleArtFreshOutputImage(project, item.document, item.side, image);
                if (!fresh) item.prepared.key = titleArtPreviewKey(*item.prepared.doc, item.side);
            }
            item.prepared.mayBeFresh = false;
            if (fresh) {
                item.writing = std::async(std::launch::async, writeThumbnail, std::move(image), item.thumbnail);
                item.state = Pending::State::Writing;
            }
        }
        if (item.state == Pending::State::Prepared && !item.prepared.mayBeFresh) {
            item.id = requestTitleArtPreview(project, item.prepared.doc, item.prepared.key, item.side, TitlePreviewPriority::Thumbnails, owner, true);
            item.state = Pending::State::Rendering;
        }
        if (item.state == Pending::State::Rendering) {
            const TitlePreviewState state = TitlePreviewQueue::editor().state(item.id);
            if (state == TitlePreviewState::Done) {
                texsynth::Image image;
                if (auto* job = dynamic_cast<TitleArtPreviewJob*>(TitlePreviewQueue::editor().result(item.id))) image = job->image().image;
                TitlePreviewQueue::editor().cancel(item.id);
                item.id = 0;
                if (image.empty()) {
                    finished = true;
                } else {
                    // the PNG encode and write on a worker
                    item.writing = std::async(std::launch::async, writeThumbnail, std::move(image), item.thumbnail);
                    item.state = Pending::State::Writing;
                }
            } else if (state == TitlePreviewState::Failed || state == TitlePreviewState::Unknown) {
                TitlePreviewQueue::editor().cancel(item.id);
                item.id = 0;
                finished = true;
            }
        }
        if (item.state == Pending::State::Writing && item.writing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            ok = item.writing.get();
            finished = true;
        }
        if (!finished) {
            i++;
            continue;
        }
        std::function<void(bool)> done = std::move(item.done);
        pending.erase(pending.begin() + static_cast<long>(i));
        if (done) done(ok);
    }
}

void TitleArtThumbnailer::cancelAll() {
    TitlePreviewQueue::editor().cancelAll(owner);
    pending.clear();
}
