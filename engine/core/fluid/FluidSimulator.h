// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FLUID_SIMULATOR_H
#define FLUID_SIMULATOR_H

// The GPU side of one FluidSimulationComponent, independent of the ECS: its render targets
// (velocity RG16F, pressure/divergence/curl R16F, dye RGBA16F ping-pong pairs, the bloom chain,
// sunrays), the fixed-step clock, the seeded random source, the pointers and the queued splats,
// and the display passes into a stable output framebuffer. The kernels are the shared GLSL of
// engine/shaderlib/effects/lib/fam_fluid_solver.glsl and fam_fluid_look.glsl (the HTML UI
// `fluid` effect runs the same ones). Main thread; every call that draws runs outside any
// render pass (FluidSystem calls them from RenderSystem's frame passes).

#include "Export.h"
#include "component/FluidSimulationComponent.h"
#include "render/PipelineState.h"
#include "texture/Framebuffer.h"
#include "texture/Texture.h"

#include <cstdint>
#include <string>

namespace jasperengine{

    struct JASPERENGINE_API FluidStats{
        uint64_t steps = 0;             // fixed steps run since the last reset
        double time = 0.0;              // simulated seconds since the last reset
        uint64_t splats = 0;            // splats applied since the last reset
        int simWidth = 0, simHeight = 0;
        int dyeWidth = 0, dyeHeight = 0;
        size_t gpuBytes = 0;            // every target this simulator owns
        int passesLastFrame = 0;        // fullscreen passes drawn in the last frame
        uint32_t seed = 0;              // the seed in use
        bool shadingActive = false, bloomActive = false, sunraysActive = false;  // after fallbacks
        // the device's fallbacks in use: the advection filters by hand (no linear filtering of
        // half floats: shading, bloom and sunrays off); compact: velocity RG16F and the scalar
        // fields R16F (else RGBA16F, Pavel's fallback)
        bool manualFiltering = false, compactFormats = false;
        int outputWidth = 0, outputHeight = 0;
    };

    // FluidSimulator::setDisabledFeaturesForTesting: device features reported missing
    enum FluidTestFeature : uint32_t {
        FLUID_TEST_NO_FLOAT_TARGETS = 1u << 0,     // no renderable half floats: not supported
        FLUID_TEST_NO_RG16F = 1u << 1,             // RG16F neither renderable nor filterable
        FLUID_TEST_NO_R16F = 1u << 2,              // R16F neither renderable nor filterable
        FLUID_TEST_NO_LINEAR_FILTER = 1u << 3      // half floats not filterable: manual filtering
    };

    class JASPERENGINE_API FluidSimulator{
    public:
        FluidSimulator();
        ~FluidSimulator();
        FluidSimulator(const FluidSimulator&) = delete;
        FluidSimulator& operator=(const FluidSimulator&) = delete;

        // the device can run a fluid (float targets renderable and filterable); else why not
        static bool isSupported(std::string* reason = nullptr);

        // Once a frame: (re)creates the targets for these settings and aspect (width / height),
        // restarts on component.needReset, applies the queued splats and bursts (also while
        // paused) and pointer splats, runs the clock's fixed steps for `dt` seconds (already
        // time-scaled by the caller) and draws the output. False: nothing could be drawn
        // (getError()).
        bool update(FluidSimulationComponent& component, float aspectRatio, double dt);

        // restarts: zero fields, the seed (0: a new one), the start burst
        void reset(const FluidSimulationComponent& component);
        // runs exactly `steps` fixed steps now and redraws the output (deterministic stepping)
        void step(FluidSimulationComponent& component, float aspectRatio, int steps);

        // pointers (id: 0 the mouse, touches their own); positions in fluid coordinates.
        // Strokes are interpolated across the frame's steps.
        void pointerDown(int id, Vector2 position);
        void pointerMove(int id, Vector2 position);
        void pointerUp(int id);
        void pointerCancelAll();

        // the drawn image (RGBA8, premultiplied by its alpha in transparent mode). The object is
        // stable for the simulator's lifetime; its version changes when it is recreated
        Framebuffer* getOutputFramebuffer();
        Texture& getOutputTexture();
        // the raw fields (current read side): dye RGBA16F (HDR), velocity RG16F (sim texels / s)
        Framebuffer* getDyeFramebuffer();
        Framebuffer* getVelocityFramebuffer();

        const FluidStats& getStats() const;
        const std::string& getError() const;

        // frees every GPU resource (a view loss, removal); the next update recreates them
        // (restarted: the fields are gone)
        void release();

        // Draws the output over the target of the render pass open now (a premultiplied
        // fullscreen triangle in its viewport; FluidSystem's SCREEN composite). state: the
        // pass's target format (its blend is set here); flipY: the target is drawn Y-flipped
        // (GL offscreen targets, RenderSystem::isRenderingFlipped). False: nothing drawn
        bool drawComposite(const PipelineState& state, bool flipY);

        // tests: device features reported missing (FluidTestFeature bits, 0: none), for every
        // simulator; their targets are made again at the next update
        static void setDisabledFeaturesForTesting(uint32_t features);

    private:
        struct Impl;
        Impl* impl;
    };

}

#endif // FLUID_SIMULATOR_H
