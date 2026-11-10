// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R54-R56)
// - Purpose: the font picker's and the Font row's preview strips: a line of text set in a font
//   file (a project asset, a system file, a Google download in the user cache) with S80's shaper
//   (TitleTextLayout: HarfBuzz, the layer's language, vertical when the layer is, its variation
//   axes, its Fallback Fonts), its hb-draw outlines rasterized on the CPU by S80's ThorVG
//   rasterizer (geometry::rasterizeElement) into straight RGBA8 in one colour; code points no
//   face covers are drawn as a red box with a cross. ImGui's font atlas is never used for a font
//   preview: it has no shaping and no axes.
// - Frames: each frame the caller asks for the strips of its visible rows (request); a strip not
//   asked for in a frame is dropped from the work queue at endFrame. Rasterization runs on the
//   jobs' Background lane, one strip at a time (one strand: the HarfBuzz faces are not shared
//   between threads); finished strips become textures on the main thread, at most 2 per frame,
//   kept in an LRU of 512. An evicted texture is destroyed at the next beginFrame, after the
//   frame that may still draw it.
//
//       raster.beginFrame();
//       for (each visible row)
//           if (const FontPreviewRaster::Strip* strip = raster.request(request))
//               ImGui::Image(strip->texture, ImVec2(strip->width, strip->height));
//           else drawPlaceholder();
//       raster.endFrame();
//
// - Thread-safety: the class from the main thread; rasterizeStrip is a pure CPU function for
//   any thread (one call at a time per font file, as HarfBuzz is built without thread safety).

#pragma once

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor::titlefonts {

    struct FontFileRef {
        std::string path;           // absolute, UTF-8
        int faceIndex = 0;
        friend bool operator==(const FontFileRef& a, const FontFileRef& b) { return a.path == b.path && a.faceIndex == b.faceIndex; }
    };

    struct StripRequest {
        FontFileRef font;
        std::vector<FontFileRef> fallbacks;         // the layer's Fallback Fonts
        std::string text;                           // UTF-8, one line (newlines become spaces)
        std::string language;                       // BCP 47, "" = from the text
        bool vertical = false;
        std::map<std::string, double> variations;   // axis tag -> value
        float pixelSize = 18.0f;                    // the em size, pixels
        uint32_t color = 0xFFFFFFFFu;               // glyphs, RGBA (R in the high byte)
        int maxWidth = 1024;                        // pixels (vertical: the height); the rest is cut

        std::string key() const;
    };

    struct StripImage {
        bool ok = false;
        std::string error;                          // the font could not be read
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;                  // straight RGBA8, rows top first
        float baseline = 0.0f;                      // pixels from the top (horizontal strips)
        std::vector<uint32_t> missing;              // code points drawn as the red box, in text order, distinct
    };

    // shapes, outlines and rasterizes one strip (blocking; the Background lane's work)
    StripImage rasterizeStrip(const StripRequest& request);

    class FontPreviewRaster {
    public:
        // texture is null when the font could not be read or the texture not made (error says why)
        struct Strip {
            ImTextureID texture{};
            int width = 0, height = 0;
            float baseline = 0.0f;
            std::vector<uint32_t> missing;
            std::string error;
        };

        // a texture made from strip pixels; null when it could not be made (tests give their own)
        struct Uploaded {
            virtual ~Uploaded() = default;
            virtual ImTextureID texture() const = 0;
        };
        using Uploader = std::function<std::unique_ptr<Uploaded>(const StripImage& image)>;
        // the rasterization (tests count or replace it); default rasterizeStrip on the Background lane
        using Rasterizer = std::function<StripImage(const StripRequest& request)>;

        static constexpr size_t DEFAULT_CAPACITY = 512;
        static constexpr int DEFAULT_UPLOADS_PER_FRAME = 2;

        explicit FontPreviewRaster(size_t capacity = DEFAULT_CAPACITY, int uploadsPerFrame = DEFAULT_UPLOADS_PER_FRAME);
        ~FontPreviewRaster();
        FontPreviewRaster(const FontPreviewRaster&) = delete;
        FontPreviewRaster& operator=(const FontPreviewRaster&) = delete;

        // main thread, before the frame's requests: destroys the textures evicted last frame
        void beginFrame();
        // the strip when it is ready, else null (its rasterization is queued or running; a
        // finished one is uploaded here while the frame's upload budget lasts). The pointer is
        // valid until the next request; the texture until the next beginFrame at least.
        const Strip* request(const StripRequest& request);
        // after the frame's requests: work for strips not requested this frame is cancelled
        void endFrame();
        // drops every strip and queued job (a font file changed, the window closed)
        void clear();
        // tests: waits until every queued rasterization has finished
        void waitIdle();

        size_t cachedCount() const { return cache.size(); }
        size_t pendingCount() const { return pending.size(); }
        int uploadsThisFrame() const { return uploads; }

        void setUploader(Uploader uploader);
        void setRasterizer(Rasterizer rasterizer);

    private:
        struct Job;
        struct Entry {
            Strip strip;
            std::unique_ptr<Uploaded> uploaded;
            std::list<std::string>::iterator lru;
        };

        size_t capacity;
        int uploadsPerFrame;
        int uploads = 0;
        uint64_t frame = 0;
        std::unordered_map<std::string, Entry> cache;
        std::list<std::string> lruOrder;            // most recent first
        std::unordered_map<std::string, std::shared_ptr<Job>> pending;
        std::vector<std::unique_ptr<Uploaded>> retired;
        Uploader uploader;
        Rasterizer rasterizer;
        struct Strand;
        std::unique_ptr<Strand> strand;

        void touch(Entry& entry, const std::string& key);
        void evictOverCapacity();
    };

    // the default uploader: an engine Texture (linear filtering) drawn through Backend::getImGuiTexture
    std::unique_ptr<FontPreviewRaster::Uploaded> uploadStripTexture(const StripImage& image);

}
