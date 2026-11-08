// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R15, R67, D7)
// - Purpose: the Layers panel's 24 x 24 thumbnails. Each is that layer alone (with its effects
//   and clipped layers: RenderRequest::isolateLayer) rendered by S80's renderer through the one
//   TitlePreviewQueue at priority Layers, keyed by the layer's content hash (TitleArtHash's
//   LayerHashes::content, plus the canvas size and the pixel size), and asked for only once the
//   layer's key has been stable for 250 ms (a drag does not queue a render per frame).
// - While a new picture is pending, the previous picture of the same layer stays; a picture of
//   another layer is never shown in its place. Finished pictures live in a small GPU LRU.
// - Closing the window cancels every request (cancelAll); a device loss is the queue's (it makes
//   the jobs again), and an LRU texture that was lost is requested again.
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/TitlePreviewQueue.h"

#include "texture/Texture.h"
#include "imgui.h"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::titledesigner {

    class TitleLayerThumbnails {
    public:
        static constexpr double SETTLE_SECONDS = 0.25;
        static constexpr size_t MAX_TEXTURES = 256;

        TitleLayerThumbnails(Project* project, TitlePreviewOwner owner);
        ~TitleLayerThumbnails();
        TitleLayerThumbnails(const TitleLayerThumbnails&) = delete;
        TitleLayerThumbnails& operator=(const TitleLayerThumbnails&) = delete;

        // the picture of a layer (null while it has none yet); `bounds` the layer's reach in
        // document space, `content` its content hash, `pixels` the side to render
        ImTextureID thumbnail(const std::shared_ptr<const jasperengine::titleart::TitleArtDoc>& doc, const std::string& layerId,
                              const jasperengine::titleart::TitleRect& bounds, uint64_t content, int pixels, bool visible);
        // the same for a picture kept under its own `slot` (a Library tile: one layer of a
        // document with that tile's item applied), asked for at `priority`
        ImTextureID thumbnail(const std::string& slot, const std::shared_ptr<const jasperengine::titleart::TitleArtDoc>& doc,
                              const std::string& layerId, const jasperengine::titleart::TitleRect& bounds, uint64_t content, int pixels,
                              bool visible, TitlePreviewPriority priority);
        // the size of the shown picture of a slot (0 x 0 while it has none)
        ImVec2 pictureSize(const std::string& slot) const;
        bool pending(const std::string& slot) const;
        // main thread, every frame: uploads finished pictures
        void process();
        void cancelAll();
        size_t pendingCount() const;
        size_t requestCount() const { return requests; }
        // tests: the settle clock
        void setClock(std::function<double()> clock) { clockFn = std::move(clock); }

    private:
        struct Entry {
            std::string key;                // the shown picture's key
            Texture texture;
            std::vector<uint8_t> pixels;
            std::string wantedKey;          // the newest key and since when it is stable
            double wantedSince = 0.0;
            uint64_t request = 0;           // the queue request in flight
            std::string requestKey;
            int width = 0, height = 0;      // the shown picture's size
            std::shared_ptr<const jasperengine::titleart::TitleArtDoc> doc;
            jasperengine::titleart::TitleRect bounds;
            int side = 48;
        };
        Project* project = nullptr;
        TitlePreviewOwner owner = 0;
        std::map<std::string, Entry> entries;     // by slot (a layer id for the Layers panel)
        std::list<std::string> recency;          // most recent first
        size_t requests = 0;
        uint64_t uploads = 0;
        std::function<double()> clockFn;

        double now() const;
        void touch(const std::string& slot);
    };

}
