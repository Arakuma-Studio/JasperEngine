// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R18-R35, R41-R43, §6.4-§6.7)
// - Purpose: the GPU side of a Dimension 3D layer, owned by the renderer's services (made by
//   the first 3D layer a renderer plans, so a document without 3D creates nothing: R45). One
//   render of a layer region is: the main pass (Title Lit, depth tested, RGBA16F with MSAA 4 at
//   k = 1 and 2 above, or the compressed-light SRGB8A8 path where float targets do not render,
//   blend or multisample, or when the tier says so), split around the gem behind copy when a
//   gem refracts the layer's own opaque parts (R21: the resolved opaque image box-mipped into
//   RGBA16F); the AO prepass, SAO and its two depth-aware blurs at output resolution (R31); the
//   glint (R32, Glint3D.h) on the pre-tone-map image; the tone map (AO multiplied, glint added,
//   the MSAA shoulder expanded, compressed light decoded) on straight colour x exposure, then
//   Mitchell H and V from k x down into the node target S80 composites (premultiplied, display
//   referred). Without float render targets (R30: the device, or TITLE_ART_TEST_NO_FLOAT_TARGETS)
//   no pass renders into a float target: the AO prepass is packed into RGBA8 and its AO is RGBA8,
//   the behind copy is SRGB8A8 in the compressed-light encoding, the glint runs on SRGB8A8.
// - Environments: where RGBA16F renders (and not under TITLE_ART_TEST_NO_FLOAT_TARGETS), built on
//   the GPU as below; else built on the builder's CPU twin (buildTitleEnvironmentCpu) on a
//   background job and uploaded as an RGBA16F cube that is only sampled (as a cooked cube is), cached
//   apart from the GPU-built one. On the GPU (title_env.frag): linear RGBA16F cubes, procedural presets
//   evaluated per texel and box-filtered per level, then GGX-prefiltered into levels down to
//   8 px; cached by (scene, face, builder version), 3 at 1024 and 1 at 2048. A photo preset whose
//   HDRI is not available renders the studio with an `environment_missing` problem. A build is
//   spread over frames: `ready()` runs a share of its tile passes (a work budget that adapts to
//   how long the calls take) and reports false until the cube is complete, so a large cube never
//   stalls a frame (a software rasteriser took 21 s in one). A part whose material overrides the
//   environment draws with its own cube (per-part environments).
// - Meshes: uploaded once per geometry key; the materials' slot table, ramps (a 256 x 16 RGBA8
//   atlas) and span table (RGBA32F, texelFetch) per render.
// - Programs build through ShaderPool (ShaderType::TITLE_LIT / TITLE_3D_POST / TITLE_ENV); a
//   render reports `ready() == false` while one compiles, and the job waits.
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "titleart/render/TitleTargetPool.h"
#include "titleart/three_d/Title3DMath.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "titleart/three_d/TitleMesh.h"
#include "titleart/three_d/TitleTierFeatures.h"
#include "titleart/three_d/TitlePicture3D.h"
#include "jobs/JobSystem.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class TextureRender;
}

namespace jasperengine::titleart {

    class TitleRenderServices;
    class TitleTarget;
    struct TitleCacheEntry;

    // the 3D path's fallback flag (RenderRequest::testFlags) beside S80's: float targets without
    // MSAA (S80's TITLE_ART_TEST_NO_FLOAT_TARGETS forces the compressed-light path as well)
    constexpr uint32_t TITLE_ART_TEST_NO_FLOAT_MSAA = 1u << 16;

    // one draw slot's material: its index in the layer's table, its Title Lit bits and its
    // environment (an index in Title3DPlan::environments)
    struct Title3DSlotMaterial {
        uint8_t material = 0;
        uint32_t bits = 0;
        uint8_t environment = 0;
    };

    // an environment a layer or one of its parts lights with
    struct Title3DEnvPlan {
        Environment3D environment;                      // the grade and the source
        const TitleEnvScene* scene = nullptr;           // the preset's (studio when its source is missing)
        bool missing = false;
        std::shared_ptr<const TitleHdri> hdri;          // a photo or softbox preset's panorama
        float hdriYaw = 0.0f;
        int face = 1024;
        uint64_t key = 0;
        std::shared_ptr<TitleCacheEntry> matcap;        // the matcap environment's image
        std::shared_ptr<const std::vector<uint8_t>> cooked;    // a cooked KTX2 cube: uploaded, not built
    };

    // render_title's debug_view (RenderRequest::debugView3D)
    enum class Title3DDebugView : uint8_t { None, Normals, Environment, AO, Glint, Ink, Wireframe };

    // everything a 3D layer's render needs, made at planning (main thread, no GPU)
    struct Title3DPlan {
        bool valid = false;
        std::string layerId;
        std::shared_ptr<const TitleMesh> mesh;
        Title3D style;
        Lighting3D lighting;
        std::vector<TitleMaterial3D> materials;         // <= 16
        std::vector<Title3DSlotMaterial> slots;         // by mesh slot
        std::vector<Mat4> partRigs;                     // per mesh part: its rig matrix (identity when none)
        Mat4 model;
        Camera3D camera;
        int canvasWidth = 0, canvasHeight = 0;
        double size = 200;
        TitleTier tier = TitleTier::Bake;
        Title3DDebugView debugView = Title3DDebugView::None;    // no tone map, no shoulder when set
        int k = 1;                                      // supersampling
        int supersample = 3;                            // the document's (Bake's k cap)
        std::vector<Title3DEnvPlan> environments;       // [0]: the layer's; then the parts' own
        std::vector<std::shared_ptr<TitleCacheEntry>> baseTextures;     // per material (null: none)
        std::vector<std::shared_ptr<TitleCacheEntry>> patternTextures;  // per material: a custom pattern's image
        bool ao = false;                                // the AO passes run (the tier allows it and it is on)
        AO3DParams aoParams;
        GlintMode glint = GlintMode::Off;               // the tier's glint chain
        Glint3DParams glintParams;
        double layerScale = 1;                          // world units per layer unit (the model's scale)
        bool behind = false;                            // a gem refracts the opaque parts (the behind copy runs)
        // R1a: the layer's own picture, made on a job (null when the layer has none)
        std::shared_ptr<jobs::Task<std::shared_ptr<TitlePicture3D>>> picture;
        double faceImage = 1, edgeImage = 0;
    };

    struct Title3DStats {
        uint64_t renders = 0;
        uint64_t mainPasses = 0;
        uint64_t postPasses = 0;
        uint64_t environmentBuilds = 0;
        uint64_t cookedEnvironments = 0;    // cooked cubes uploaded
        uint64_t environmentSteps = 0;      // ready() calls that advanced a cube build
        uint64_t environmentMaxStepUnits = 0;   // the most build work one call ran (texel samples)
        uint64_t meshUploads = 0;
        uint64_t aoPasses = 0;
        uint64_t behindCopies = 0;
        uint64_t glintPasses = 0;
        uint64_t environmentCpuBuilds = 0;  // cubes built on the builder's CPU twin (no float cube targets)
        uint64_t floatRenderTargets = 0;    // float render targets made outside the pool (cubes, the behind copy)
    };

    // the share of an environment build one ready() call starts with (texel x samples); the
    // share doubles while calls stay short and halves when one takes long
    constexpr uint64_t TITLE_ENV_STEP_UNITS = 16u << 20;
    constexpr uint64_t TITLE_ENV_STEP_UNITS_MAX = 512u << 20;
    // the red control of the spread build (RenderRequest::testFlags): the whole cube in one call
    constexpr uint32_t TITLE_ART_TEST_ENV_ONE_SHOT = 1u << 17;
    // the red control of the gem behind copy: gems refract the environment only
    constexpr uint32_t TITLE_ART_TEST_NO_BEHIND = 1u << 18;
    // a player built without the mesher: only cooked meshes (RenderRequest::cookedMeshes3D) draw
    constexpr uint32_t TITLE_ART_TEST_NO_MESHER = 1u << 19;

    class JASPERENGINE_API Title3DRenderer {
    public:
        Title3DRenderer();
        ~Title3DRenderer();
        Title3DRenderer(const Title3DRenderer&) = delete;
        Title3DRenderer& operator=(const Title3DRenderer&) = delete;

        // false while a program of the plan compiles or an environment of it is being built (each
        // call runs a share of the build)
        bool ready(const Title3DPlan& plan, uint32_t testFlags);
        // the layer region `region` (output px of the canvas at `pxScale`) into `out` (an S80
        // node target of the region's size); false with `error`
        bool render(const Title3DPlan& plan, const TitleRectI& region, double pxScale, const std::vector<std::string>& parts,
                    TitleRenderServices& services, TitleTarget* out, uint32_t testFlags, std::string& error);

        // the passes a render of the plan runs (steps' estimates, §6.7's pass budgets)
        static uint32_t passCount(const Title3DPlan& plan);
        // whether the main pass of this device and request holds compressed light
        static bool compressedLight(const Title3DPlan& plan, uint32_t testFlags);
        const Title3DStats& stats() const { return counters; }

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        Title3DStats counters;
    };

    // the renderer of a services object (made on first use)
    JASPERENGINE_API Title3DRenderer& title3DRenderer(TitleRenderServices& services);

}
