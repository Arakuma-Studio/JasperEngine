// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtCaps.h"

using namespace jasperengine;
using namespace jasperengine::titleart;

TitleArtCaps TitleArtCaps::select(const RenderFeatures& features, uint32_t testFlags) {
    const bool noFloat = (testFlags & TITLE_ART_TEST_NO_FLOAT_TARGETS) != 0;
    const bool noFilter = (testFlags & TITLE_ART_TEST_NO_FLOAT_FILTER) != 0;
    const bool rgba16fRender = !noFloat && features.renderRGBA16F;
    const bool rgba16fFilter = rgba16fRender && !noFilter && features.filterRGBA16F;
    const bool rgba32fRender = !noFloat && !(testFlags & TITLE_ART_TEST_NO_RGBA32F) && features.renderRGBA32F;
    const bool r16fRender = !noFloat && !(testFlags & TITLE_ART_TEST_NO_R16F) && features.renderR16F;
    const bool r16fFilter = r16fRender && !noFilter && features.filterR16F;
    const bool r32fRender = !noFloat && features.renderR32F;

    TitleArtCaps caps;
    auto set = [&caps](TitleBuffer buffer, ColorFormat format, TextureFilter filter, TitleEncoding encoding) {
        caps.buffers[(int)buffer] = {format, filter, encoding};
    };

    // accumulators are sampled linearly (placement, blurs): render + filter, else SRGB8A8, which
    // keeps dark precision and blends in linear light (core on every backend)
    caps.floatPath = rgba16fRender && rgba16fFilter;
    if (caps.floatPath) {
        set(TitleBuffer::Accumulator, ColorFormat::RGBA16F, TextureFilter::LINEAR, TitleEncoding::Native);
    } else {
        set(TitleBuffer::Accumulator, ColorFormat::SRGB8A8, TextureFilter::LINEAR, TitleEncoding::SrgbPremultiplied);
    }
    caps.floatBlend = caps.floatPath && features.blendRGBA16F && !(testFlags & TITLE_ART_TEST_NO_FLOAT_BLEND);

    set(TitleBuffer::Coverage, ColorFormat::RED, TextureFilter::LINEAR, TitleEncoding::Native);

    // seeds are read with texelFetch: render only
    if (rgba32fRender) {
        set(TitleBuffer::Seeds, ColorFormat::RGBA32F, TextureFilter::NEAREST, TitleEncoding::Native);
    } else {
        set(TitleBuffer::Seeds, ColorFormat::RGBA, TextureFilter::NEAREST, TitleEncoding::Fixed12_4);
    }

    if (r16fRender) {
        set(TitleBuffer::Distance, ColorFormat::R16F, TextureFilter::NEAREST, TitleEncoding::Native);
    } else {
        set(TitleBuffer::Distance, ColorFormat::RGBA, TextureFilter::NEAREST, TitleEncoding::Fixed16);
    }

    if (r32fRender) {
        set(TitleBuffer::Height, ColorFormat::R32F, TextureFilter::NEAREST, TitleEncoding::Native);
    } else if (r16fRender) {
        set(TitleBuffer::Height, ColorFormat::R16F, TextureFilter::NEAREST, TitleEncoding::Native);
    } else {
        set(TitleBuffer::Height, ColorFormat::RGBA, TextureFilter::NEAREST, TitleEncoding::Fixed16);
    }

    if (r16fFilter) {
        set(TitleBuffer::Satin, ColorFormat::R16F, TextureFilter::LINEAR, TitleEncoding::Native);
    } else {
        set(TitleBuffer::Satin, ColorFormat::RED, TextureFilter::LINEAR, TitleEncoding::Native);
    }

    // sRGB stops, as Pachislop's code does
    set(TitleBuffer::GradientLut, ColorFormat::RGBA, TextureFilter::LINEAR, TitleEncoding::Native);

    // uploaded, not rendered: filtering decides
    if (!noFloat && !noFilter && features.filterRGBA16F) {
        set(TitleBuffer::ToneLut, ColorFormat::RGBA16F, TextureFilter::LINEAR, TitleEncoding::Native);
    } else {
        set(TitleBuffer::ToneLut, ColorFormat::RGBA, TextureFilter::LINEAR, TitleEncoding::Native);
    }
    return caps;
}
