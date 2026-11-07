// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R37, R48, §6.4, §6.6, §6.7)
// - Purpose: what each quality tier keeps, as pure functions shared by the runtime renderer, the
//   exporter's shader variant collector and `check_title` (one source, so they never disagree):
//   the Title Lit variant bits of a material at a tier (titleTierFeatures, which strips features
//   by §6.7's table), the tier's geometry reductions, limits and budgets, and the mapping of the
//   project's Realtime 3D Quality and S56's device tiers onto title tiers.
// - Over-budget titles still render: the budgets drive `3d_tier_budget`, the badges and
//   getMeshStats().overBudget.
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "device/DeviceTypes.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleMesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    // ShaderType::TITLE_LIT property bits (§6.4); title_lit.frag reads the same defines
    namespace TitleLitBits {
        constexpr uint32_t RAMP = 1u << 0;
        constexpr uint32_t CLEARCOAT = 1u << 1;
        constexpr uint32_t IRIDESCENCE = 1u << 2;
        constexpr uint32_t GEM = 1u << 3;
        constexpr uint32_t GEM_BEHIND = 1u << 4;
        constexpr uint32_t DISPERSION = 1u << 5;
        constexpr uint32_t GLASS_DEPTH = 1u << 6;
        constexpr uint32_t FLAKES = 1u << 7;
        constexpr uint32_t PATTERN = 1u << 8;
        constexpr uint32_t SURFACE_SHIFT = 9;          // 2 bits: 1 crumple, 2 hammered, 3 liquid
        constexpr uint32_t SURFACE_MASK = 3u << 9;
        constexpr uint32_t ENV_LOCK = 1u << 11;
        constexpr uint32_t BASE_TEXTURE = 1u << 12;
        constexpr uint32_t WRAP_UV = 1u << 13;
        constexpr uint32_t EMISSIVE = 1u << 14;
        constexpr uint32_t TRANSPARENT = 1u << 15;
        constexpr uint32_t RIGS = 1u << 16;
        constexpr uint32_t LIGHTS = 1u << 17;
        constexpr uint32_t HDR_COMPRESS = 1u << 18;
        constexpr uint32_t OUTPUT_SHIFT = 19;          // 2 bits: 0 colour, 1 normal + depth, 2 depth only
        constexpr uint32_t OUTPUT_MASK = 3u << 19;
        constexpr uint32_t MATCAP = 1u << 21;
        constexpr uint32_t SPECULAR_AA = 1u << 22;
        constexpr uint32_t SURFACE_LITE = 1u << 23;
        constexpr uint32_t MATERIAL_MASK = RAMP | CLEARCOAT | IRIDESCENCE | GEM | GEM_BEHIND | DISPERSION | GLASS_DEPTH | FLAKES | PATTERN |
                                           SURFACE_MASK | ENV_LOCK | BASE_TEXTURE | WRAP_UV | EMISSIVE | TRANSPARENT | SPECULAR_AA | SURFACE_LITE;
    }
    // the bit names, lowest first (traces, check_title's variant lists)
    JASPERENGINE_API std::vector<std::string> titleLitBitNames(uint32_t bits);

    enum class GlintMode : uint8_t { Off, Full, Mobile, Sprites };
    JASPERENGINE_API const char* glintModeName(GlintMode mode);

    struct TitleTierFeatures {
        uint32_t litBits = 0;       // the material's Title Lit bits at the tier (RIGS, LIGHTS, HDR_COMPRESS,
                                    // OUTPUT and MATCAP are the renderer's per draw)
        GlintMode glint = GlintMode::Off;
        bool ao = false;
        bool behind = false;        // the gem behind pass
        bool partRenders = true;    // per-part effects render parts (Low: the whole layer)
    };
    JASPERENGINE_API TitleTierFeatures titleTierFeatures(const TitleMaterial3D& material, const Title3D& style, TitleTier tier);

    struct TitleTierGeometry {
        double curveScale = 1.0;    // curve segments x this
        int minCurveSegments = 2;
        int maxBevelSegments = 24;
        size_t inflateNodeCap = 200000;
        double inflateStepScale = 1.0;
    };
    JASPERENGINE_API TitleTierGeometry titleTierGeometry(TitleTier tier);
    // the curve segments a tier flattens with (draft halves them)
    JASPERENGINE_API int titleTierCurveSegments(int authored, TitleTier tier, bool draft);

    struct TitleTierLimits {
        int k = 1;                  // supersampling (Bake: the document's `supersample`)
        uint64_t renderPxCap = 0;   // region px x k^2 at most
        int environmentFace = 256;
        uint32_t maxTriangles = 0;  // 0: no limit (Bake warns above 400k)
        uint32_t maxSolids = 0;     // 0: any
        uint32_t maxPasses = 0;
        size_t targetBytes = 0;     // a title covering 30 % of 1080p
        size_t environmentBytes = 0;
        size_t meshBytes = 0;       // 0: no limit
        int msaa = 1;               // the main pass's sample count
        bool compressedLight = false;   // the main target holds compressed light (RGBA8)
    };
    JASPERENGINE_API TitleTierLimits titleTierLimits(TitleTier tier);
    constexpr uint32_t TITLE_BAKE_TRIANGLE_WARNING = 400000;

    // S56's device tier -> a title tier (Ultra and High -> High)
    JASPERENGINE_API TitleTier titleTierForDevice(DeviceTier tier);
    // the project setting's default per platform (Engine::Platform as an int: MacOS, iOS, Web,
    // Android, Linux, Windows): desktop High, iOS Medium, Android and HTML5 Low
    JASPERENGINE_API TitleTier defaultTitleTier(int platform);
    // GLES3 / WebGL2 backends clamp High to Medium
    JASPERENGINE_API TitleTier clampTitleTierForBackend(TitleTier tier, bool gles);
    // the tier a Realtime title renders at: the component's `qualityTier` ("auto", "high",
    // "medium", "low"), else S56's device tier where one applies, else the project's setting
    JASPERENGINE_API TitleTier resolveTitleTier(const std::string& qualityTier, TitleTier projectTier, bool deviceTierApplied,
                                                DeviceTier deviceTier, bool gles);

    // a mesh against a tier's limits: what is over ("triangles 45210 > 30000", ...)
    struct TitleTierFit {
        bool fits = true;
        std::vector<std::string> over;
    };
    JASPERENGINE_API TitleTierFit titleTierFit(const TitleMeshStats& stats, size_t meshBytes, TitleTier tier);

}
