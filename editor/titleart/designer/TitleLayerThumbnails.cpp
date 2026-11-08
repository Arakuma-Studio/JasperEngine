// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLayerThumbnails.h"

#include "Backend.h"
#include "render/preview/TitleArtPreviewRender.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

TitleLayerThumbnails::TitleLayerThumbnails(Project* project, TitlePreviewOwner owner) : project(project), owner(owner) {
}

TitleLayerThumbnails::~TitleLayerThumbnails() {
    cancelAll();
}

double TitleLayerThumbnails::now() const {
    if (clockFn) return clockFn();
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void TitleLayerThumbnails::touch(const std::string& slot) {
    recency.remove(slot);
    recency.push_front(slot);
    while (recency.size() > MAX_TEXTURES) {
        const std::string oldest = recency.back();
        recency.pop_back();
        auto it = entries.find(oldest);
        if (it != entries.end()) {
            if (it->second.request) TitlePreviewQueue::editor().cancel(it->second.request);
            it->second.texture.destroy();
            entries.erase(it);
        }
    }
}

ImTextureID TitleLayerThumbnails::thumbnail(const std::shared_ptr<const ta::TitleArtDoc>& doc, const std::string& layerId, const ta::TitleRect& bounds,
                                            uint64_t content, int pixels, bool visible) {
    return thumbnail(layerId, doc, layerId, bounds, content, pixels, visible, TitlePreviewPriority::Layers);
}

ImVec2 TitleLayerThumbnails::pictureSize(const std::string& slot) const {
    auto it = entries.find(slot);
    if (it == entries.end() || it->second.key.empty()) return ImVec2(0, 0);
    return ImVec2((float)it->second.width, (float)it->second.height);
}

bool TitleLayerThumbnails::pending(const std::string& slot) const {
    auto it = entries.find(slot);
    return it != entries.end() && (it->second.request != 0 || it->second.key != it->second.wantedKey);
}

ImTextureID TitleLayerThumbnails::thumbnail(const std::string& slot, const std::shared_ptr<const ta::TitleArtDoc>& doc, const std::string& layerId,
                                            const ta::TitleRect& bounds, uint64_t content, int pixels, bool visible, TitlePreviewPriority priority) {
    if (!doc || layerId.empty() || slot.empty()) return 0;
    Entry& entry = entries[slot];
    touch(slot);
    const std::string key = doc->guid() + ":" + slot + ":" + layerId + ":" + std::to_string(content) + ":" + std::to_string(doc->canvasWidth()) + "x" +
                            std::to_string(doc->canvasHeight()) + ":" + std::to_string(pixels);
    const double t = now();
    if (key != entry.wantedKey) {
        entry.wantedKey = key;
        entry.wantedSince = t;
    }
    entry.doc = doc;
    entry.bounds = bounds;
    entry.side = pixels;
    if (entry.request) TitlePreviewQueue::editor().setVisible(entry.request, visible);
    // a settled new key asks for a render (the shown picture stays until it is done)
    if (visible && entry.wantedKey != entry.key && entry.wantedKey != entry.requestKey && t - entry.wantedSince >= SETTLE_SECONDS && !bounds.empty()) {
        if (entry.request) TitlePreviewQueue::editor().cancel(entry.request);
        ta::RenderRequest request;
        request.doc = doc;
        request.isolateLayer = layerId;
        // the layer's reach as the region, at a scale that fits `pixels`
        const double scale = (double)pixels / std::max(1.0, std::max(bounds.width, bounds.height));
        request.pxScale = (float)std::clamp(scale, 0.01, 4.0);
        const double left = bounds.x + doc->canvasWidth() * 0.5;
        const double top = doc->canvasHeight() * 0.5 - (bounds.y + bounds.height);
        request.region = {(int)std::floor(left), (int)std::floor(top), (int)std::ceil(bounds.width) + 1, (int)std::ceil(bounds.height) + 1};
        Project* p = project;
        entry.request = TitlePreviewQueue::editor().request(
            priority, "layerthumb:" + key, [p, request]() { return std::make_unique<TitleArtPreviewJob>(p, request); }, owner, visible);
        entry.requestKey = entry.wantedKey;
        requests++;
    }
    if (entry.key.empty() || !entry.texture.getRender()) return 0;
    return Backend::getImGuiTexture(entry.texture.getRender());
}

void TitleLayerThumbnails::process() {
    for (auto& [slot, entry] : entries) {
        if (!entry.request) continue;
        const TitlePreviewState state = TitlePreviewQueue::editor().state(entry.request);
        if (state == TitlePreviewState::Failed) {
            TitlePreviewQueue::editor().cancel(entry.request);
            entry.request = 0;
            continue;
        }
        if (state != TitlePreviewState::Done) continue;
        auto* job = dynamic_cast<TitleArtPreviewJob*>(TitlePreviewQueue::editor().result(entry.request));
        if (job && job->image().image.width > 0) {
            const texsynth::Image& image = job->image().image;
            entry.pixels.assign(image.data.begin(), image.data.end());
            entry.texture.destroy();
            entry.texture.setData("editor:titledesigner-layer:" + std::to_string(++uploads),
                                  TextureData(image.width, image.height, (unsigned int)entry.pixels.size(), ColorFormat::RGBA, 4, entry.pixels.data()));
            entry.texture.load();
            entry.width = image.width;
            entry.height = image.height;
            entry.key = entry.requestKey;
        }
        TitlePreviewQueue::editor().cancel(entry.request);
        entry.request = 0;
        entry.requestKey.clear();
    }
}

void TitleLayerThumbnails::cancelAll() {
    TitlePreviewQueue::editor().cancelAll(owner);
    for (auto& [layerId, entry] : entries) {
        entry.request = 0;
        entry.requestKey.clear();
        entry.texture.destroy();
    }
    entries.clear();
    recency.clear();
}

size_t TitleLayerThumbnails::pendingCount() const {
    size_t n = 0;
    for (const auto& [layerId, entry] : entries) {
        if (entry.request) n++;
    }
    return n;
}
