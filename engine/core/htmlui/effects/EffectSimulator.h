// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTSIMULATOR_H
#define HTMLUI_EFFECTS_EFFECTSIMULATOR_H

// Stateful effects (phase 6, docs/html-ui/phase-6-stateful-effects.md 3.3-3.6): the state images of
// the simulations and their pool. The passes themselves run in EffectRenderer
// (EffectSimulator.cpp), which owns the programs and draws with the RmlRenderer's passes.

#include "Export.h"
#include "EffectTypes.h"
#include "render/Render.h"
#include "texture/Framebuffer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine::htmlui {

    ColorFormat colorFormatOf(EffectStateFormat format);

    // One state image: a 1-sample colour render target without depth
    struct EffectStateImage {
        Framebuffer framebuffer;
        EffectStateFormat format = EffectStateFormat::RGBA16F;
        int width = 0, height = 0;
        bool linear = true, repeat = false;
        size_t bytes = 0;
        uint64_t releasedFrame = 0;     // the pool's frame it was released in (pooled images)
        bool cleared = false;           // holds zeros (fresh, or cleared since it was pooled)
    };

    // The process's state images (main thread). A released image is pooled by its (format, size,
    // filter, wrap) and reused by the next acquire of that key; pooled images idle for 120 frames,
    // or beyond 64 MB pooled, are destroyed. destroyAll() (a view loss, shutdown) destroys every
    // image, live ones too: simulations notice the generation change and restart.
    class JASPERENGINE_INTERNAL EffectStatePool {
    public:
        static EffectStatePool& get();

        std::shared_ptr<EffectStateImage> acquire(EffectStateFormat format, int width, int height, bool linear, bool repeat);
        void release(std::shared_ptr<EffectStateImage> image);
        // destroys pooled images idle for at least `idleFrames` frames (0: every pooled image)
        void trim(uint64_t idleFrames);
        // once a frame (EffectRuntime::beginFrame): trims idle and excess pooled images
        void beginFrame();
        void destroyAll();
        uint64_t generation() const { return generationCounter; }

        size_t liveImages() const { return live; }
        size_t liveBytes() const { return liveBytesCount; }
        size_t pooledImages() const { return pooled.size(); }
        size_t pooledBytes() const;
        uint64_t createdImages() const { return created; }

        // the device's support for a format (render, and blend for additive passes)
        static bool supports(EffectStateFormat format, bool blend);
        // tests: formats reported unsupported (bit = (int)EffectStateFormat)
        static void setUnsupportedFormatsForTesting(uint32_t mask);

        static constexpr uint64_t kIdleFrames = 120;
        static constexpr size_t kMaxPooledBytes = 64u * 1024u * 1024u;

    private:
        EffectStatePool() = default;
        std::vector<std::shared_ptr<EffectStateImage>> pooled;
        size_t live = 0;
        size_t liveBytesCount = 0;
        uint64_t created = 0;
        uint64_t frame = 0;
        uint64_t generationCounter = 1;
    };

}

#endif //HTMLUI_EFFECTS_EFFECTSIMULATOR_H
