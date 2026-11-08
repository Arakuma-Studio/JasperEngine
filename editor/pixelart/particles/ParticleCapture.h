// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// An effect drawn alone at fixed times (docs/specs/S49-pixel-particles.md R21, R38): a scene of its
// own, an orthographic camera one base pixel (1 / PPU) per texel over the effect's reach plus a
// margin, the system seeked to each time with its seed and read back, so the frames are the
// game's pixels at 1x on a key colour. capture_particles' contact sheets and checks and Bake to
// Sprite Sheet run on it. Main thread: step() once per editor frame until it is done.

#include "pixelart/PixelImage.h"
#include "pixelart/Rgba8.h"
#include "particles/ParticleEffect.h"
#include "math/Vector2.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
    class Camera;
    class ParticleSystem;
}

namespace jasperengine::editor::pixelart {

    struct ParticleCaptureSettings {
        particles::ParticleEffectDocument document;
        uint32_t seed = 1;
        std::vector<double> times;
        int pixelsPerUnit = 1;
        int margin = 24;                 // base pixels around the effect's reach (pixelfx's spill margin)
        jasperengine::pixelart::Rgba8 background{255, 0, 255, 255};
        int maxSide = 512;               // base pixels
    };

    // one particle at one frame, for the checks: its centre in base pixels of the frame (x right,
    // y up) and its drawn size
    struct CapturedParticle {
        float x = 0.0f, y = 0.0f;
        float size = 1.0f;
    };

    struct CapturedFrame {
        double time = 0.0;
        uint32_t count = 0;
        PixelImage image;                // the frame at 1x, rows top first
        std::vector<CapturedParticle> particles;
    };

    class ParticleCapture {
    public:
        ParticleCapture();
        ~ParticleCapture();
        ParticleCapture(const ParticleCapture&) = delete;
        ParticleCapture& operator=(const ParticleCapture&) = delete;

        bool begin(const ParticleCaptureSettings& settings, std::string& error);
        // one frame of the work; true when every frame is read (or it failed: error())
        bool step();
        bool failed() const { return !failure.empty(); }
        const std::string& error() const { return failure; }

        int width() const { return frameWidth; }
        int height() const { return frameHeight; }
        // the frame's lower-left corner in world units (the effect's emitter at the origin)
        Vector2 origin() const { return frameOrigin; }
        const std::vector<CapturedFrame>& frames() const { return captured; }
        const ParticleCaptureSettings& settings() const { return config; }
        // the effect's reach in base pixels of the frame (min x, min y, max x, max y)
        const float* reach() const { return reachPx; }

    private:
        void release();
        void seekTo(double time);
        void collectParticles(CapturedFrame& frame);

        ParticleCaptureSettings config;
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        std::unique_ptr<ParticleSystem> system;
        std::vector<CapturedFrame> captured;
        int frameWidth = 0, frameHeight = 0;
        Vector2 frameOrigin;
        float reachPx[4] = {0, 0, 0, 0};
        size_t next = 0;
        int warmFrames = 0;
        bool rendering = false;
        std::string failure;
    };

    // Bake to Sprite Sheet (S49 R21): the frames left to right at an integer scale with `padding`
    // pixels between them, the key colour made transparent
    PixelImage particleSpriteStrip(const std::vector<CapturedFrame>& frames, int frameWidth, int frameHeight, int scale, int padding,
                                   jasperengine::pixelart::Rgba8 key);
    // the strip's Aseprite json-hash (frame rects and durations, the layout `aseprite --data`
    // writes), which a texture's Sprite Mode imports (S45 R10)
    std::string particleSpriteStripJson(const std::string& imageName, const std::vector<CapturedFrame>& frames, int frameWidth,
                                        int frameHeight, int scale, int padding);

    // the frames in a grid (columns; 0: about square) at an integer scale, a 1 px separator
    // between cells and each labelled "#i t=0.25s" under it in a 3x5 pixel font
    PixelImage particleContactSheet(const std::vector<CapturedFrame>& frames, int frameWidth, int frameHeight, int scale, int columns,
                                    jasperengine::pixelart::Rgba8 background, bool labels);

}
