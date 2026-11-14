// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIIMAGECACHE_H
#define HTMLUIIMAGECACHE_H

#include "Export.h"
#include "render/Render.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class TextureRender;

    // The part of an image file a URL fragment names (S93 R20, D10): #xywh=x,y,w,h (image px),
    // #xywh=percent:x,y,w,h (Media Fragments URI 1.0) or #sprite=<name> (a frame of the texture's
    // Sprite Mode sheet, S45). Cropped into its own texture, so repeat modes see only the region.
    struct JASPERENGINE_API HtmlUIImageRegion {
        enum class Kind : uint8_t { NONE, PIXELS, PERCENT, SPRITE };
        Kind kind = Kind::NONE;
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        std::string sprite;

        bool empty() const { return kind == Kind::NONE; }
        // the fragment as written in a key ("xywh=1,2,3,4", "sprite=name"; "" for none)
        std::string keyText() const;
        // "xywh=...", "xywh=pixel:...", "xywh=percent:...", "sprite=..." (percent-encoded names
        // decoded), with or without the leading '#'. False (and error set) for a malformed xywh or
        // sprite; another fragment (an SVG's "#id") or "" is no region, as Media Fragments ignore it
        static bool parse(const std::string& fragment, HtmlUIImageRegion& out, std::string* error = nullptr);
    };

    // One process-wide cache of the HTML UI's image files (S93 R18, R19, D8, D9): every context and
    // renderer (screen, texture-mode, world documents) shares the GPU texture of a (file, file
    // stamp, filter, wrap, region). LoadTexture reads only the file's header (the dimensions for
    // layout); the decode and premultiply run on the Background lane and the upload on the main
    // thread when the image is first drawn after that. A deterministic clock (play fixed_fps, a
    // fixed UI or effect time, the tests' switch), decoding="sync" and builds without worker
    // threads decode at once. Released images stay in an LRU bounded by the budget
    // (graphics.html_ui_image_cache_mib; 0 = none kept). Hot reload drops a path's images.
    // Main thread only.
    class JASPERENGINE_API HtmlUIImageCache {
    public:
        struct Entry;

        struct Request {
            std::string path;                       // the file (an asset path, asset:// or absolute), no query or fragment
            HtmlUIImageRegion region;
            TextureFilter filter = TextureFilter::LINEAR;
            bool repeatX = false, repeatY = false;
            bool sync = false;                      // decoding="sync": decoded before acquire returns
        };

        // A holder's reference: the image stays (not released into the LRU) while one exists
        class JASPERENGINE_API Reference {
        public:
            explicit Reference(std::shared_ptr<Entry> entry);
            ~Reference();
            Reference(const Reference&) = delete;
            Reference& operator=(const Reference&) = delete;

            // texels of the texture (the region's size for a region)
            int getWidth() const;
            int getHeight() const;
            // the GPU texture once uploaded, else nullptr; uploads it when its decode is done (a
            // view must exist). failed(): the decode failed (logged once), it never draws
            TextureRender* texture();
            bool pending() const;
            // its decode is still running (pending() and not decoded yet)
            bool decoding() const;
            bool failed() const;
            // decodes now if it is still pending (decoding="sync", a deterministic clock)
            void finish();

        private:
            std::shared_ptr<Entry> entry;
        };

        // what an acquire did, for the renderer's statistics
        struct AcquireInfo {
            bool cacheHit = false;     // an existing image (pending, decoded or uploaded) was reused
            bool decoded = false;      // a decode was started (or run) for it
            bool regionFailed = false; // the region is malformed, out of bounds or an unknown sprite
        };

        static HtmlUIImageCache& get();

        // A reference to the image of a request: from the cache, or a new one whose header was
        // read (its decode started). nullptr, with error set, when the file cannot be read, is not
        // an image, or the region is out of bounds or names an unknown sprite.
        std::shared_ptr<Reference> acquire(const Request& request, std::string* error = nullptr, AcquireInfo* info = nullptr);

        // Prewarm (S93 R15): decodes the image on the Background lane and uploads it within the
        // frame budget of update(); the job holds it until the job is dropped (then the LRU).
        // done once uploaded or failed; onDone runs on the main thread, in update()
        struct PrewarmJob {
            std::vector<std::shared_ptr<Reference>> images;
            size_t total = 0;
            size_t completed = 0;
            bool done = false;
            std::function<void()> onDone;
            float progress() const { return total ? (float)completed / (float)total : 1.0f; }
        };
        std::shared_ptr<PrewarmJob> prewarm(const std::vector<std::string>& paths);
        // the main-thread part of prewarm jobs: uploads within budgetMs (at least one image a
        // frame), completes jobs; runs at the start of every frame while jobs are open
        void update(double budgetMs = 2.0);

        // MiB of released images kept; negative: the platform's default (64 on desktop, 32 on
        // Android, iOS and the web); 0 keeps none
        static void setBudgetMiB(int mib);
        static size_t getBudgetBytes();
        static int defaultBudgetMiB();
        // tests: decode at once, as a deterministic clock does
        static void setSynchronousForTesting(bool synchronous);
        // decodes run at once now (deterministic clock, tests, no worker threads)
        static bool decodesSynchronously();

        // the reason of the last region that drew nothing (R20), taken once: compat adds it to the
        // document's diagnostics after its LoadTexture call
        void setLastFailure(const std::string& reason) { lastFailure = reason; }
        std::string takeLastFailure() { std::string reason; reason.swap(lastFailure); return reason; }

        // hot reload: the path's images are not reused (holders keep theirs until they release it)
        void invalidate(const std::string& path);
        // the view went (every GPU object with it): released images go, held ones upload again
        void releaseGpuResources();
        // hears the view go while no HtmlUI context is up (HtmlUI's own view-loss handler calls releaseGpuResources
        // otherwise): from the first upload, again at HtmlUI's shutdown
        void watchViewLoss();
        // every released image goes (shutdown, tests)
        void clear();

        // counters since the process started (tests, inspect_ui through the renderers' stats)
        struct Counters {
            uint64_t decodes = 0;
            uint64_t uploads = 0;
            uint64_t hits = 0;
        };
        const Counters& getCounters() const { return counters; }
        size_t getEntryCount() const;
        size_t getReleasedBytes() const;
        // images whose decode is still running (screenshots and isAllLoaded wait for them; a decoded
        // image uploads in the first frame that draws it)
        size_t getDecodingCount() const;
        // sokol ids of the cache's textures (tests)
        void getGpuHandles(std::vector<uint32_t>& images) const;

        HtmlUIImageCache(const HtmlUIImageCache&) = delete;
        HtmlUIImageCache& operator=(const HtmlUIImageCache&) = delete;

    private:
        HtmlUIImageCache();
        ~HtmlUIImageCache();

        friend class Reference;
        void onReleased(Entry& entry);
        void trim();
        void forget(Entry& entry);
        void scheduleTick();
        static constexpr const char* viewLossTag = "jasperengine::HtmlUIImageCache_onViewDestroyed";

        struct Source;
        std::map<std::string, std::shared_ptr<Entry>> entries;      // by key
        std::map<std::string, std::weak_ptr<Source>> sources;       // decodes by file key
        std::map<std::string, std::shared_ptr<Source>> keptSources; // decoded atlases regions are cut from (LRU)
        std::vector<std::shared_ptr<PrewarmJob>> prewarms;
        uint64_t releaseTick = 0;
        Counters counters;
        std::string lastFailure;
    };

}

#endif //HTMLUIIMAGECACHE_H
