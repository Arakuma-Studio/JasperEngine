// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R18-R35, R41-R43, §6.4-§6.7)
// - Purpose: the GPU side of a Dimension 3D layer, owned by the renderer's services (made by
//   the first 3D layer a renderer plans, so a document without 3D creates nothing: R45). One
//   render of a layer region is: the main pass (Title Lit, depth tested, RGBA16F with MSAA 4 at
//   k = 1 and 2 above, or the compressed-light SRGB8A8 path where float targets do not render,
//   blend or multisample, or when the tier says so), the tone map (the MSAA shoulder expanded,
//   compressed light decoded) on straight colour x exposure, then Mitchell H and V from k x down
//   into the node target S80 composites (premultiplied, display referred).
// - Environments: built on the GPU (title_env.frag) as linear RGBA16F cubes, procedural presets
//   evaluated per texel and box-filtered per level, then GGX-prefiltered into levels down to
//   8 px; cached by (scene, face, builder version), 3 at 1024 and 1 at 2048. A photo preset whose
//   HDRI is not available renders the studio with an `environment_missing` problem.
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

    // one draw slot's material: its index in the layer's table and its Title Lit bits
    struct Title3DSlotMaterial {
        uint8_t material = 0;
        uint32_t bits = 0;
    };

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
        int k = 1;                                      // supersampling
        int supersample = 3;                            // the document's (Bake's k cap)
        int environmentFace = 1024;
        const TitleEnvScene* scene = nullptr;           // the environment's (studio when its source is missing)
        bool environmentMissing = false;
        uint64_t environmentKey = 0;
        std::shared_ptr<const TitleHdri> hdri;                          // a photo or softbox preset's panorama
        float hdriYaw = 0.0f;
        std::vector<std::shared_ptr<TitleCacheEntry>> baseTextures;     // per material (null: none)
        std::shared_ptr<TitleCacheEntry> matcap;                        // the matcap environment's image
    };

    struct Title3DStats {
        uint64_t renders = 0;
        uint64_t mainPasses = 0;
        uint64_t postPasses = 0;
        uint64_t environmentBuilds = 0;
        uint64_t meshUploads = 0;
    };

    class JASPERENGINE_API Title3DRenderer {
    public:
        Title3DRenderer();
        ~Title3DRenderer();
        Title3DRenderer(const Title3DRenderer&) = delete;
        Title3DRenderer& operator=(const Title3DRenderer&) = delete;

        // false while a program of the plan compiles
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
