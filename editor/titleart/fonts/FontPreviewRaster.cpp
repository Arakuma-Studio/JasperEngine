// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontPreviewRaster.h"

#include "FontTables.h"

#include "Backend.h"
#include "jobs/JobSystem.h"
#include "render/TextureRender.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtIO.h"
#include "titleart/geometry/TitleGeometry.h"
#include "titleart/text/TitleTextLayout.h"
#include "util/FileUtils.h"
#include "util/FontFace.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <mutex>
#include <set>
#include <system_error>

namespace jasperengine::editor::titlefonts {

    namespace fs = std::filesystem;
    namespace ta = jasperengine::titleart;

    namespace {

        // ---------- the font files a strip reads ----------

        // A few recently used faces, shared by every strip (a system CJK font is tens of MB: it is
        // read once while the picker scrolls over its rows). Keyed by file, face, size and time.
        class FaceCache {
        public:
            std::shared_ptr<FontFace> load(const FontFileRef& ref, std::string& error) {
                std::error_code ec;
                const fs::path path = FileUtils::pathFromUtf8(ref.path);
                const uintmax_t size = fs::file_size(path, ec);
                const auto time = ec ? fs::file_time_type() : fs::last_write_time(path, ec);
                if (ec) {
                    error = "cannot read '" + ref.path + "'";
                    return nullptr;
                }
                const std::string key = ref.path + "#" + std::to_string(ref.faceIndex) + "#" + std::to_string(size) + "#" +
                                        std::to_string((long long)time.time_since_epoch().count());
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    for (auto it = entries.begin(); it != entries.end(); ++it) {
                        if (it->first == key) {
                            entries.splice(entries.begin(), entries, it);
                            return entries.front().second;
                        }
                    }
                }
                std::ifstream in(path, std::ios::binary);
                const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (bytes.empty()) {
                    error = "cannot read '" + ref.path + "'";
                    return nullptr;
                }
                // as every Title Art resolver: CFF2 variable faces open for shaping and outlines
                std::shared_ptr<FontFace> face = FontFace::loadMemory(bytes.data(), bytes.size(), ref.faceIndex, true, ref.path, &error,
                                                                      FontFaceRasterizer::Optional);
                if (!face) return nullptr;
                std::lock_guard<std::mutex> lock(mutex);
                entries.emplace_front(key, face);
                while (entries.size() > CAPACITY) entries.pop_back();
                return face;
            }

        private:
            static constexpr size_t CAPACITY = 16;
            std::mutex mutex;
            std::list<std::pair<std::string, std::shared_ptr<FontFace>>> entries;
        };

        FaceCache& faceCache() {
            static FaceCache cache;
            return cache;
        }

        // The strip's fonts by the keys its layer names ("f0" the font, "f1".. the fallbacks)
        class StripAssets : public ta::AssetResolver {
        public:
            std::vector<FontFileRef> files;

            ta::ResolvedTitleAsset resolve(const ta::TitleAssetRef& ref, ta::TitleAssetKind kind) const override {
                ta::ResolvedTitleAsset asset;
                const int index = indexOf(ref.path);
                if (kind != ta::TitleAssetKind::Font || index < 0) return asset;
                asset.found = true;
                asset.path = ref.path;
                asset.assetPath = files[(size_t)index].path;
                asset.revision = 1;
                return asset;
            }
            std::shared_ptr<FontFace> loadFont(const ta::ResolvedTitleAsset& asset, int, std::string* error) const override {
                const int index = indexOf(asset.path);
                if (index < 0) {
                    if (error) *error = "no such font";
                    return nullptr;
                }
                std::string why;
                std::shared_ptr<FontFace> face = faceCache().load(files[(size_t)index], why);
                if (!face && error) *error = why;
                return face;
            }
            bool readBytes(const ta::ResolvedTitleAsset&, std::vector<uint8_t>&, std::string* error) const override {
                if (error) *error = "a preview strip reads fonts only";
                return false;
            }
            bool loadImage(const ta::ResolvedTitleAsset&, ta::TitleImageData&, std::string* error) const override {
                if (error) *error = "a preview strip reads fonts only";
                return false;
            }
            // no built-in face at the end of the chain: what the font lacks shows as the red box
            std::shared_ptr<FontFace> builtinFont() const override { return nullptr; }

        private:
            int indexOf(const std::string& key) const {
                if (key.size() < 2 || key[0] != 'f') return -1;
                const int index = std::atoi(key.c_str() + 1);
                return index >= 0 && (size_t)index < files.size() ? index : -1;
            }
        };

        ta::Json fontRef(size_t index) { return ta::Json{{"path", "f" + std::to_string(index)}, {"guid", ""}}; }

        // ---------- pixels ----------

        constexpr uint8_t MISSING_RGBA[4] = {235, 52, 52, 255};

        void putOpaque(StripImage& image, int x, int y, const uint8_t rgba[4]) {
            if (x < 0 || y < 0 || x >= image.width || y >= image.height) return;
            std::memcpy(&image.rgba[((size_t)y * image.width + x) * 4], rgba, 4);
        }

        // the red box with a cross over [x0, x1) x [y0, y1) (pixels)
        void drawMissingBox(StripImage& image, double x0, double y0, double x1, double y1, double thickness) {
            const int ix0 = (int)std::floor(x0), iy0 = (int)std::floor(y0), ix1 = (int)std::ceil(x1), iy1 = (int)std::ceil(y1);
            const double w = x1 - x0, h = y1 - y0;
            if (w <= 0.0 || h <= 0.0) return;
            const double length = std::sqrt(w * w + h * h);
            for (int y = iy0; y < iy1; y++) {
                for (int x = ix0; x < ix1; x++) {
                    const double px = x + 0.5, py = y + 0.5;
                    const bool border = px - x0 < thickness || x1 - px < thickness || py - y0 < thickness || y1 - py < thickness;
                    // distance to each diagonal
                    const double d1 = std::abs((px - x0) * h - (py - y0) * w) / length;
                    const double d2 = std::abs((px - x0) * h - (y1 - py) * w) / length;
                    if (border || d1 < thickness * 0.5 || d2 < thickness * 0.5) putOpaque(image, x, y, MISSING_RGBA);
                }
            }
        }

        ta::TitleRect unite(const ta::TitleRect& a, const ta::TitleRect& b) {
            if (a.empty()) return b;
            if (b.empty()) return a;
            const double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
            const double x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
            return {x0, y0, x1 - x0, y1 - y0};
        }

    }

    std::string StripRequest::key() const {
        std::string k = font.path + "#" + std::to_string(font.faceIndex);
        for (const FontFileRef& f : fallbacks) k += "|" + f.path + "#" + std::to_string(f.faceIndex);
        char numbers[96];
        std::snprintf(numbers, sizeof(numbers), "\n%d|%.3f|%08x|%d\n", vertical ? 1 : 0, pixelSize, (unsigned)color, maxWidth);
        k += numbers;
        k += language + "\n";
        for (const auto& [tag, value] : variations) {
            char axis[48];
            std::snprintf(axis, sizeof(axis), "%s=%.4f;", tag.c_str(), value);
            k += axis;
        }
        k += "\n" + text;
        return k;
    }

    StripImage rasterizeStrip(const StripRequest& request) {
        StripImage image;
        StripAssets assets;
        assets.files.push_back(request.font);
        for (const FontFileRef& fallback : request.fallbacks) assets.files.push_back(fallback);
        // the primary face must open: a strip of fallbacks alone says nothing about the font
        std::string error;
        if (!faceCache().load(request.font, error)) {
            image.error = error.empty() ? "cannot read the font" : error;
            return image;
        }

        std::string text = request.text;
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');

        ta::Json layer = {{"type", "text"}, {"id", "0000000a"}, {"text", text}, {"font", fontRef(0)}, {"face_index", request.font.faceIndex},
                          {"size", std::clamp((double)request.pixelSize, 1.0, 512.0)}, {"language", request.language},
                          {"vertical", request.vertical}, {"align", "start"}};
        layer["fallback_fonts"] = ta::Json::array();
        for (size_t i = 0; i < request.fallbacks.size(); i++) layer["fallback_fonts"].push_back(fontRef(i + 1));
        layer["variations"] = ta::Json::object();
        for (const auto& [tag, value] : request.variations) layer["variations"][tag] = value;
        ta::Json root = {{"format", ta::TITLE_ART_FORMAT}, {"version", ta::TITLE_ART_VERSION}};
        root["layers"] = ta::Json::array();
        root["layers"].push_back(layer);
        const ta::TitleArtParseResult parsed = ta::parseTitleArtJson(root);
        if (!parsed.ok || parsed.doc.layers().empty()) {
            image.error = "the strip's layer is not valid";
            return image;
        }

        ta::TitleTextLayout layout(&assets);
        const ta::LaidOutText& laid = layout.layout(parsed.doc.layers()[0]);

        const double pad = 2.0;
        ta::TitleRect bounds = unite(laid.frame, laid.inkBounds);
        if (bounds.empty()) bounds = {0.0, -request.pixelSize * 0.25, request.pixelSize * 0.5, request.pixelSize * 1.25};
        const int limit = std::clamp(request.maxWidth, 1, 4096);
        int width = (int)std::ceil(bounds.width + 2.0 * pad);
        int height = (int)std::ceil(bounds.height + 2.0 * pad);
        if (request.vertical) height = std::min(height, limit);
        else width = std::min(width, limit);
        width = std::clamp(width, 1, 4096);
        height = std::clamp(height, 1, 4096);

        ta::geometry::MaskFrame frame;
        frame.width = width;
        frame.height = height;
        frame.scale = 1.0;
        frame.origin = {bounds.x - pad, bounds.y + bounds.height + pad};

        ta::geometry::TitleElement element;
        element.kind = ta::geometry::ElementKind::Face;
        std::set<uint32_t> seenMissing;
        for (const ta::LaidOutCluster& cluster : laid.clusters) {
            if (cluster.missing) {
                for (uint32_t cp : cluster.codepoints) {
                    if (!isIgnorableForCoverage(cp) && seenMissing.insert(cp).second) image.missing.push_back(cp);
                }
                continue;
            }
            for (const ta::LaidOutGlyph& glyph : cluster.glyphs) {
                if (!glyph.outline.empty()) element.outlines.push_back(glyph.outline);
            }
        }

        image.width = width;
        image.height = height;
        image.rgba.assign((size_t)width * height * 4, 0);
        const uint8_t r = (uint8_t)(request.color >> 24), g = (uint8_t)(request.color >> 16), b = (uint8_t)(request.color >> 8);
        const uint32_t a = request.color & 0xFF;
        if (!element.outlines.empty()) {
            const ta::geometry::CoverageMask mask = ta::geometry::rasterizeElement(element, frame);
            const size_t count = std::min((size_t)mask.width * mask.height, (size_t)width * height);
            if (mask.width == width) {
                for (size_t i = 0; i < count; i++) {
                    const uint32_t coverage = mask.pixels[i];
                    if (!coverage) continue;
                    uint8_t* p = &image.rgba[i * 4];
                    p[0] = r;
                    p[1] = g;
                    p[2] = b;
                    p[3] = (uint8_t)((coverage * a + 127) / 255);
                }
            }
        }

        // the missing clusters' boxes, inside their em cells
        const double thickness = std::max(1.0, std::round(request.pixelSize / 14.0));
        const auto toX = [&frame](double x) { return (x - frame.origin.x) * frame.scale; };
        const auto toY = [&frame](double y) { return (frame.origin.y - y) * frame.scale; };
        for (const ta::LaidOutCluster& cluster : laid.clusters) {
            if (!cluster.missing) continue;
            bool visible = false;
            for (uint32_t cp : cluster.codepoints) visible = visible || !isIgnorableForCoverage(cp);
            if (!visible) continue;
            ta::TitleRect cell = cluster.emCell;
            if (cell.empty()) {
                const double advance = cluster.advance > 0.0 ? cluster.advance : request.pixelSize * 0.6;
                cell = request.vertical ? ta::TitleRect{cluster.pen.x - request.pixelSize * 0.5, cluster.pen.y - advance, request.pixelSize, advance}
                                        : ta::TitleRect{cluster.pen.x, cluster.pen.y - request.pixelSize * 0.2, advance, request.pixelSize};
            }
            const double inset = std::min(cell.width, cell.height) * 0.1;
            drawMissingBox(image, toX(cell.x + inset), toY(cell.y + cell.height - inset), toX(cell.x + cell.width - inset), toY(cell.y + inset),
                           thickness);
        }

        image.baseline = laid.clusters.empty() ? (float)toY(0.0) : (float)toY(laid.clusters.front().pen.y);
        image.ok = true;
        return image;
    }

    // ---------- the textures ----------

    namespace {

        struct TextureStrip : FontPreviewRaster::Uploaded {
            Texture gpu;
            ImTextureID id{};
            ~TextureStrip() override { gpu.destroy(); }
            ImTextureID texture() const override { return id; }
        };

    }

    std::unique_ptr<FontPreviewRaster::Uploaded> uploadStripTexture(const StripImage& image) {
        if (!image.ok || image.width <= 0 || image.height <= 0 || image.rgba.size() != (size_t)image.width * image.height * 4) return nullptr;
        static uint64_t counter = 0;
        // TextureData frees what it is given (releaseData below)
        void* pixels = std::malloc(image.rgba.size());
        if (!pixels) return nullptr;
        std::memcpy(pixels, image.rgba.data(), image.rgba.size());
        auto strip = std::make_unique<TextureStrip>();
        TextureData data(image.width, image.height, (unsigned int)image.rgba.size(), ColorFormat::RGBA, 4, pixels);
        strip->gpu.setData("editor:title-font-strip:" + std::to_string(++counter), data);
        strip->gpu.setMinFilter(TextureFilter::LINEAR);
        strip->gpu.setMagFilter(TextureFilter::LINEAR);
        TextureRender* render = strip->gpu.getRender();
        strip->gpu.releaseData();
        if (!render || !render->isCreated()) return nullptr;
        strip->id = Backend::getImGuiTexture(render);
        return strip;
    }

    struct FontPreviewRaster::Job {
        StripRequest request;
        StripImage image;
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        jobs::Task<void> task;
        uint64_t lastRequested = 0;
    };

    struct FontPreviewRaster::Strand {
        // one strip at a time: the faces' HarfBuzz objects are not shared between threads
        jobs::SerialQueue queue{"title-font-strips", jobs::Lane::Background};
    };

    FontPreviewRaster::FontPreviewRaster(size_t capacity, int uploadsPerFrame)
        : capacity(std::max<size_t>(1, capacity)), uploadsPerFrame(std::max(1, uploadsPerFrame)), uploader(&uploadStripTexture),
          strand(std::make_unique<Strand>()) {}

    FontPreviewRaster::~FontPreviewRaster() {
        for (auto& [key, job] : pending) job->cancelled.store(true, std::memory_order_relaxed);
        strand->queue.cancelPending();
        strand.reset();   // waits for the strip running now
        cache.clear();
        retired.clear();
    }

    void FontPreviewRaster::beginFrame() {
        frame++;
        uploads = 0;
        retired.clear();
    }

    void FontPreviewRaster::touch(Entry& entry, const std::string& key) {
        lruOrder.erase(entry.lru);
        lruOrder.push_front(key);
        entry.lru = lruOrder.begin();
    }

    void FontPreviewRaster::evictOverCapacity() {
        while (cache.size() > capacity && !lruOrder.empty()) {
            const std::string key = lruOrder.back();
            lruOrder.pop_back();
            auto it = cache.find(key);
            if (it == cache.end()) continue;
            if (it->second.uploaded) retired.push_back(std::move(it->second.uploaded));
            cache.erase(it);
        }
    }

    const FontPreviewRaster::Strip* FontPreviewRaster::request(const StripRequest& stripRequest) {
        const std::string key = stripRequest.key();
        auto hit = cache.find(key);
        if (hit != cache.end()) {
            touch(hit->second, key);
            return &hit->second.strip;
        }
        auto queued = pending.find(key);
        if (queued != pending.end()) {
            const std::shared_ptr<Job> job = queued->second;
            job->lastRequested = frame;
            if (!job->done.load(std::memory_order_acquire) || uploads >= uploadsPerFrame) return nullptr;
            pending.erase(queued);
            Entry entry;
            entry.strip.width = job->image.width;
            entry.strip.height = job->image.height;
            entry.strip.baseline = job->image.baseline;
            entry.strip.missing = job->image.missing;
            entry.strip.error = job->image.error;
            if (job->image.ok) {
                uploads++;
                entry.uploaded = uploader ? uploader(job->image) : nullptr;
                if (entry.uploaded) entry.strip.texture = entry.uploaded->texture();
                else if (entry.strip.error.empty()) entry.strip.error = "the preview texture could not be made";
            }
            lruOrder.push_front(key);
            entry.lru = lruOrder.begin();
            auto inserted = cache.emplace(key, std::move(entry)).first;
            const Strip* strip = &inserted->second.strip;
            evictOverCapacity();
            // (the strip just made is the most recent: never the one evicted)
            return strip;
        }
        auto job = std::make_shared<Job>();
        job->request = stripRequest;
        job->lastRequested = frame;
        job->task = strand->queue.push(
            [job, rasterize = rasterizer]() {
                if (job->cancelled.load(std::memory_order_relaxed)) return;
                job->image = rasterize ? rasterize(job->request) : rasterizeStrip(job->request);
                job->done.store(true, std::memory_order_release);
            },
            "title-font-strip");
        pending.emplace(key, std::move(job));
        return nullptr;
    }

    void FontPreviewRaster::endFrame() {
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->second->lastRequested != frame) {
                it->second->cancelled.store(true, std::memory_order_relaxed);
                it->second->task.cancel();
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
    }

    void FontPreviewRaster::clear() {
        for (auto& [key, job] : pending) {
            job->cancelled.store(true, std::memory_order_relaxed);
            job->task.cancel();
        }
        pending.clear();
        for (auto& [key, entry] : cache) {
            if (entry.uploaded) retired.push_back(std::move(entry.uploaded));
        }
        cache.clear();
        lruOrder.clear();
    }

    void FontPreviewRaster::waitIdle() {
        strand->queue.waitIdle();
    }

    void FontPreviewRaster::setUploader(Uploader newUploader) {
        uploader = std::move(newUploader);
    }

    void FontPreviewRaster::setRasterizer(Rasterizer newRasterizer) {
        rasterizer = std::move(newRasterizer);
    }

}
