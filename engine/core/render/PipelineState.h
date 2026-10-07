// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIPELINESTATE_H
#define PIPELINESTATE_H

#include "Render.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine {

    // Dynamic pipeline state (HTML UI renderer, phase-5 effects): objects loaded with
    // ObjectRender::endLoadDynamic draw with any PipelineState; SokolPipelineCache creates one
    // sokol pipeline per (shader, vertex layout, state) on first use. The fixed pipeline types
    // (PipelineType, ObjectRender::endLoad) are untouched by this path.

    // rgb and alpha alike, additive blend equation; the last two are phase-5 effect blends
    //   NONE                    blending off
    //   ALPHA                   SRC_ALPHA, ONE_MINUS_SRC_ALPHA (alpha: ONE, ONE_MINUS_SRC_ALPHA)
    //   PREMULTIPLIED           ONE, ONE_MINUS_SRC_ALPHA
    //   ADDITIVE_PREMULTIPLIED  ONE, ONE
    //   SCREEN_PREMULTIPLIED    ONE, ONE_MINUS_SRC_COLOR
    //   MULTIPLY_PREMULTIPLIED  DST_COLOR, ONE_MINUS_SRC_ALPHA
    enum class BlendMode : uint8_t { NONE, ALPHA, PREMULTIPLIED, ADDITIVE_PREMULTIPLIED,
                                     SCREEN_PREMULTIPLIED, MULTIPLY_PREMULTIPLIED };
    enum class ColorWriteMask : uint8_t { NONE = 0, R = 1, G = 2, B = 4, A = 8, RGB = 7, RGBA = 15 };
    enum class CompareFunc : uint8_t { NEVER, LESS, EQUAL, LESS_EQUAL, GREATER, NOT_EQUAL, GREATER_EQUAL, ALWAYS };
    enum class StencilOp : uint8_t { KEEP, ZERO, REPLACE, INCR_CLAMP, DECR_CLAMP, INVERT, INCR_WRAP, DECR_WRAP };
    enum class DepthStencilFormat : uint8_t { NONE, DEPTH, DEPTH_STENCIL };
    enum class IndexType : uint8_t { NONE, UINT16, UINT32 };

    // pass load/store actions (CameraRender::setColorActions/setDepthStencilActions)
    enum class LoadAction : uint8_t { CLEAR, LOAD, DONTCARE };
    enum class StoreAction : uint8_t { STORE, DONTCARE };

    struct StencilState {
        bool enabled = false;
        CompareFunc compare = CompareFunc::ALWAYS;
        StencilOp failOp = StencilOp::KEEP, depthFailOp = StencilOp::KEEP, passOp = StencilOp::KEEP;
        uint8_t readMask = 0xFF, writeMask = 0xFF, ref = 0;     // same for front and back faces

        bool operator==(const StencilState& o) const{
            return enabled == o.enabled && compare == o.compare && failOp == o.failOp && depthFailOp == o.depthFailOp &&
                   passOp == o.passOp && readMask == o.readMask && writeMask == o.writeMask && ref == o.ref;
        }
        bool operator!=(const StencilState& o) const{ return !(*this == o); }
    };

    struct RenderTargetFormat {
        bool swapchain = false;          // true: colour/depth/sample count from sg_environment defaults
        ColorFormat color = ColorFormat::RGBA;
        DepthStencilFormat depth = DepthStencilFormat::DEPTH_STENCIL;
        int sampleCount = 1;

        bool operator==(const RenderTargetFormat& o) const{
            return swapchain == o.swapchain && color == o.color && depth == o.depth && sampleCount == o.sampleCount;
        }
        bool operator!=(const RenderTargetFormat& o) const{ return !(*this == o); }
    };

    struct PipelineState {
        RenderTargetFormat target;
        BlendMode blend = BlendMode::PREMULTIPLIED;
        ColorWriteMask writeMask = ColorWriteMask::RGBA;
        StencilState stencil;
        CompareFunc depthCompare = CompareFunc::ALWAYS;
        bool depthWrite = false;
        bool cullBack = false;

        // FNV-1a over every field, in declaration order
        uint64_t hash() const{
            uint64_t h = 14695981039346656037ull;
            auto add = [&h](uint64_t value, int bytes){
                for (int i = 0; i < bytes; i++){
                    h ^= (value >> (8 * i)) & 0xFF;
                    h *= 1099511628211ull;
                }
            };
            add(target.swapchain ? 1 : 0, 1);
            add((uint64_t)target.color, 1);
            add((uint64_t)target.depth, 1);
            add((uint64_t)(uint32_t)target.sampleCount, 4);
            add((uint64_t)blend, 1);
            add((uint64_t)writeMask, 1);
            add(stencil.enabled ? 1 : 0, 1);
            add((uint64_t)stencil.compare, 1);
            add((uint64_t)stencil.failOp, 1);
            add((uint64_t)stencil.depthFailOp, 1);
            add((uint64_t)stencil.passOp, 1);
            add(stencil.readMask, 1);
            add(stencil.writeMask, 1);
            add(stencil.ref, 1);
            add((uint64_t)depthCompare, 1);
            add(depthWrite ? 1 : 0, 1);
            add(cullBack ? 1 : 0, 1);
            return h;
        }

        bool operator==(const PipelineState& o) const{
            return target == o.target && blend == o.blend && writeMask == o.writeMask && stencil == o.stencil &&
                   depthCompare == o.depthCompare && depthWrite == o.depthWrite && cullBack == o.cullBack;
        }
        bool operator!=(const PipelineState& o) const{ return !(*this == o); }
    };

    // The formats PIP_DEFAULT / PIP_RTT / ... resolve to (SokolObject::makePipelineDesc), so
    // code drawing inside an existing camera pass can build a matching state
    inline RenderTargetFormat targetFormatFor(PipelineType pipelineType){
        RenderTargetFormat format;
        switch (pipelineType){
        case PipelineType::PIP_DEFAULT:
        case PipelineType::PIP_DEFAULT_NODEPTH:
        case PipelineType::PIP_ZPREPASS:
            // the swapchain pass: sokol's environment defaults
            format.swapchain = true;
            break;
        default:
            // offscreen: RGBA8 colour (attachment 0), DEPTH, one sample
            format.swapchain = false;
            format.color = ColorFormat::RGBA;
            format.depth = DepthStencilFormat::DEPTH;
            format.sampleCount = 1;
            break;
        }
        return format;
    }

}

#endif //PIPELINESTATE_H
