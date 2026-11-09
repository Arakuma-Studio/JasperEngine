// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEEVAL_H
#define PARTICLES_PARTICLEEVAL_H

// One particle's compiled values, one at a time: the emission's rates, the appearance an event
// hands to a sub-emitter, the trails' widths and colours. The kernels evaluate the same
// parameters four at a time (ParticleKernels.h).

#include "particles/ParticleKernels.h"

#include <algorithm>
#include <cmath>

namespace jasperengine { namespace particles {

    // what a curve's driver reads for one particle
    struct ScalarDriver {
        float normAge = 0.0f;
        float age = 0.0f;
        float speed = 0.0f;
        float emitterTime = 0.0f;
        uint32_t spawn = 0;
        uint32_t seed = 0;
    };

    inline float wrapScalar(float t, CurveWrap w){
        switch (w){
            case CurveWrap::Repeat: return t - std::floor(t);
            case CurveWrap::PingPong: {
                const float m = t - std::floor(t * 0.5f) * 2.0f;
                return m > 1.0f ? 2.0f - m : m;
            }
            case CurveWrap::Clamp:
            default: return std::clamp(t, 0.0f, 1.0f);
        }
    }

    inline float lutScalar(const ParticleProgram& prog, uint32_t offset, uint32_t size, float t){
        const float* table = prog.floats(offset);
        const float x = t * (float)(size - 1);
        uint32_t i = (uint32_t)x;
        if (i > size - 2) i = size - 2;
        const float f = x - (float)i;
        return table[i] + (table[i + 1] - table[i]) * f;
    }

    inline float driverScalar(CurveDriver driver, uint32_t stream, float scale, float offset, const ScalarDriver& d){
        float raw;
        switch (driver){
            case CurveDriver::Age: raw = d.age; break;
            case CurveDriver::Speed: raw = d.speed; break;
            case CurveDriver::EmitterTime: raw = d.emitterTime; break;
            case CurveDriver::Random: raw = random01(d.seed ^ stream ^ 0x68e31da4u, d.spawn); break;
            case CurveDriver::NormalizedAge:
            default: raw = d.normAge; break;
        }
        return raw * scale + offset;
    }

    inline float curveScalar(const ParticleProgram& prog, const CurveParam& c, const ScalarDriver& d, uint32_t salt = 0){
        switch (c.kind){
            case CurveParam::Const: return c.c0;
            case CurveParam::Range: return c.c0 + (c.c1 - c.c0) * random01(d.seed ^ c.stream ^ salt, d.spawn);
            case CurveParam::Lut:
                return lutScalar(prog, c.lut0, c.lutSize, wrapScalar(driverScalar(c.driver, c.stream, c.scale, c.offset, d), c.wrap));
            case CurveParam::LutRange: {
                const float t = wrapScalar(driverScalar(c.driver, c.stream, c.scale, c.offset, d), c.wrap);
                const float a = lutScalar(prog, c.lut0, c.lutSize, t);
                const float b = lutScalar(prog, c.lut1, c.lutSize, t);
                return a + (b - a) * random01(d.seed ^ c.stream ^ salt, d.spawn);
            }
        }
        return c.c0;
    }

    // a Ramp's step (the kernels' rampIndex for one particle; std::pow where they take their own)
    inline uint32_t rampIndexScalar(const GradientParam& g, const ScalarDriver& d){
        float t = wrapScalar(driverScalar(g.driver, g.stream, g.scale, g.offset, d), g.wrap);
        if (g.rampBias != 1.0f)
            t = t > 1.0e-10f ? std::pow(t, g.rampBias) : 0.0f;
        const float n = (float)g.lutSize;
        const uint32_t step = (uint32_t)std::min(std::max(std::floor(t * n), 0.0f), n - 1.0f);
        return g.rampReverse ? g.lutSize - 1u - step : step;
    }

    inline void gradientScalar(const ParticleProgram& prog, const GradientParam& g, const ScalarDriver& d, float out[4]){
        switch (g.kind){
            case GradientParam::Ramp: {
                const float* table = prog.floats(g.lut0);
                const uint32_t i = rampIndexScalar(g, d);
                for (int c = 0; c < 4; c++) out[c] = table[i * 4 + c];
                return;
            }
            case GradientParam::Const:
                for (int c = 0; c < 4; c++) out[c] = g.c0[c];
                return;
            case GradientParam::Range: {
                const float r = random01(d.seed ^ g.stream, d.spawn);
                for (int c = 0; c < 4; c++) out[c] = g.c0[c] + (g.c1[c] - g.c0[c]) * r;
                return;
            }
            case GradientParam::Lut:
            case GradientParam::LutRange: {
                const float t = wrapScalar(driverScalar(g.driver, g.stream, g.scale, g.offset, d), g.wrap);
                const float x = t * (float)(g.lutSize - 1);
                uint32_t i = (uint32_t)x;
                if (i > g.lutSize - 2) i = g.lutSize - 2;
                const float f = x - (float)i;
                const float* a = prog.floats(g.lut0);
                const float r = g.kind == GradientParam::LutRange ? random01(d.seed ^ g.stream, d.spawn) : 0.0f;
                for (int c = 0; c < 4; c++){
                    float v = a[i * 4 + c] + (a[i * 4 + 4 + c] - a[i * 4 + c]) * f;
                    if (g.kind == GradientParam::LutRange){
                        const float* b = prog.floats(g.lut1);
                        const float w = b[i * 4 + c] + (b[i * 4 + 4 + c] - b[i * 4 + c]) * f;
                        v = v + (w - v) * r;
                    }
                    out[c] = v;
                }
                return;
            }
        }
    }

}}

#endif // PARTICLES_PARTICLEEVAL_H
