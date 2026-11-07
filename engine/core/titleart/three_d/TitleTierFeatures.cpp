// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleTierFeatures.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

std::vector<std::string> titleart::titleLitBitNames(uint32_t bits) {
    static const char* const NAMES[] = {"TL_RAMP",        "TL_CLEARCOAT", "TL_IRIDESCENCE", "TL_GEM",        "TL_GEM_BEHIND", "TL_DISPERSION",
                                        "TL_GLASS_DEPTH", "TL_FLAKES",    "TL_PATTERN",     "TL_SURFACE",    nullptr,         "TL_ENV_LOCK",
                                        "TL_BASE_TEXTURE", "TL_WRAP_UV",  "TL_EMISSIVE",    "TL_TRANSPARENT", "TL_RIGS",      "TL_LIGHTS",
                                        "TL_HDR_COMPRESS", "TL_OUTPUT",   nullptr,          "TL_MATCAP",     "TL_SPECULAR_AA", "TL_SURFACE_LITE"};
    std::vector<std::string> out;
    for (uint32_t i = 0; i < 24; i++) {
        if (!(bits & (1u << i))) continue;
        const char* name = NAMES[i] ? NAMES[i] : NAMES[i - 1];
        if (out.empty() || out.back() != name) out.push_back(name);
    }
    return out;
}

const char* titleart::glintModeName(GlintMode mode) {
    switch (mode) {
        case GlintMode::Full: return "full";
        case GlintMode::Mobile: return "mobile";
        case GlintMode::Sprites: return "sprites";
        default: return "off";
    }
}

TitleTierFeatures titleart::titleTierFeatures(const TitleMaterial3D& m, const Title3D& style, TitleTier tier) {
    namespace B = TitleLitBits;
    TitleTierFeatures f;
    uint32_t bits = 0;
    const bool gem = m.transmission > 0;
    if (m.gradient) bits |= B::RAMP;
    if (m.clearcoat > 0) bits |= B::CLEARCOAT;
    if (m.iridescence > 0) bits |= B::IRIDESCENCE;
    if (gem) {
        bits |= B::GEM | B::GEM_BEHIND;
        if (m.dispersion > 0) bits |= B::DISPERSION;
    }
    if (m.glassDepth > 0) bits |= B::GLASS_DEPTH;
    if (m.flakes > 0) bits |= B::FLAKES;
    if (m.hasPattern()) bits |= B::PATTERN;
    if (m.surface != SurfaceKind3D::None) bits |= (uint32_t)m.surface << B::SURFACE_SHIFT;
    if (style.envLock) bits |= B::ENV_LOCK;
    if (!m.baseTexture.empty()) {
        bits |= B::BASE_TEXTURE;
        if (m.baseWrap) bits |= B::WRAP_UV;
    }
    if (m.emissionIntensity > 0) bits |= B::EMISSIVE;
    if (m.opacity < 1 || gem) bits |= B::TRANSPARENT;
    if (m.surface != SurfaceKind3D::None || m.flakes > 0) bits |= B::SPECULAR_AA;
    f.ao = style.ao.enabled;
    f.glint = style.glint.enabled ? GlintMode::Full : GlintMode::Off;
    switch (tier) {
        case TitleTier::Bake:
        case TitleTier::High:
            break;
        case TitleTier::Medium:
            bits &= ~(B::DISPERSION | B::GEM_BEHIND);
            if (bits & B::SURFACE_MASK) bits |= B::SURFACE_LITE;
            f.ao = false;
            if (f.glint == GlintMode::Full) f.glint = GlintMode::Mobile;
            break;
        case TitleTier::Low:
            bits &= ~(B::DISPERSION | B::GEM_BEHIND | B::IRIDESCENCE);
            if (bits & B::GEM) {
                // a gem becomes glass depth plus its reflection
                bits &= ~B::GEM;
                bits |= B::GLASS_DEPTH;
            }
            if (bits & B::SURFACE_MASK) bits |= B::SURFACE_LITE;
            f.ao = false;
            f.partRenders = false;
            if (f.glint == GlintMode::Full) f.glint = GlintMode::Sprites;
            break;
    }
    f.behind = (bits & B::GEM_BEHIND) != 0;
    f.litBits = bits;
    return f;
}

TitleTierGeometry titleart::titleTierGeometry(TitleTier tier) {
    TitleTierGeometry g;
    switch (tier) {
        case TitleTier::Bake:
            break;
        case TitleTier::High:
            g.maxBevelSegments = 8;
            g.inflateNodeCap = 100000;
            break;
        case TitleTier::Medium:
            g.curveScale = 0.75;
            g.minCurveSegments = 4;
            g.maxBevelSegments = 4;
            g.inflateNodeCap = 40000;
            break;
        case TitleTier::Low:
            g.curveScale = 0.5;
            g.minCurveSegments = 4;
            g.maxBevelSegments = 3;
            g.inflateNodeCap = 12000;
            g.inflateStepScale = 1.5;
            break;
    }
    return g;
}

int titleart::titleTierCurveSegments(int authored, TitleTier tier, bool draft) {
    const TitleTierGeometry g = titleTierGeometry(tier);
    int n = authored;
    if (g.curveScale != 1.0) n = std::max(g.minCurveSegments, (int)std::lround(authored * g.curveScale));
    if (draft) n = std::max(1, n / 2);
    return std::clamp(n, 1, 32);
}

TitleTierLimits titleart::titleTierLimits(TitleTier tier) {
    TitleTierLimits l;
    switch (tier) {
        case TitleTier::Bake:
            l.k = 3;
            l.renderPxCap = 1ull << 23;
            l.environmentFace = 1024;
            l.maxPasses = 90;
            l.targetBytes = 350ull << 20;
            l.environmentBytes = 70ull << 20;
            l.msaa = 4;
            break;
        case TitleTier::High:
            l.k = 2;
            l.renderPxCap = 1ull << 22;
            l.environmentFace = 1024;
            l.maxTriangles = 150000;
            l.maxSolids = 8;
            l.maxPasses = 80;
            l.targetBytes = 128ull << 20;
            l.environmentBytes = (size_t)(8.5 * 1048576);
            l.meshBytes = 5ull << 20;
            l.msaa = 2;
            break;
        case TitleTier::Medium:
            l.k = 1;
            l.renderPxCap = 1ull << 22;
            l.environmentFace = 512;
            l.maxTriangles = 60000;
            l.maxSolids = 5;
            l.maxPasses = 48;
            l.targetBytes = 48ull << 20;
            l.environmentBytes = (size_t)(8.4 * 1048576);
            l.meshBytes = 2ull << 20;
            l.msaa = 4;
            break;
        case TitleTier::Low:
            l.k = 1;
            l.renderPxCap = 1ull << 22;
            l.environmentFace = 256;
            l.maxTriangles = 30000;
            l.maxSolids = 3;
            l.maxPasses = 7;
            l.targetBytes = 32ull << 20;
            l.environmentBytes = (size_t)(2.2 * 1048576);
            l.meshBytes = 1ull << 20;
            l.msaa = 4;
            l.compressedLight = true;
            break;
    }
    return l;
}

TitleTier titleart::titleTierForDevice(DeviceTier tier) {
    switch (tier) {
        case DeviceTier::Low: return TitleTier::Low;
        case DeviceTier::Medium: return TitleTier::Medium;
        default: return TitleTier::High;
    }
}

TitleTier titleart::defaultTitleTier(int platform) {
    // Engine::Platform: MacOS 0, iOS 1, Web 2, Android 3, Linux 4, Windows 5
    switch (platform) {
        case 1: return TitleTier::Medium;
        case 2:
        case 3: return TitleTier::Low;
        default: return TitleTier::High;
    }
}

TitleTier titleart::clampTitleTierForBackend(TitleTier tier, bool gles) {
    return gles && (tier == TitleTier::High || tier == TitleTier::Bake) ? TitleTier::Medium : tier;
}

TitleTier titleart::resolveTitleTier(const std::string& qualityTier, TitleTier projectTier, bool deviceTierApplied, DeviceTier deviceTier,
                                     bool gles) {
    TitleTier tier = projectTier;
    TitleTier named;
    if (qualityTier != "auto" && titleTierFromName(qualityTier, named) && named != TitleTier::Bake) {
        tier = named;
    } else if (deviceTierApplied) {
        tier = titleTierForDevice(deviceTier);
    }
    return clampTitleTierForBackend(tier, gles);
}

TitleTierFit titleart::titleTierFit(const TitleMeshStats& stats, size_t meshBytes, TitleTier tier) {
    const TitleTierLimits l = titleTierLimits(tier);
    TitleTierFit fit;
    auto over = [&](const char* what, uint64_t value, uint64_t limit) {
        if (!limit || value <= limit) return;
        fit.fits = false;
        fit.over.push_back(std::string(what) + " " + std::to_string(value) + " > " + std::to_string(limit));
    };
    over("triangles", stats.triangles, l.maxTriangles);
    over("solids", stats.solids, l.maxSolids);
    over("mesh bytes", meshBytes, l.meshBytes);
    if (tier == TitleTier::Bake && stats.triangles > TITLE_BAKE_TRIANGLE_WARNING) {
        fit.over.push_back("triangles " + std::to_string(stats.triangles) + " > " + std::to_string(TITLE_BAKE_TRIANGLE_WARNING) + " (warning)");
    }
    return fit;
}
