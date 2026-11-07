// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtPreviewRender.h"

#include "Project.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleEffectRegistry.h"
#include "util/FileUtils.h"

#include <algorithm>
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
    static titleart::TitleStepCosts* costs = new titleart::TitleStepCosts();
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
    bool first = true;
    // the next piece starts while it fits what is left of the budget (the first always does)
    const auto fits = [&](uint32_t predictedUs) {
        return first || costs.nowUs() - start + predictedUs <= static_cast<double>(budgetUs);
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
        first = false;
    }
    if (!job->done()) {
        if (!fits(job->nextStepCostUs())) return false;
        titleart::StepBudget budget;
        const double left = static_cast<double>(budgetUs) - (costs.nowUs() - start);
        budget.budgetUs = static_cast<uint32_t>(std::max(1.0, left));
        titleart::StepReport report;
        job->step(budget, &report);
        if (report.steps > 0) noteLongest(report.longestLabel, report.longestUs / 1000.0);
        first = false;
        if (job->failed()) {
            failure = job->error();
            return true;
        }
        if (!job->done()) return false;
    }
    const uint32_t estimate = readbackEstimateUs();
    if (!fits(costs.predictUs(READBACK_KIND, estimate))) return false;
    std::string error;
    const double t0 = costs.nowUs();
    const bool read = TitleArtRenderHost::readBack(*job, 8, result, error);
    const double us = costs.nowUs() - t0;
    costs.observe(READBACK_KIND, estimate, us);
    noteLongest(READBACK_KIND, us / 1000.0);
    if (!read) {
        failure = error;
        return true;
    }
    job.reset();
    finished = true;
    return true;
}

uint64_t editor::requestTitleArtPreview(Project* project, const fs::path& document, int side, TitlePreviewPriority priority,
                                        TitlePreviewOwner owner, bool visible, std::string& error) {
    // the document load and hash the key needs are the preview's main-thread work too
    SideWork charge;
    titleart::TitleArtDoc doc;
    std::string code;
    if (!loadTitleArtForBake(project, document, doc, code, error)) return 0;
    auto snapshot = std::make_shared<titleart::TitleArtDoc>(std::move(doc));
    const std::string key = snapshot->guid() + ":" + std::to_string(side) + ":" + titleArtSourceHash(*snapshot) + ":" +
                            std::to_string(titleart::titleArtBakeVersion(*snapshot));
    titleart::RenderRequest request;
    request.doc = snapshot;
    const int larger = std::max(snapshot->canvasWidth(), snapshot->canvasHeight());
    request.pxScale = std::min(1.0f, static_cast<float>(side) / static_cast<float>(std::max(1, larger)));
    return TitlePreviewQueue::editor().request(priority, key, [project, request]() {
        return std::make_unique<TitleArtPreviewJob>(project, request);
    }, owner, visible);
}

bool editor::titleArtFreshOutputImage(Project* project, const fs::path& document, int side, texsynth::Image& out) {
    titleart::TitleArtDoc doc;
    std::string code, error;
    if (!loadTitleArtForBake(project, document, doc, code, error)) return false;
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
    TitlePreviewQueue& queue = TitlePreviewQueue::editor();
    for (size_t i = 0; i < pending.size();) {
        Pending& item = pending[i];
        bool finished = false, ok = false;
        texsynth::Image image;
        if (item.id == 0) {
            // a new item's document loads on the main thread: only while the frame has budget left
            if (queue.frameLeftMs() <= 0) {
                i++;
                continue;
            }
            bool fresh = false;
            {
                SideWork charge;
                // the fresh main output, thumbnailed like an image (a worker-free CPU downscale)
                fresh = titleArtFreshOutputImage(project, item.document, item.side, image);
            }
            if (fresh) {
                finished = true;
                ok = true;
            } else {
                std::string error;
                item.id = requestTitleArtPreview(project, item.document, item.side, TitlePreviewPriority::Thumbnails, owner, true, error);
                if (item.id == 0) finished = true;
            }
        }
        if (!finished && item.id != 0) {
            const TitlePreviewState state = TitlePreviewQueue::editor().state(item.id);
            if (state == TitlePreviewState::Done) {
                if (auto* job = dynamic_cast<TitleArtPreviewJob*>(TitlePreviewQueue::editor().result(item.id))) {
                    image = job->image().image;
                    ok = !image.empty();
                }
                finished = true;
            } else if (state == TitlePreviewState::Failed || state == TitlePreviewState::Unknown) {
                finished = true;
            }
        }
        if (!finished) {
            i++;
            continue;
        }
        if (ok) {
            SideWork charge;
            std::vector<uint8_t> png;
            std::error_code ec;
            FileUtils::createDirectories(item.thumbnail.parent_path(), ec);
            ok = texsynth::encodePng(image, png) && FileUtils::writeFileReplacing(item.thumbnail, std::string(png.begin(), png.end()));
        }
        if (item.id) TitlePreviewQueue::editor().cancel(item.id);
        std::function<void(bool)> done = std::move(item.done);
        pending.erase(pending.begin() + static_cast<long>(i));
        if (done) done(ok);
    }
}

void TitleArtThumbnailer::cancelAll() {
    TitlePreviewQueue::editor().cancelAll(owner);
    pending.clear();
}
