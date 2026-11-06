// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R12, R38, §6.3 "Formats and the RGBA8 fallback")
// - Purpose: which texture format each kind of title-art buffer takes on this device, the
//   FluidSimulator pattern (caps + test flags): the float path where the device renders (and
//   filters, where sampled linearly) the preferred format, else the RGBA8 fallback with the
//   same pass structure and the stated encoding. The flags force the fallback per request
//   (RenderRequest::testFlags), never globally, so a test renders both paths in one process.
// - Thread-safety: select() is pure.

#pragma once

#include "Export.h"
#include "render/Render.h"
#include "render/SystemRender.h"

#include <cstdint>

namespace jasperengine::titleart {

    // RenderRequest::testFlags. S80's bits; S81 (R21) and S86 (R41) append theirs after them.
    constexpr uint32_t TITLE_ART_TEST_NO_FLOAT_TARGETS = 1u << 0;  // no float render target at all
    constexpr uint32_t TITLE_ART_TEST_NO_FLOAT_FILTER = 1u << 1;   // float targets sample NEAREST only
    constexpr uint32_t TITLE_ART_TEST_NO_R16F = 1u << 2;
    constexpr uint32_t TITLE_ART_TEST_NO_RGBA32F = 1u << 3;
    constexpr uint32_t TITLE_ART_TEST_NO_FLOAT_BLEND = 1u << 4;    // fixed-function blending of RGBA16F off
    constexpr uint32_t TITLE_ART_TEST_UNBOUNDED_FIELDS = 1u << 5;  // every distance field floods the whole frame (R27's comparison)
    constexpr uint32_t TITLE_ART_TEST_S80_LAST_BIT = 5;

    // the buffer kinds of §6.3's format table
    enum class TitleBuffer : uint8_t {
        Accumulator,    // node, effect and stack targets: linear premultiplied colour
        Coverage,       // element coverage masks
        Seeds,          // jump-flood seeds (x, y, valid)
        Distance,       // signed distance
        Height,         // height and Fit cover radius
        Satin,          // satin blur and the combined layer mask
        GradientLut,    // gradient and curve LUTs (256 x 1, uploaded)
        ToneLut         // levels, curves and brightness/contrast LUTs (256 x 1, uploaded)
    };
    constexpr int TITLE_BUFFER_COUNT = 8;

    // how a fallback format stores what the preferred one holds natively
    enum class TitleEncoding : uint8_t {
        Native,             // the format holds the value
        SrgbPremultiplied,  // SRGB8A8: linear premultiplied colour, encoded by the hardware on write
        Fixed12_4,          // RGBA8: x and y as 12.4 fixed point in two bytes each, 0xFFFF = no seed
        Fixed16             // RGBA8: 16-bit fixed point in RG (distance: 1/64 px steps, +-512 px)
    };

    struct TitleBufferFormat {
        ColorFormat format = ColorFormat::RGBA;
        TextureFilter filter = TextureFilter::NEAREST;
        TitleEncoding encoding = TitleEncoding::Native;
    };

    struct JASPERENGINE_API TitleArtCaps {
        TitleBufferFormat buffers[TITLE_BUFFER_COUNT];
        bool floatPath = false;             // the accumulators are RGBA16F
        bool floatBlend = false;            // Normal and Add blend RGBA16F in fixed function

        const TitleBufferFormat& format(TitleBuffer buffer) const { return buffers[(int)buffer]; }
        // "rgba16f" or "rgba8" (stats, results' path_used, the bake recipe)
        const char* pathName() const { return floatPath ? "rgba16f" : "rgba8"; }

        // the formats for a device and a request's test flags
        static TitleArtCaps select(const RenderFeatures& features, uint32_t testFlags);
    };

}
