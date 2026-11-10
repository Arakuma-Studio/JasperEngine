// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TfxPrograms.h"

#include "pool/ShaderPool.h"

#include <atomic>

using namespace jasperengine;
using namespace jasperengine::titleart;

const char* tfx::programName(Program program) {
    switch (program) {
        case Program::LightEmit: return "light_emit";
        case Program::LightDown: return "light_down";
        case Program::LightBlur: return "light_blur";
        case Program::LightAccum: return "light_accum";
        case Program::LightCompose: return "light_compose";
        case Program::ContourLines: return "contour_lines";
        case Program::ContourCompose: return "contour_compose";
        case Program::NeonTube: return "neon_tube";
        case Program::NeonCompose: return "neon_compose";
        case Program::MotionBlur: return "motion_blur";
        case Program::LensDown: return "lens_down";
        case Program::LensGather: return "lens_gather";
        case Program::LensFinish: return "lens_finish";
        case Program::TiltFar: return "tilt_far";
        case Program::TiltBlend: return "tilt_blend";
        case Program::RadialBlur: return "radial_blur";
        case Program::Extrude: return "extrude";
        case Program::MotionSmear: return "motion_smear";
        case Program::MotionEcho: return "motion_echo";
        case Program::Halftone: return "halftone";
        case Program::PerspectiveShadow: return "perspective_shadow";
        case Program::FloorReflection: return "floor_reflection";
        case Program::WaveWarp: return "wave_warp";
        case Program::Chromatic: return "chromatic_aberration";
        case Program::Grain: return "grain";
        case Program::Glitch: return "glitch";
        case Program::Scanlines: return "scanlines";
        case Program::PixelDown: return "pixel_down";
        case Program::PixelUp: return "pixel_up";
        case Program::Holographic: return "holographic";
        case Program::Distress: return "distress";
        case Program::DripOrigins: return "drip_origins";
        case Program::DripField: return "drip_field";
        case Program::DripCompose: return "drip_compose";
        case Program::BrushStroke: return "brush_stroke";
        case Program::Frost: return "frost";
        case Program::Chalk: return "chalk";
        case Program::StitchLayout: return "stitch_layout";
        case Program::Stitch: return "stitch";
        case Program::Shatter: return "shatter";
        case Program::MaterialSurface: return "material_surface";
        case Program::MaterialDetail: return "material_detail";
    }
    return "";
}

bool tfx::readsSeeds(Program program) {
    switch (program) {
        // the layer box (fx_layerBox decodes the bounds reduction in the Seeds format)
        case Program::ContourLines:
        case Program::MotionSmear:
        case Program::MotionEcho:
        case Program::Halftone:
        case Program::PerspectiveShadow:
        case Program::FloorReflection:
        case Program::Glitch:
        case Program::DripOrigins:
        case Program::DripField:
        case Program::StitchLayout:
        case Program::Stitch:
        case Program::Shatter:
        case Program::MaterialSurface:
        case Program::MaterialDetail:
            return true;
        default:
            return false;
    }
}

bool tfx::readsDistance(Program program) {
    switch (program) {
        case Program::ContourLines:
        case Program::NeonTube:
        case Program::Distress:
        case Program::DripOrigins:
        case Program::DripField:
        case Program::DripCompose:
        case Program::BrushStroke:
        case Program::Frost:
        case Program::Chalk:
        case Program::StitchLayout:
        case Program::Stitch:
        case Program::Holographic:
        case Program::MaterialSurface:
        case Program::MaterialDetail:
            return true;
        default:
            return false;
    }
}

uint64_t tfx::programVariant(Program program, const TitleArtCaps& caps, bool live) {
    uint64_t variant = TITLE_VARIANT_CATALOGUE_EFFECT;
    if (readsSeeds(program) && caps.format(TitleBuffer::Seeds).encoding == TitleEncoding::Fixed12_4) variant |= TITLE_VARIANT_SEEDS_FIXED;
    if (readsDistance(program) && caps.format(TitleBuffer::Distance).encoding == TitleEncoding::Fixed16) variant |= TITLE_VARIANT_DISTANCE_FIXED;
    if (live) variant |= TITLE_VARIANT_LIVE;
    return variant;
}

ShaderKey tfx::programKey(Program program, uint64_t variant) {
    return ShaderPool::getShaderKey(ShaderType::TITLE_ART, (uint64_t)program | variant | TITLE_VARIANT_CATALOGUE_EFFECT);
}

TitlePass tfx::makePass(const EffectContext& ctx, Program program, const char* label, TargetRef& output) {
    TitlePass pass;
    // the number lives in titleart_fx.frag's range: the 'Efx' bit tells the two files apart
    pass.program = (TitleProgram)(uint8_t)program;
    pass.variant = programVariant(program, ctx.runner.caps());
    pass.label = label;
    pass.output = &output;
    pass.pxScale = ctx.pxScale;
    if (!ctx.frame.empty() && ctx.width > 0 && ctx.height > 0) {
        pass.frameUv[0] = (float)ctx.frame.x / (float)ctx.width;
        pass.frameUv[1] = (float)ctx.frame.y / (float)ctx.height;
        pass.frameUv[2] = (float)(ctx.frame.x + ctx.frame.width) / (float)ctx.width;
        pass.frameUv[3] = (float)(ctx.frame.y + ctx.frame.height) / (float)ctx.height;
    }
    return pass;
}

namespace {
    std::atomic<float> headroomForTesting{0.0f};
}

float tfx::lightHeadroom(const TitleArtCaps& caps) {
    const float forced = headroomForTesting.load(std::memory_order_relaxed);
    if (forced > 0.0f) return forced;
    return caps.floatPath ? 1.0f : 4.0f;
}

void tfx::setLightHeadroomForTesting(float headroom) {
    headroomForTesting.store(headroom, std::memory_order_relaxed);
}
