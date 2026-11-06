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

TitleArtPreviewJob::TitleArtPreviewJob(Project* project, titleart::RenderRequest request) : project(project), request(std::move(request)) {
}

uint32_t TitleArtPreviewJob::nextStepCostUs() const {
    if (finished || failed()) return 0;
    // before the job exists: the cost of building it is small, its first step is measured then
    return job ? std::max<uint32_t>(1, job->nextStepCostUs()) : 500;
}

bool TitleArtPreviewJob::step(uint32_t budgetUs) {
    if (finished) return true;
    if (failed()) return true;
    if (!job) {
        std::string error;
        job = TitleArtRenderHost::editor().begin(project, request, error);
        if (!job) {
            failure = error;
            return true;
        }
    }
    titleart::StepBudget budget;
    budget.budgetUs = std::max<uint32_t>(1, budgetUs);
    job->step(budget);
    if (job->failed()) {
        failure = job->error();
        return true;
    }
    if (!job->done()) return false;
    std::string error;
    if (!TitleArtRenderHost::readBack(*job, 8, result, error)) {
        failure = error;
        return true;
    }
    job.reset();
    finished = true;
    return true;
}

uint64_t editor::requestTitleArtPreview(Project* project, const fs::path& document, int side, TitlePreviewPriority priority,
                                        TitlePreviewOwner owner, bool visible, std::string& error) {
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
    for (size_t i = 0; i < pending.size();) {
        Pending& item = pending[i];
        bool finished = false, ok = false;
        texsynth::Image image;
        if (item.id == 0) {
            // the fresh main output, thumbnailed like an image (a worker-free CPU downscale)
            if (titleArtFreshOutputImage(project, item.document, item.side, image)) {
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
