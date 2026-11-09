// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The capacity hint follows OpenEngine's ContinuousEmissionPeak and the structure comparison
// its HaveSameStructure (Engine/Modules/Particles/Source/ParticleStackCompiler.cpp,
// ParticleStackDocument.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors,
// MIT License. See NOTICE.md.

#include "ParticleCompiler.h"

#include "math/noise/GradientCurl3D.h"
#include "particles/AnalyticParticles.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleKernels.h"
#include "pixelart/PixelColor.h"
#include "particles/ParticleModules.h"
#include "util/Color.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float kDegToRad = 0.01745329251994329577f;

    // ---------------------------------------------------------------- the pool

    class PoolWriter {
    public:
        explicit PoolWriter(std::vector<uint32_t>& pool): pool(pool) {}

        template<class T>
        uint32_t add(const T& value){
            static_assert(std::is_trivially_copyable<T>::value, "pool blocks are plain data");
            const uint32_t offset = reserve(sizeof(T));
            std::memcpy(reinterpret_cast<unsigned char*>(pool.data()) + offset, &value, sizeof(T));
            return offset;
        }

        uint32_t addFloats(const float* values, size_t count){
            const uint32_t offset = reserve(count * sizeof(float));
            std::memcpy(reinterpret_cast<unsigned char*>(pool.data()) + offset, values, count * sizeof(float));
            return offset;
        }

        uint32_t addUints(const uint32_t* values, size_t count){
            const uint32_t offset = reserve(count * sizeof(uint32_t));
            std::memcpy(reinterpret_cast<unsigned char*>(pool.data()) + offset, values, count * sizeof(uint32_t));
            return offset;
        }

        template<class T>
        T& at(uint32_t offset){ return *reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(pool.data()) + offset); }

    private:
        // 16-byte aligned blocks
        uint32_t reserve(size_t bytes){
            const size_t words = (pool.size() + 3) & ~size_t(3);
            const size_t need = (bytes + 3) / 4;
            pool.resize(words + ((need + 3) & ~size_t(3)), 0u);
            return (uint32_t)(words * 4);
        }

        std::vector<uint32_t>& pool;
    };

    // ---------------------------------------------------------------- curves

    float curveValueRange(const Curve& curve){
        float lo, hi;
        curve.valueRange(lo, hi);
        return std::max(1.0f, hi - lo);
    }

    void bakeCurve(const Curve& curve, float multiplier, uint32_t size, std::vector<float>& out){
        out.resize(size);
        for (uint32_t i = 0; i < size; i++)
            out[i] = curve.evaluate((float)i / (float)(size - 1)) * multiplier;
    }

    // where a value's LUT goes: appended to the pool (a compile), or over the LUT the value had
    // (applyExposed: false when the new one does not fit there)
    using LutPlace = std::function<bool(const std::vector<float>& values, bool second, uint32_t& offset)>;

    // the LUT size that keeps every channel of a gradient within half a step of 8 bits
    uint32_t gradientLutSize(const Gradient& g){
        if (g.mode == GradientMode::Fixed)
            return 1025;
        for (uint32_t size = 65; size < 1025; size = (size - 1) * 2 + 1){
            bool ok = true;
            for (uint32_t i = 0; i + 1 < size && ok; i++){
                const float t0 = (float)i / (float)(size - 1), t1 = (float)(i + 1) / (float)(size - 1);
                const Vector4 a = g.evaluate(t0), b = g.evaluate(t1), m = g.evaluate((t0 + t1) * 0.5f);
                const Vector4 lerp = (a + b) * 0.5f;
                ok = std::fabs(lerp.x - m.x) < 0.5f / 255.0f && std::fabs(lerp.y - m.y) < 0.5f / 255.0f &&
                     std::fabs(lerp.z - m.z) < 0.5f / 255.0f && std::fabs(lerp.w - m.w) < 0.5f / 255.0f;
            }
            if (ok)
                return size;
        }
        return 1025;
    }

    bool buildCurve(const MinMaxCurve& v, uint32_t salt, float scale, bool startValue, const LutPlace& place, CurveParam& c){
        c = CurveParam();
        c.stream = salt;
        c.driver = v.driver;
        c.wrap = v.wrap;
        if (startValue && c.driver != CurveDriver::Random)
            c.driver = CurveDriver::EmitterTime;
        const float span = v.driverMax - v.driverMin;
        c.scale = span != 0.0f ? 1.0f / span : 0.0f;
        c.offset = span != 0.0f ? -v.driverMin / span : 0.0f;
        switch (v.mode){
            case CurveMode::Constant:
                c.kind = CurveParam::Const;
                c.c0 = c.c1 = v.constant * scale;
                return true;
            case CurveMode::TwoConstants:
                c.kind = v.constant == v.constantMax ? CurveParam::Const : CurveParam::Range;
                c.c0 = v.constant * scale;
                c.c1 = v.constantMax * scale;
                return true;
            case CurveMode::Curve:
            case CurveMode::Ease:
            case CurveMode::TwoCurves: {
                const bool two = v.mode == CurveMode::TwoCurves;
                // a flat curve is a constant
                float lo0, hi0;
                v.curve.valueRange(lo0, hi0);
                if (!two && lo0 == hi0){
                    c.kind = CurveParam::Const;
                    c.c0 = c.c1 = lo0 * v.multiplier * scale;
                    return true;
                }
                c.kind = two ? CurveParam::LutRange : CurveParam::Lut;
                // both LUTs share a size (the finer of the two)
                uint32_t size0 = ParticleCompiler::lutSize(v.curve, 1.0e-4f * curveValueRange(v.curve));
                uint32_t size1 = two ? ParticleCompiler::lutSize(v.curveMax, 1.0e-4f * curveValueRange(v.curveMax)) : size0;
                c.lutSize = std::max(size0, size1);
                std::vector<float> values;
                bakeCurve(v.curve, v.multiplier * scale, c.lutSize, values);
                if (!place(values, false, c.lut0))
                    return false;
                if (two){
                    bakeCurve(v.curveMax, v.multiplier * scale, c.lutSize, values);
                    if (!place(values, true, c.lut1))
                        return false;
                }
                return true;
            }
        }
        return true;
    }

    std::vector<float> gradientLut(const Gradient& g, uint32_t size){
        std::vector<float> values(size * 4);
        for (uint32_t i = 0; i < size; i++){
            const Vector4 c = g.evaluate((float)i / (float)(size - 1));
            values[i * 4 + 0] = c.x;
            values[i * 4 + 1] = c.y;
            values[i * 4 + 2] = c.z;
            values[i * 4 + 3] = c.w;
        }
        return values;
    }

    bool buildGradient(const MinMaxGradient& v, uint32_t salt, bool startValue, const LutPlace& place, GradientParam& g){
        g = GradientParam();
        g.stream = salt;
        g.driver = v.driver;
        g.wrap = v.wrap;
        if (startValue && g.driver != CurveDriver::Random)
            g.driver = CurveDriver::EmitterTime;
        const float span = v.driverMax - v.driverMin;
        g.scale = span != 0.0f ? 1.0f / span : 0.0f;
        g.offset = span != 0.0f ? -v.driverMin / span : 0.0f;
        auto linear = [](const Vector4& c, float out[4]){
            const Vector4 l = Color::sRGBToLinear(c);
            out[0] = l.x; out[1] = l.y; out[2] = l.z; out[3] = l.w;
        };
        switch (v.mode){
            case GradientValueMode::Ramp: {
                // the exact steps (S49 R13): no LUT between them; Start Color picks one at random.
                // lut0 the steps in linear, lut1 their sRGB8 bytes (packed, bit for bit in the pool)
                const std::vector<pixelart::Rgba8> colors = v.resolveRamp();
                if (colors.empty()){
                    g.kind = GradientParam::Const;
                    for (int c = 0; c < 4; c++) g.c0[c] = g.c1[c] = 1.0f;
                    return true;
                }
                g.kind = GradientParam::Ramp;
                if (startValue){
                    g.driver = CurveDriver::Random;
                    g.scale = 1.0f;
                    g.offset = 0.0f;
                }
                g.lutSize = (uint32_t)colors.size();
                std::vector<float> values, bytes;
                for (const pixelart::Rgba8& c : colors){
                    const Vector4 l = pixelart::toEngineLinear(c);
                    values.insert(values.end(), {l.x, l.y, l.z, l.w});
                    const uint32_t packed = (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | ((uint32_t)c.a << 24);
                    float word;
                    std::memcpy(&word, &packed, sizeof(word));
                    bytes.push_back(word);
                }
                if (!place(values, false, g.lut0) || !place(bytes, true, g.lut1))
                    return false;
                g.rampBias = std::clamp(v.rampBias, 0.1f, 10.0f);
                g.rampReverse = v.rampReverse ? 1u : 0u;
                return true;
            }
            case GradientValueMode::Color:
                g.kind = GradientParam::Const;
                linear(v.color, g.c0);
                linear(v.color, g.c1);
                return true;
            case GradientValueMode::TwoColors:
                g.kind = v.color == v.colorMax ? GradientParam::Const : GradientParam::Range;
                linear(v.color, g.c0);
                linear(v.colorMax, g.c1);
                return true;
            case GradientValueMode::RandomColor:
                // Unity's Random Color: the gradient read at a random point per particle
                g.driver = CurveDriver::Random;
                g.scale = 1.0f;
                g.offset = 0.0f;
                g.wrap = CurveWrap::Clamp;
                [[fallthrough]];
            case GradientValueMode::Gradient:
            case GradientValueMode::TwoGradients: {
                const bool two = v.mode == GradientValueMode::TwoGradients;
                g.kind = two ? GradientParam::LutRange : GradientParam::Lut;
                g.lutSize = std::max(gradientLutSize(v.gradient), two ? gradientLutSize(v.gradientMax) : 0u);
                if (!place(gradientLut(v.gradient, g.lutSize), false, g.lut0))
                    return false;
                if (two && !place(gradientLut(v.gradientMax, g.lutSize), true, g.lut1))
                    return false;
                return true;
            }
        }
        return true;
    }

    // What the compiler decides from a value besides its numbers: a change of any of it is a
    // change of the program's structure (applyExposed compiles again)
    uint32_t curveShape(const MinMaxCurve& v){
        uint32_t shape = (uint32_t)v.mode | ((uint32_t)v.driver << 4);
        if (v.isZero()) shape |= 1u << 8;
        if (v.isConstant()) shape |= 1u << 9;
        if (v.isConstant() && v.constant == 1.0f) shape |= 1u << 10;
        if (v.usesCurve() && v.mode != CurveMode::TwoCurves){
            // a flat curve compiles to a constant
            float lo, hi;
            v.curve.valueRange(lo, hi);
            if (lo == hi) shape |= 1u << 11;
        }
        return shape;
    }

    uint32_t gradientShape(const MinMaxGradient& v){
        return (uint32_t)v.mode | ((uint32_t)v.driver << 4);
    }

    struct Context {
        ParticleProgram& program;
        PoolWriter pool;
        const ParticleEffectDocument& doc;
        const CompileEnvironment& env;
        std::vector<Diagnostic>& diagnostics;

        // the exposed parameters' values (curve or colour fields) and the slots of their uses
        // (empty without exposed parameters)
        struct Bound {
            const void* source = nullptr;
            std::vector<uint32_t> bindings;
        };
        std::vector<Bound> bound;
        std::vector<ExposedSlot>* slots = nullptr;
        std::vector<ExposedBinding>* bindings = nullptr;
        // the slots' patterns, to find their copies once the program is done
        std::vector<std::vector<unsigned char>> patterns;

        LutPlace append(){
            return [this](const std::vector<float>& values, bool, uint32_t& offset){
                offset = pool.addFloats(values.data(), values.size());
                return true;
            };
        }

        // a constant-free curve compiled into its LUT(s)
        uint32_t curveLut(const Curve& curve, float multiplier, uint32_t& size){
            size = ParticleCompiler::lutSize(curve, 1.0e-4f * curveValueRange(curve));
            std::vector<float> values;
            bakeCurve(curve, multiplier, size, values);
            return pool.addFloats(values.data(), values.size());
        }

        Bound* boundOf(const void* source){
            for (Bound& b : bound){
                if (b.source == source)
                    return &b;
            }
            return nullptr;
        }

        // a compiled use of an exposed value: a slot tagged in the param's pad byte
        template<class Param>
        void record(const void* source, Param& param, bool gradient, const MinMaxCurve* curveValue, const MinMaxGradient* colorValue,
                    float scale, bool startValue){
            Bound* b = boundOf(source);
            if (!b)
                return;
            if (slots->size() >= 255){
                // past the pad byte's tags: these uses are not rewritable
                for (uint32_t i : b->bindings) (*bindings)[i].rewritable = false;
                return;
            }
            ExposedSlot slot;
            slot.gradient = gradient;
            slot.tag = (uint8_t)(slots->size() + 1);
            slot.salt = param.stream;
            slot.scale = scale;
            slot.startValue = startValue;
            if (curveValue) slot.curve = *curveValue;
            if (colorValue) slot.color = *colorValue;
            param.pad = slot.tag;
            std::vector<unsigned char> pattern(sizeof(Param));
            std::memcpy(pattern.data(), &param, sizeof(Param));
            patterns.push_back(std::move(pattern));
            for (uint32_t i : b->bindings) (*bindings)[i].slots.push_back((uint32_t)slots->size());
            slots->push_back(std::move(slot));
        }

        // a use of an exposed value's numbers outside its CurveParam / GradientParam: its
        // parameters compile again when they change
        void raw(const void* source){
            if (Bound* b = boundOf(source)){
                for (uint32_t i : b->bindings) (*bindings)[i].rewritable = false;
            }
        }

        // `source`: the document's value when `v` is an adjusted copy of it
        CurveParam curve(const MinMaxCurve& v, ModuleId module, uint32_t stream, float scale = 1.0f, bool startValue = false,
                         const MinMaxCurve* source = nullptr){
            CurveParam c;
            buildCurve(v, randomSalt((uint32_t)module, stream), scale, startValue, append(), c);
            if (!bound.empty())
                record(source ? source : &v, c, false, &v, nullptr, scale, startValue);
            return c;
        }

        GradientParam gradient(const MinMaxGradient& v, ModuleId module, uint32_t stream, bool startValue = false, const MinMaxGradient* source = nullptr){
            GradientParam g;
            buildGradient(v, randomSalt((uint32_t)module, stream), startValue, append(), g);
            if (!bound.empty())
                record(source ? source : &v, g, true, nullptr, &v, 1.0f, startValue);
            // a ramp that cannot be read draws white (S49 R12)
            if (v.mode == GradientValueMode::Ramp && g.kind != GradientParam::Ramp)
                diagnostics.push_back({Diagnostic::Severity::Warning, std::string(ParticleModules::get(module).key),
                                       "the palette ramp \"" + v.ramp + "\" cannot be read: drawn white", "ramp_missing",
                                       "pick a ramp of a .palette in the project"});
            return g;
        }

        void warn(const std::string& path, const std::string& message){
            diagnostics.push_back({Diagnostic::Severity::Warning, path, message});
        }
    };

    bool on(bool present, bool enabled){ return present && enabled; }

    // R = Ry * Rx * Rz of Euler degrees (Z first, then X, then Y), column-major 3x3
    void eulerMatrix(const Vector3& degrees, float m[9]){
        const float x = degrees.x * kDegToRad, y = degrees.y * kDegToRad, z = degrees.z * kDegToRad;
        const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z), sz = std::sin(z);
        // columns of Ry Rx Rz
        m[0] = cy * cz + sy * sx * sz;  m[1] = cx * sz;  m[2] = -sy * cz + cy * sx * sz;
        m[3] = -cy * sz + sy * sx * cz; m[4] = cx * cz;  m[5] = sy * sz + cy * sx * cz;
        m[6] = sy * cx;                 m[7] = -sx;      m[8] = cy * cx;
    }

    // ---------------------------------------------------------------- pixel art (S49)

    // the camera-facing modes the Pixel Art module applies to (R1)
    bool pixelArtMode(RenderMode mode){
        return mode == RenderMode::Billboard || mode == RenderMode::FacingCameraPosition || mode == RenderMode::StretchedBillboard ||
               mode == RenderMode::VelocityAligned || mode == RenderMode::Pixel;
    }

    void compilePixel(Context& cx, RenderParams& r, const ColorOverLifetimeModule& col, const ColorBySpeedModule& cbs, bool replaceColor, bool storedColor){
        const ParticleEffectDocument& d = cx.doc;
        const RendererModule& rm = d.renderer;
        const PixelArtModule& pa = d.pixelArt;
        PixelParams& p = r.pixel;
        const bool module = on(pa.present, pa.enabled);
        if (module && rm.enabled && rm.renderMode == RenderMode::Mesh)
            cx.diagnostics.push_back({Diagnostic::Severity::Warning, "modules.pixelArt", "Mesh particles ignore the Pixel Art module", "pixel_art_mesh",
                                      "use Render Mode Pixel or a billboard mode"});
        p.shapes = r.mode == RenderMode::Pixel ? 1 : 0;
        p.module = (module && pixelArtMode(r.mode)) ? 1 : 0;
        p.active = (p.shapes || p.module) ? 1 : 0;
        if (!p.active)
            return;
        // without the module the Pixel mode keeps S20's look otherwise: blended, unsnapped sizes in pixels
        const PixelArtModule defaults;
        const PixelArtModule& m = p.module ? pa : defaults;
        p.snap = (p.module && m.snap) ? 1 : 0;
        p.pixelSizes = (!p.module || m.pixelSizes) ? 1 : 0;
        p.integerSize = (!p.module || m.integerSize) ? 1 : 0;
        p.fade = p.module ? m.fade : PixelFade::Alpha;
        p.ditherSize = (uint8_t)(m.ditherSize <= 2 ? 2 : (m.ditherSize >= 8 ? 8 : 4));
        p.ditherSpace = m.ditherSpace;
        p.secondary = m.secondary;
        p.secondaryOffset = (int8_t)std::clamp(m.secondaryOffset, -8, 8);
        p.secondaryColor[0] = m.secondaryColor.x; p.secondaryColor[1] = m.secondaryColor.y;
        p.secondaryColor[2] = m.secondaryColor.z; p.secondaryColor[3] = m.secondaryColor.w;
        p.holdFrameRate = p.module ? std::max(m.holdFrameRate, 0.0f) : 0.0f;
        p.fadeCurve = (p.module && !(m.fadeOverLifetime.isConstant() && m.fadeOverLifetime.constant == 1.0f)) ? 1 : 0;
        if (p.fadeCurve)
            p.fadeOverLifetime = cx.curve(m.fadeOverLifetime, ModuleId::PixelArt, 4);
        p.wobble = (p.module && (m.wobbleAmplitude.x != 0.0f || m.wobbleAmplitude.y != 0.0f)) ? 1 : 0;
        p.wobbleRandomPhase = m.wobbleRandomPhase ? 1 : 0;
        p.wobbleAmplitude[0] = m.wobbleAmplitude.x; p.wobbleAmplitude[1] = m.wobbleAmplitude.y;
        p.wobbleFrequency[0] = m.wobbleFrequency.x; p.wobbleFrequency[1] = m.wobbleFrequency.y;
        p.flickerBelow = p.module ? std::clamp(m.flickerBelow, 0.0f, 1.0f) : 0.0f;
        p.flickerRate = std::max(m.flickerRate, 0.1f);
        p.shape = rm.pixelShape;
        p.streakLengthScale = std::max(rm.streakLengthScale, 0.0f);
        p.streakMaxLength = std::clamp(rm.streakMaxLength, 1.0f, 64.0f);
        p.streakHeadFraction = std::clamp(rm.streakHeadFraction, 0.0f, 1.0f);
        p.streakWidth = (uint32_t)std::clamp(rm.streakWidth, 1, 3);
        p.discRim = rm.discRim ? 1 : 0;
        p.ringWidth = (uint32_t)std::clamp(rm.ringWidth, 1, 8);
        p.dotShadeBottom = rm.dotShadeBottom ? 1 : 0;
        p.glowBands = (uint32_t)std::clamp(rm.glowBands, 1, 4);
        p.rotationSteps = (uint32_t)std::clamp(rm.rotationSteps, 1, 360);

        // the ramp: Color over Lifetime's when it is one, else Start Color's (R6)
        const bool colOn = on(col.present, col.enabled);
        const bool cbsOn = on(cbs.present, cbs.enabled);
        if (colOn && !col.setMode && r.colorOverLifetime && r.color.kind == GradientParam::Ramp){
            p.rampSource = 2;
            p.ramp = r.color;
        }else if (d.main.startColor.mode == GradientValueMode::Ramp){
            p.ramp = cx.gradient(d.main.startColor, ModuleId::Main, 10, true);
            p.rampSource = p.ramp.kind == GradientParam::Ramp ? 1 : 0;
        }
        // the ramp's bytes are drawn as they are when every other colour is white (R13)
        const MinMaxGradient& start = d.main.startColor;
        const bool startWhiteRgb = start.mode == GradientValueMode::Color && start.color.x == 1.0f && start.color.y == 1.0f && start.color.z == 1.0f;
        const bool othersWhite = !cbsOn && !replaceColor && !storedColor;
        if (p.rampSource == 2)
            p.exactColor = (startWhiteRgb && othersWhite) ? 1 : 0;
        else if (p.rampSource == 1)
            p.exactColor = (othersWhite && (!colOn || (col.color.mode == GradientValueMode::Color && col.color.color.x == 1.0f &&
                                                      col.color.color.y == 1.0f && col.color.color.z == 1.0f))) ? 1 : 0;
    }

    // ---------------------------------------------------------------- the Low noise field

    // OpenEngine's potential gradient with its lattice wrapped every kNoiseLowPeriod cells, so
    // the baked grid tiles without seams
    noise::Curl3 periodicPotential(const noise::Curl3& point, uint32_t seed){
        using namespace noise::gradientcurl3d;
        const uint32_t period = (uint32_t)kNoiseLowPeriod;
        uint32_t cell[3]{};
        noise::Curl3 fraction, fade, fadeSlope;
        for (uint32_t axis = 0; axis < 3; ++axis){
            const float wrapped = point[axis] - std::floor(point[axis] / kNoiseLowPeriod) * kNoiseLowPeriod;
            const float whole = std::floor(wrapped);
            cell[axis] = (uint32_t)whole % period;
            const float t = wrapped - whole;
            fraction[axis] = t;
            fade[axis] = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            fadeSlope[axis] = 30.0f * t * t * (t - 1.0f) * (t - 1.0f);
        }
        noise::Curl3 result;
        for (uint32_t corner = 0; corner < 8; ++corner){
            uint32_t h = seed;
            noise::Curl3 delta, weights, slopes;
            for (uint32_t axis = 0; axis < 3; ++axis){
                const uint32_t upper = (corner >> axis) & 1u;
                h ^= ((cell[axis] + upper) % period) * kAxisPrimes[axis];
                delta[axis] = fraction[axis] - (float)upper;
                weights[axis] = upper ? fade[axis] : 1.0f - fade[axis];
                slopes[axis] = upper ? fadeSlope[axis] : -fadeSlope[axis];
            }
            h = hash(h);
            const uint32_t absent = h % 3u;
            uint32_t sign = h >> 2;
            noise::Curl3 gradient;
            for (uint32_t axis = 0; axis < 3; ++axis){
                if (axis != absent){
                    gradient[axis] = (sign & 1u) ? 1.0f : -1.0f;
                    sign >>= 1;
                }
            }
            const float dot = gradient[0] * delta[0] + gradient[1] * delta[1] + gradient[2] * delta[2];
            const float weight = weights[0] * weights[1] * weights[2];
            for (uint32_t axis = 0; axis < 3; ++axis){
                float ws = slopes[axis];
                for (uint32_t other = 0; other < 3; ++other){
                    if (other != axis) ws *= weights[other];
                }
                result[axis] += gradient[axis] * weight + dot * ws;
            }
        }
        return result;
    }

    uint32_t bakeLowField(Context& cx, uint32_t seed){
        const uint32_t n = kNoiseLowCells;
        std::vector<float> field((size_t)n * n * n * 3);
        const float step = kNoiseLowPeriod / (float)n;
        for (uint32_t z = 0; z < n; z++){
            for (uint32_t y = 0; y < n; y++){
                for (uint32_t x = 0; x < n; x++){
                    noise::Curl3 p;
                    p[0] = x * step; p[1] = y * step; p[2] = z * step;
                    noise::Curl3 potential[3];
                    for (uint32_t c = 0; c < 3; c++){
                        noise::Curl3 rotated;
                        for (uint32_t axis = 0; axis < 3; axis++) rotated[axis] = p[(c + axis + 1) % 3];
                        const noise::Curl3 g = periodicPotential(rotated, noise::gradientcurl3d::hash(seed + c * 0x9e3779b9u));
                        for (uint32_t axis = 0; axis < 3; axis++) potential[c][(c + axis + 1) % 3] = g[axis];
                    }
                    float* out = &field[((size_t)(z * n + y) * n + x) * 3];
                    out[0] = potential[2][1] - potential[1][2];
                    out[1] = potential[0][2] - potential[2][0];
                    out[2] = potential[1][0] - potential[0][1];
                }
            }
        }
        return cx.pool.addFloats(field.data(), field.size());
    }

    // ---------------------------------------------------------------- what is ticked

    struct Stored {
        bool rotation = false;      // ChRotation ticked
        bool size = false;          // ChSize*
        bool size3D = false;
        bool color = false;         // ChColor*
        bool accel = false;         // ChAccel*
        bool frame = false;
        bool quat = false;
        bool angularVelocity3D = false;
        bool startRotation = false; // ChStartRotation (not a pure function of the spawn index)
    };

    // the rotation is a closed form of the age: a constant (or per-particle random) rate, or a
    // curve over normalized age whose integral is baked
    bool drivenPurely(const MinMaxCurve& c){
        return !c.usesCurve() || c.driver == CurveDriver::NormalizedAge;
    }

    Stored decideStorage(const ParticleEffectDocument& d){
        Stored s;
        const bool setOn = on(d.setOverAge.present, d.setOverAge.enabled);
        if (setOn){
            for (const SetOverAgeEntry& e : d.setOverAge.entries){
                switch (e.property){
                    case SetProperty::Rotation: s.rotation = true; break;
                    case SetProperty::Size: s.size = true; break;
                    case SetProperty::Scale: s.size = true; s.size3D = true; break;
                    case SetProperty::Color:
                    case SetProperty::Alpha: s.color = true; break;
                    case SetProperty::Acceleration: s.accel = true; break;
                    case SetProperty::Frame: s.frame = true; break;
                    default: break;
                }
            }
        }
        if (on(d.colorOverLifetime.present, d.colorOverLifetime.enabled) && d.colorOverLifetime.setMode)
            s.color = true;
        if (on(d.rotationBySpeed.present, d.rotationBySpeed.enabled))
            s.rotation = true;
        if (on(d.noise.present, d.noise.enabled) && !d.noise.rotationAmount.isZero())
            s.rotation = true;
        if (on(d.rotationOverLifetime.present, d.rotationOverLifetime.enabled) && !drivenPurely(d.rotationOverLifetime.angularVelocity))
            s.rotation = true;
        // a particle at rest stops turning: its rotation is ticked, so the tick can hold it (S49 R14)
        if (on(d.rotationOverLifetime.present, d.rotationOverLifetime.enabled) && on(d.collision.present, d.collision.enabled) &&
                d.collision.settleSpeed > 0.0f)
            s.rotation = true;
        const bool mesh = d.renderer.enabled && (d.renderer.renderMode == RenderMode::Mesh || d.renderer.renderMode == RenderMode::Ring);
        if (mesh){
            s.quat = true;
            s.angularVelocity3D = on(d.rotationOverLifetime.present, d.rotationOverLifetime.enabled) || on(d.rotationBySpeed.present, d.rotationBySpeed.enabled);
            if (s.rotation && !setOn)
                s.rotation = false;   // a mesh turns its quaternion
        }else if (d.main.startRotation3D){
            s.quat = false;
        }
        // a start rotation over the emitter time (or a parent's) cannot be recomputed from the spawn index
        if (!s.rotation && !s.quat && d.main.startRotation.usesCurve() && d.main.startRotation.driver != CurveDriver::Random)
            s.startRotation = true;
        // nor a billboard's roll aligned to its start direction (Shape's Align to Direction)
        if (!s.rotation && !s.quat && on(d.shape.present, d.shape.enabled) && d.shape.alignToDirection)
            s.startRotation = true;
        return s;
    }

    // ---------------------------------------------------------------- shape

    void compileShape(Context& cx, ShapeParams& p){
        const ShapeModule& s = cx.doc.shape;
        if (!on(s.present, s.enabled)){
            p.kind = SpawnShapeKind::Point;
            return;
        }
        p.emitFrom = s.emitFrom;
        p.radius = s.radius;
        p.innerRatio = 1.0f - std::clamp(s.radiusThickness, 0.0f, 1.0f);
        p.tanAngle = std::tan(std::min(s.angle, 89.9f) * kDegToRad);
        p.length = s.length;
        p.arc = std::clamp(s.arc, 0.0f, 360.0f) * kDegToRad;
        p.arcDegrees = std::clamp(s.arc, 0.0f, 360.0f);
        p.arcMode = s.arcMode;
        p.arcSpread = std::clamp(s.arcSpread, 0.0f, 1.0f);
        p.radiusMode = s.radiusMode;
        p.radiusSpread = std::clamp(s.radiusSpread, 0.0f, 1.0f);
        p.alignToDirection = s.alignToDirection ? 1 : 0;
        p.donutRadius = s.donutRadius;
        p.randomizeDirection = s.randomizeDirection;
        p.spherizeDirection = s.spherizeDirection;
        p.randomizePosition = s.randomizePosition;
        Vector3 scale = s.scale;
        switch (s.shape){
            case ShapeType::Sphere: p.kind = SpawnShapeKind::Sphere; break;
            case ShapeType::Hemisphere: p.kind = SpawnShapeKind::Hemisphere; break;
            case ShapeType::Cone: p.kind = SpawnShapeKind::Cone; break;
            case ShapeType::Circle: p.kind = SpawnShapeKind::Circle; break;
            case ShapeType::Donut: p.kind = SpawnShapeKind::Donut; break;
            case ShapeType::Edge: p.kind = SpawnShapeKind::Edge; break;
            case ShapeType::Box:
            case ShapeType::Rectangle:
                // the scale is the size of the box
                p.kind = s.shape == ShapeType::Box ? SpawnShapeKind::Box : SpawnShapeKind::Rectangle;
                p.boxHalf[0] = s.scale.x * 0.5f;
                p.boxHalf[1] = s.scale.y * 0.5f;
                p.boxHalf[2] = s.scale.z * 0.5f;
                for (int c = 0; c < 3; c++){
                    const float half = p.boxHalf[c];
                    const float thickness = (&s.boxThickness.x)[c];
                    p.boxInner[c] = half > 0.0f ? std::clamp(1.0f - thickness / half, 0.0f, 1.0f) : 0.0f;
                }
                if (s.emitFrom == ShapeEmitFrom::Base || s.emitFrom == ShapeEmitFrom::Vertex || s.emitFrom == ShapeEmitFrom::Triangle)
                    p.emitFrom = ShapeEmitFrom::Volume;
                scale = Vector3(1.0f, 1.0f, 1.0f);
                break;
            case ShapeType::Mesh: {
                p.kind = SpawnShapeKind::Mesh;
                MeshGeometry geometry;
                if (!cx.env.mesh || !cx.env.mesh(s.mesh, geometry) || geometry.positions.empty()){
                    cx.warn("shape.mesh", "the mesh could not be read: particles start at the shape's origin");
                    p.kind = SpawnShapeKind::Point;
                    break;
                }
                if (geometry.normals.size() != geometry.positions.size())
                    geometry.normals.assign(geometry.positions.size(), 0.0f);
                p.meshVertexCount = (uint32_t)(geometry.positions.size() / 3);
                p.meshVertices = cx.pool.addFloats(geometry.positions.data(), geometry.positions.size());
                p.meshNormals = cx.pool.addFloats(geometry.normals.data(), geometry.normals.size());
                // triangles by area: the cumulative table, normalized
                std::vector<uint32_t> tris;
                std::vector<float> cdf;
                float total = 0.0f;
                for (size_t t = 0; t + 2 < geometry.indices.size(); t += 3){
                    const uint32_t a = geometry.indices[t], b = geometry.indices[t + 1], c = geometry.indices[t + 2];
                    if (a >= p.meshVertexCount || b >= p.meshVertexCount || c >= p.meshVertexCount)
                        continue;
                    const float* pa = &geometry.positions[a * 3];
                    const float* pb = &geometry.positions[b * 3];
                    const float* pc = &geometry.positions[c * 3];
                    const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
                    const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
                    const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                    total += std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
                    cdf.push_back(total);
                    tris.push_back(a); tris.push_back(b); tris.push_back(c);
                }
                for (float& v : cdf) v = total > 0.0f ? v / total : 1.0f;
                p.meshTriangles = (uint32_t)(tris.size() / 3);
                if (!tris.empty()){
                    p.meshIndices = cx.pool.addUints(tris.data(), tris.size());
                    p.meshAreaCdf = cx.pool.addFloats(cdf.data(), cdf.size());
                }else if (p.emitFrom != ShapeEmitFrom::Vertex){
                    p.emitFrom = ShapeEmitFrom::Vertex;
                }
                if (p.emitFrom != ShapeEmitFrom::Vertex && p.emitFrom != ShapeEmitFrom::Edge)
                    p.emitFrom = ShapeEmitFrom::Triangle;
                break;
            }
        }
        // the shape's transform: position, rotation, scale
        float r[9];
        eulerMatrix(s.rotation, r);
        std::memcpy(p.rotation, r, sizeof(r));
        p.transform[0] = r[0] * scale.x; p.transform[1] = r[1] * scale.x; p.transform[2] = r[2] * scale.x;
        p.transform[3] = r[3] * scale.y; p.transform[4] = r[4] * scale.y; p.transform[5] = r[5] * scale.y;
        p.transform[6] = r[6] * scale.z; p.transform[7] = r[7] * scale.z; p.transform[8] = r[8] * scale.z;
        p.transform[9] = s.position.x; p.transform[10] = s.position.y; p.transform[11] = s.position.z;
    }

    float maxOf(const MinMaxCurve& c){
        float lo, hi;
        c.range(lo, hi);
        return hi;
    }

    // the integral over normalized age of a curve, baked next to it (Rotation over Lifetime)
    uint32_t integralLut(Context& cx, const MinMaxCurve& v, bool second, uint32_t size){
        const Curve& curve = second ? v.curveMax : v.curve;
        std::vector<float> values(size);
        double sum = 0.0;
        values[0] = 0.0f;
        const int steps = 8;
        for (uint32_t i = 1; i < size; i++){
            const double a = (double)(i - 1) / (size - 1), b = (double)i / (size - 1);
            for (int s = 0; s < steps; s++){
                const double t0 = a + (b - a) * s / steps, t1 = a + (b - a) * (s + 1) / steps;
                sum += 0.5 * (curve.evaluate((float)t0) + curve.evaluate((float)t1)) * (t1 - t0);
            }
            values[i] = (float)(sum * v.multiplier * kDegToRad);
        }
        return cx.pool.addFloats(values.data(), values.size());
    }

}

namespace {

    // what the bounds and the collision radii read of the sizes and speeds (also after
    // applyExposed changed one of them)
    void sizeHints(const ParticleEffectDocument& d, ParticleProgram& prog){
        float lo, hi;
        d.main.startSize.range(lo, hi);
        prog.maxStartSize = std::max(hi, 0.0f);
        if (d.main.startSize3D){
            prog.maxStartSize = std::max({prog.maxStartSize, maxOf(d.main.startSizeY), maxOf(d.main.startSizeZ)});
        }
        prog.maxSpeedHint = std::max(std::fabs(maxOf(d.main.startSpeed)), 0.0f);
        // the bounds' padding: the largest size multiplier the modules reach
        const RenderParams& r = prog.render;
        const SizeOverLifetimeModule& sol = d.sizeOverLifetime;
        const SizeBySpeedModule& sbs = d.sizeBySpeed;
        float factor = 1.0f;
        if (r.sizeOverLifetime)
            factor *= std::max({maxOf(sol.size), sol.separateAxes ? maxOf(sol.sizeY) : 0.0f, sol.separateAxes ? maxOf(sol.sizeZ) : 0.0f, 0.0f});
        if (r.sizeBySpeed)
            factor *= std::max({maxOf(sbs.size), sbs.separateAxes ? maxOf(sbs.sizeY) : 0.0f, sbs.separateAxes ? maxOf(sbs.sizeZ) : 0.0f, 0.0f});
        if (r.noiseSize)
            factor *= 1.0f + std::fabs(maxOf(d.noise.sizeAmount)) * std::fabs(maxOf(d.noise.strength));
        if (r.storedSize && on(d.setOverAge.present, d.setOverAge.enabled)){
            for (const SetOverAgeEntry& e : d.setOverAge.entries){
                if (e.property == SetProperty::Size || e.property == SetProperty::Scale){
                    const float big = std::max({std::fabs(e.from.x), std::fabs(e.to.x), std::fabs(e.from.y), std::fabs(e.to.y), std::fabs(e.from.z), std::fabs(e.to.z)});
                    prog.maxStartSize = std::max(prog.maxStartSize, big);
                }
            }
        }
        prog.maxSizeFactor = std::max(factor, 1.0e-3f);
    }

    // where the copies of each slot's param went: the pool, the program object (its params
    // outside the pool) and the bursts. The pad byte's tag and the random stream make each
    // pattern unique.
    void locateSlots(const ParticleProgram& prog, const std::vector<std::vector<unsigned char>>& patterns, std::vector<ExposedSlot>& slots){
        auto find = [](const unsigned char* base, size_t bytes, const std::vector<unsigned char>& pattern, std::vector<uint32_t>& out){
            for (size_t at = 0; at + pattern.size() <= bytes; at += 4){
                if (std::memcmp(base + at, pattern.data(), pattern.size()) == 0)
                    out.push_back((uint32_t)at);
            }
        };
        for (size_t i = 0; i < slots.size(); i++){
            find(reinterpret_cast<const unsigned char*>(prog.pool.data()), prog.pool.size() * sizeof(uint32_t), patterns[i], slots[i].inPool);
            find(reinterpret_cast<const unsigned char*>(&prog), sizeof(ParticleProgram), patterns[i], slots[i].inProgram);
            find(reinterpret_cast<const unsigned char*>(prog.emission.bursts.data()), prog.emission.bursts.size() * sizeof(BurstParam), patterns[i], slots[i].inBursts);
        }
    }

}

uint32_t ParticleCompiler::lutSize(const Curve& curve, float tolerance){
    if (curve.keys.size() < 2)
        return 65;
    for (uint32_t size = 65; size < 1025; size = (size - 1) * 2 + 1){
        bool ok = true;
        for (uint32_t i = 0; i + 1 < size && ok; i++){
            const float t0 = (float)i / (float)(size - 1), t1 = (float)(i + 1) / (float)(size - 1);
            const float lerp = 0.5f * (curve.evaluate(t0) + curve.evaluate(t1));
            ok = std::fabs(lerp - curve.evaluate(0.5f * (t0 + t1))) <= tolerance;
        }
        if (ok)
            return size;
    }
    return 1025;
}

const float* ParticleCompiler::srgbDecodeTable(){
    static const std::vector<float> table = [](){
        std::vector<float> t(256);
        for (int i = 0; i < 256; i++)
            t[i] = std::pow((float)i / 255.0f, 2.2f);
        return t;
    }();
    return table.data();
}

void ParticleCompiler::shapeOutline(const ShapeModule& s, std::vector<Vector3>& out){
    out.clear();
    float r[9];
    eulerMatrix(s.rotation, r);
    const bool box = s.shape == ShapeType::Box || s.shape == ShapeType::Rectangle;
    // a box's scale is its size; the other shapes scale by it
    const Vector3 scale = box ? Vector3(1.0f, 1.0f, 1.0f) : s.scale;
    auto toEmitter = [&](const Vector3& p){
        const float x = p.x * scale.x, y = p.y * scale.y, z = p.z * scale.z;
        return Vector3(r[0] * x + r[3] * y + r[6] * z + s.position.x,
                       r[1] * x + r[4] * y + r[7] * z + s.position.y,
                       r[2] * x + r[5] * y + r[8] * z + s.position.z);
    };
    auto segment = [&](const Vector3& a, const Vector3& b){
        out.push_back(toEmitter(a));
        out.push_back(toEmitter(b));
    };
    const int steps = 32;
    // an arc of a circle around +Y at height y, or around another axis by `plane`
    // (0: XZ, 1: XY, 2: YZ), from angle `from` over `span`
    auto arc = [&](float radius, float y, float from, float span, int plane){
        auto at = [&](float a){
            const float c = std::cos(a) * radius, sn = std::sin(a) * radius;
            if (plane == 1) return Vector3(c, sn, 0.0f);
            if (plane == 2) return Vector3(0.0f, sn, c);
            return Vector3(c, y, sn);
        };
        for (int i = 0; i < steps; i++)
            segment(at(from + span * (float)i / steps), at(from + span * (float)(i + 1) / steps));
    };
    const float pi = 3.14159265358979f;
    const float span = std::clamp(s.arc, 0.0f, 360.0f) * kDegToRad;
    switch (s.shape){
        case ShapeType::Sphere:
            arc(s.radius, 0.0f, 0.0f, 2.0f * pi, 0);
            arc(s.radius, 0.0f, 0.0f, 2.0f * pi, 1);
            arc(s.radius, 0.0f, 0.0f, 2.0f * pi, 2);
            break;
        case ShapeType::Hemisphere:
            arc(s.radius, 0.0f, 0.0f, 2.0f * pi, 0);
            arc(s.radius, 0.0f, 0.0f, pi, 1);
            arc(s.radius, 0.0f, 0.0f, pi, 2);
            break;
        case ShapeType::Cone: {
            const float top = s.radius + s.length * std::tan(std::min(s.angle, 89.9f) * kDegToRad);
            arc(s.radius, 0.0f, 0.0f, span, 0);
            arc(top, s.length, 0.0f, span, 0);
            for (int i = 0; i < 4; i++){
                const float a = span * (float)i / 4.0f;
                segment(Vector3(std::cos(a) * s.radius, 0.0f, std::sin(a) * s.radius), Vector3(std::cos(a) * top, s.length, std::sin(a) * top));
            }
            break;
        }
        case ShapeType::Circle:
            arc(s.radius, 0.0f, 0.0f, span, 0);
            if (span < 2.0f * pi){
                segment(Vector3(0.0f, 0.0f, 0.0f), Vector3(s.radius, 0.0f, 0.0f));
                segment(Vector3(0.0f, 0.0f, 0.0f), Vector3(std::cos(span) * s.radius, 0.0f, std::sin(span) * s.radius));
            }
            break;
        case ShapeType::Donut:
            arc(s.radius + s.donutRadius, 0.0f, 0.0f, span, 0);
            arc(std::max(0.0f, s.radius - s.donutRadius), 0.0f, 0.0f, span, 0);
            arc(s.radius, s.donutRadius, 0.0f, span, 0);
            arc(s.radius, -s.donutRadius, 0.0f, span, 0);
            break;
        case ShapeType::Edge:
            segment(Vector3(-s.radius, 0.0f, 0.0f), Vector3(s.radius, 0.0f, 0.0f));
            break;
        case ShapeType::Box: {
            const Vector3 h = s.scale * 0.5f;
            const Vector3 c[8] = {
                Vector3(-h.x, -h.y, -h.z), Vector3(h.x, -h.y, -h.z), Vector3(h.x, h.y, -h.z), Vector3(-h.x, h.y, -h.z),
                Vector3(-h.x, -h.y, h.z), Vector3(h.x, -h.y, h.z), Vector3(h.x, h.y, h.z), Vector3(-h.x, h.y, h.z)
            };
            const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : edges) segment(c[e[0]], c[e[1]]);
            break;
        }
        case ShapeType::Rectangle: {
            const float hx = s.scale.x * 0.5f, hz = s.scale.z * 0.5f;
            segment(Vector3(-hx, 0.0f, -hz), Vector3(hx, 0.0f, -hz));
            segment(Vector3(hx, 0.0f, -hz), Vector3(hx, 0.0f, hz));
            segment(Vector3(hx, 0.0f, hz), Vector3(-hx, 0.0f, hz));
            segment(Vector3(-hx, 0.0f, hz), Vector3(-hx, 0.0f, -hz));
            break;
        }
        case ShapeType::Mesh:
            // the mesh shows itself; a cross marks the shape's origin
            segment(Vector3(-0.25f, 0.0f, 0.0f), Vector3(0.25f, 0.0f, 0.0f));
            segment(Vector3(0.0f, -0.25f, 0.0f), Vector3(0.0f, 0.25f, 0.0f));
            segment(Vector3(0.0f, 0.0f, -0.25f), Vector3(0.0f, 0.0f, 0.25f));
            break;
    }
}

std::shared_ptr<CompiledEffect> ParticleCompiler::compile(const ParticleEffectDocument& document, const CompileEnvironment& environment){
    auto effect = std::make_shared<CompiledEffect>();
    compile(document, environment, *effect);
    return effect;
}

namespace {

    // compiles run on loading threads too; 32 bits: a lock-free atomic on every target (wasm32
    // and armv7 included), and the count is only ever compared before and after a change
    std::atomic<uint32_t> compiles{0};

}

uint32_t ParticleCompiler::compileCount(){
    return compiles.load(std::memory_order_relaxed);
}

void ParticleCompiler::compile(const ParticleEffectDocument& d, const CompileEnvironment& env, CompiledEffect& effect){
    compiles.fetch_add(1, std::memory_order_relaxed);
    effect.document = d;
    effect.diagnostics.clear();
    effect.program = ParticleProgram();
    effect.exposedSlots.clear();
    effect.exposedBindings.clear();
    effect.valid = ParticleEffectCodec::validate(d, effect.diagnostics);
    // an Analytic effect computes a subset of the modules (S49 R15): the rest refuse it
    if (effect.valid && d.simulationMode == SimulationMode::Analytic)
        effect.valid = AnalyticParticles::validate(d, effect.diagnostics);
    if (!effect.valid)
        return;

    ParticleProgram& prog = effect.program;
    Context cx{prog, PoolWriter(prog.pool), d, env, effect.diagnostics};
    // the exposed parameters' curve and colour values: their uses become slots (R10)
    cx.slots = &effect.exposedSlots;
    cx.bindings = &effect.exposedBindings;
    for (size_t i = 0; i < d.exposed.size(); i++){
        const ParticleModules::Resolved r = ParticleModules::resolve(const_cast<ParticleEffectDocument&>(d), d.exposed[i].path);
        ExposedBinding binding;
        binding.parameter = (uint32_t)i;
        binding.member = r.member;
        binding.rewritable = r.field && (r.field->type == FieldType::MinMaxCurve || r.field->type == FieldType::MinMaxGradient);
        effect.exposedBindings.push_back(binding);
        if (!binding.rewritable)
            continue;
        Context::Bound* b = cx.boundOf(r.storage);
        if (!b){
            cx.bound.push_back({r.storage, {}});
            b = &cx.bound.back();
        }
        b->bindings.push_back((uint32_t)i);
    }

    prog.duration = d.duration;
    prog.looping = d.looping;
    prog.prewarm = d.looping && d.prewarm;
    prog.prewarmSync = prog.prewarm && d.prewarmSync;
    prog.startDelay = cx.curve(d.startDelay, ModuleId::Main, 72);
    prog.useUnscaledTime = d.useUnscaledTime;
    prog.stopAction = d.stopAction;
    prog.emitterVelocityMode = d.emitterVelocityMode;
    prog.gravitySource = d.main.gravitySource;
    {
        const ShapeModule& sh = d.shape;
        const bool shapeOn = on(sh.present, sh.enabled);
        auto moving = [](ShapeMultiMode m){ return m == ShapeMultiMode::Loop || m == ShapeMultiMode::PingPong; };
        prog.shapePhases = shapeOn && (moving(sh.arcMode) || moving(sh.radiusMode));
        if (prog.shapePhases){
            // over the emitter time, one pick per play (a range: by the seed)
            MinMaxCurve arcSpeed = sh.arcSpeed, radiusSpeed = sh.radiusSpeed;
            if (arcSpeed.driver != CurveDriver::Random) arcSpeed.driver = CurveDriver::EmitterTime;
            if (radiusSpeed.driver != CurveDriver::Random) radiusSpeed.driver = CurveDriver::EmitterTime;
            prog.arcSpeed = moving(sh.arcMode) ? cx.curve(arcSpeed, ModuleId::Shape, 20, 360.0f, false, &sh.arcSpeed) : CurveParam();
            prog.radiusSpeed = moving(sh.radiusMode) ? cx.curve(radiusSpeed, ModuleId::Shape, 21, 1.0f, false, &sh.radiusSpeed) : CurveParam();
        }
        const NoiseModule& n = d.noise;
        prog.noiseScroll = on(n.present, n.enabled) && !n.scrollSpeed.isConstant();
        if (prog.noiseScroll){
            MinMaxCurve scroll = n.scrollSpeed;
            if (scroll.driver != CurveDriver::Random) scroll.driver = CurveDriver::EmitterTime;
            prog.noiseScrollSpeed = cx.curve(scroll, ModuleId::Noise, 10, 1.0f, false, &n.scrollSpeed);
        }
    }
    prog.space = d.simulationSpace;
    prog.tickRate = d.tickRate;
    prog.interpolate = d.interpolate && d.tickRate > 0;
    prog.maxParticles = std::clamp<uint32_t>(d.maxParticles, 1u, kMaxParticlesPerEmitter);
    prog.seed = d.seed;
    prog.scalingMode = d.scalingMode;
    prog.simulationSpeed = d.simulationSpeed;
    prog.cullingMode = d.cullingMode;
    prog.boundsMode = d.boundsMode;
    prog.boundsCenter[0] = d.boundsCenter.x; prog.boundsCenter[1] = d.boundsCenter.y; prog.boundsCenter[2] = d.boundsCenter.z;
    prog.boundsSize[0] = d.boundsSize.x; prog.boundsSize[1] = d.boundsSize.y; prog.boundsSize[2] = d.boundsSize.z;
    prog.priority = d.priority;
    prog.lod = d.lod.enabled;
    if (prog.lod){
        prog.lodEmission = cx.curve(d.lod.emissionScale, ModuleId::Main, 70);
        prog.lodThinning = cx.curve(d.lod.renderThinning, ModuleId::Main, 71);
        prog.lodMaxDistance = std::max(d.lod.maxDistance, 1.0e-3f);
    }
    prog.startSizeFromTexture = d.main.startSizeFromTexture;

    const Stored stored = decideStorage(d);
    ChannelMask& ch = prog.channels;
    ch = kCoreChannels;
    auto need = [&](std::initializer_list<Channel> list){ for (Channel c : list) ch |= channelBit(c); };
    if (stored.rotation) need({ChRotation, ChPrevRotation});
    if (stored.startRotation || env.inheritsRotation) need({ChStartRotation});
    if (stored.size) need({ChSizeX, ChPrevSizeX});
    if (stored.size3D) need({ChSizeY, ChSizeZ});
    if (d.main.startSize3D) need({ChStartSizeY, ChStartSizeZ});
    if (stored.quat) need({ChQuatX, ChQuatY, ChQuatZ, ChQuatW});
    if (stored.angularVelocity3D) need({ChAngVelX, ChAngVelY, ChAngVelZ});
    if (stored.color) need({ChColorR, ChColorG, ChColorB, ChColorA});
    if (stored.accel) need({ChAccelX, ChAccelY, ChAccelZ});
    if (stored.frame) need({ChFrame});
    const bool noiseOn = on(d.noise.present, d.noise.enabled);
    if (noiseOn && !d.noise.sizeAmount.isZero()) need({ChNoiseSize});
    const bool collisionOn = on(d.collision.present, d.collision.enabled);
    const bool worldCollision = collisionOn && (d.collision.type == CollisionType::World || d.collision.type == CollisionType::World2D);
    if (worldCollision) need({ChPlaneNX, ChPlaneNY, ChPlaneNZ, ChPlaneD, ChPlaneTick});
    // only an effect that settles pays for the flag (S49 R14)
    if (collisionOn && d.collision.settleSpeed > 0.0f) need({ChSettled});
    const bool trailsOn = on(d.trails.present, d.trails.enabled) && d.trails.mode == TrailMode::PerParticle;
    if (trailsOn) need({ChTrailSlot});
    {
        // the renderer draws along the velocity: the animated part is kept for it (the total
        // velocity, Unity's)
        const RendererModule& rm = d.renderer;
        const VelocityOverLifetimeModule& v = d.velocityOverLifetime;
        const bool animated = noiseOn || (on(v.present, v.enabled) &&
            !(v.x.isZero() && v.y.isZero() && v.z.isZero() && v.orbitalX.isZero() && v.orbitalY.isZero() &&
              v.orbitalZ.isZero() && v.radial.isZero()));
        const bool alongVelocity = rm.renderMode == RenderMode::StretchedBillboard ||
            ((rm.renderMode == RenderMode::Mesh || rm.renderMode == RenderMode::Ring) && rm.meshAlignment == MeshAlignment::Velocity);
        if (animated && alongVelocity) need({ChAnimVelX, ChAnimVelY, ChAnimVelZ});
    }
    prog.inheritsRotation = env.inheritsRotation;
    prog.followsParent = env.followsParent;
    prog.followsRotation = env.followsParent && env.followsRotation;
    prog.followsScale = env.followsParent && env.followsScale;
    if (prog.followsParent) need({ChParentId});
    if (prog.followsRotation) need({ChFollowQuatX, ChFollowQuatY, ChFollowQuatZ, ChFollowQuatW});
    if (prog.followsScale) need({ChFollowSize});

    // ---- emission
    {
        EmissionParams& e = prog.emission;
        e.enabled = on(d.emission.present, d.emission.enabled);
        if (e.enabled){
            // rates follow the emitter time
            MinMaxCurve rate = d.emission.rateOverTime;
            if (rate.driver != CurveDriver::Random) rate.driver = CurveDriver::EmitterTime;
            e.rate = cx.curve(rate, ModuleId::Emission, 1, 1.0f, false, &d.emission.rateOverTime);
            MinMaxCurve distance = d.emission.rateOverDistance;
            if (distance.driver != CurveDriver::Random) distance.driver = CurveDriver::EmitterTime;
            e.rateOverDistance = cx.curve(distance, ModuleId::Emission, 2, 1.0f, false, &d.emission.rateOverDistance);
            for (size_t bi = 0; bi < d.emission.bursts.size(); bi++){
                const Burst& b = d.emission.bursts[bi];
                BurstParam bp;
                bp.time = b.time;
                // the constants are read here, not through a CurveParam
                cx.raw(&b.count);
                bp.countMin = b.count.mode == CurveMode::TwoConstants ? std::min(b.count.constant, b.count.constantMax) : b.count.constant;
                bp.countMax = b.count.mode == CurveMode::TwoConstants ? std::max(b.count.constant, b.count.constantMax) : b.count.constant;
                if (b.count.usesCurve()){
                    // read at the burst's time over the emitter's duration (Unity)
                    MinMaxCurve count = b.count;
                    if (count.driver != CurveDriver::Random) count.driver = CurveDriver::EmitterTime;
                    bp.countCurve = 1;
                    bp.count = cx.curve(count, ModuleId::Emission, 110u + (uint32_t)bi, 1.0f, false, &b.count);
                }
                bp.cycles = b.cycles;
                bp.interval = std::max(b.interval, 1.0e-4f);
                bp.probability = b.probability;
                e.bursts.push_back(bp);
            }
            e.maxPerTick = (uint32_t)std::max(d.emission.maxPerTick, 0);
        }
    }

    // ---- spawn ops
    {
        ShapeParams shape;
        compileShape(cx, shape);
        prog.spawnOps.push_back({OpCode::SpawnShape, cx.pool.add(shape)});

        VelocityParams vel;
        vel.startSpeed = cx.curve(d.main.startSpeed, ModuleId::Main, 1, 1.0f, true);
        if (on(d.startVelocity.present, d.startVelocity.enabled)){
            vel.hasStartVelocity = 1;
            vel.startVelocityUniform = d.startVelocity.uniform ? 1 : 0;
            vel.velMin[0] = d.startVelocity.min.x; vel.velMin[1] = d.startVelocity.min.y; vel.velMin[2] = d.startVelocity.min.z;
            vel.velMax[0] = d.startVelocity.max.x; vel.velMax[1] = d.startVelocity.max.y; vel.velMax[2] = d.startVelocity.max.z;
        }
        if (!d.main.inheritVelocity.isZero() && d.main.inheritVelocityMode == InheritVelocityMode::Initial){
            vel.hasInherit = 1;
            vel.inherit = cx.curve(d.main.inheritVelocity, ModuleId::Main, 2, 1.0f, true);
        }
        prog.spawnOps.push_back({OpCode::SpawnVelocity, cx.pool.add(vel)});

        StartParams st;
        st.lifetime = cx.curve(d.main.startLifetime, ModuleId::Main, 3, 1.0f, true);
        st.size = cx.curve(d.main.startSize, ModuleId::Main, 4, 1.0f, true);
        st.size3D = d.main.startSize3D ? 1 : 0;
        if (st.size3D){
            // linked axes share the X pick (the same stream)
            st.sizeY = cx.curve(d.main.startSizeY, ModuleId::Main, d.main.startSizeLinked ? 4 : 5, 1.0f, true);
            st.sizeZ = cx.curve(d.main.startSizeZ, ModuleId::Main, d.main.startSizeLinked ? 4 : 6, 1.0f, true);
        }
        st.rotation3D = d.main.startRotation3D ? 1 : 0;
        st.rotation = cx.curve(d.main.startRotation, ModuleId::Main, 7, kDegToRad, true);
        st.rotationX = cx.curve(d.main.startRotationX, ModuleId::Main, 8, kDegToRad, true);
        st.rotationY = cx.curve(d.main.startRotationY, ModuleId::Main, 9, kDegToRad, true);
        st.flipRotation = d.main.flipRotation;
        // Shape's Align to Direction: the start rotation turned to the shape's direction
        st.alignToDirection = (on(d.shape.present, d.shape.enabled) && d.shape.alignToDirection) ? 1 : 0;
        st.color = cx.gradient(d.main.startColor, ModuleId::Main, 10, true);
        st.storeRotation = (stored.startRotation || env.inheritsRotation) ? 1 : 0;
        st.storeQuat = stored.quat ? 1 : 0;
        st.initRotation = stored.rotation ? 1 : 0;
        st.initSize = stored.size ? 1 : 0;
        st.initColor = stored.color ? 1 : 0;
        st.initFrame = stored.frame ? 1 : 0;
        if (stored.frame){
            const TextureSheetAnimationModule& ts = d.textureSheetAnimation;
            const MinMaxCurve noFrame(0.0f);
            st.startFrame = cx.curve(ts.present ? ts.startFrame : noFrame, ModuleId::TextureSheetAnimation, 2);
            if (ts.present && !ts.startFrameList.empty()){
                std::vector<uint32_t> frames;
                for (int f : ts.startFrameList) frames.push_back((uint32_t)std::max(f, 0));
                st.startFrameList = cx.pool.addUints(frames.data(), frames.size());
                st.startFrameListCount = (uint32_t)frames.size();
            }
        }
        if (stored.angularVelocity3D){
            st.initAngularVelocity = 1;
            const RotationOverLifetimeModule& r = d.rotationOverLifetime;
            const bool rol = on(r.present, r.enabled);
            st.angularVelocity = rol ? cx.curve(r.angularVelocity, ModuleId::RotationOverLifetime, 1, kDegToRad, true) : CurveParam();
            st.angularVelocityX = rol && r.separateAxes ? cx.curve(r.angularVelocityX, ModuleId::RotationOverLifetime, 2, kDegToRad, true) : CurveParam();
            st.angularVelocityY = rol && r.separateAxes ? cx.curve(r.angularVelocityY, ModuleId::RotationOverLifetime, 3, kDegToRad, true) : CurveParam();
        }
        if (stored.accel){
            st.initAccel = 1;
            const ForceOverLifetimeModule& f = d.forceOverLifetime;
            if (on(f.present, f.enabled)){
                st.accelX = cx.curve(f.x, ModuleId::ForceOverLifetime, 1, 1.0f, true);
                st.accelY = cx.curve(f.y, ModuleId::ForceOverLifetime, 2, 1.0f, true);
                st.accelZ = cx.curve(f.z, ModuleId::ForceOverLifetime, 3, 1.0f, true);
                st.accelWorld = f.space == ValueSpace::World ? 1 : 0;
            }
        }
        prog.spawnOps.push_back({OpCode::SpawnStartValues, cx.pool.add(st)});
    }

    // ---- update ops, in order
    const bool setOn = on(d.setOverAge.present, d.setOverAge.enabled);
    const bool replaceColor = on(d.colorOverLifetime.present, d.colorOverLifetime.enabled) && d.colorOverLifetime.setMode;
    if ((setOn && !d.setOverAge.entries.empty()) || replaceColor){
        std::vector<SetParams> entries;
        for (const SetOverAgeEntry& e : setOn ? d.setOverAge.entries : std::vector<SetOverAgeEntry>()){
            SetParams s;
            s.property = e.property;
            s.shortestPath = e.shortestPath ? 1 : 0;
            s.srgb = e.srgb ? 1 : 0;
            s.fromTime = e.fromTime;
            s.toTime = e.toTime;
            s.invSpan = e.toTime != e.fromTime ? 1.0f / (e.toTime - e.fromTime) : 0.0f;
            s.from[0] = e.from.x; s.from[1] = e.from.y; s.from[2] = e.from.z; s.from[3] = e.from.w;
            s.to[0] = e.to.x; s.to[1] = e.to.y; s.to[2] = e.to.z; s.to[3] = e.to.w;
            s.curveLut = cx.curveLut(e.curve.empty() ? Curve::linear(0.0f, 1.0f) : e.curve, 1.0f, s.curveLutSize);
            if (e.property == SetProperty::Frame){
                std::vector<uint32_t> frames;
                for (int f : e.frames) frames.push_back((uint32_t)std::max(f, 0));
                s.frames = cx.pool.addUints(frames.data(), frames.size());
                s.frameCount = (uint32_t)frames.size();
            }
            s.worldSpace = d.simulationSpace == SimulationSpace::World ? 1 : 0;
            entries.push_back(s);
        }
        SetOverAgeParams sp;
        sp.count = (uint32_t)entries.size();
        if (!entries.empty()){
            sp.entries = cx.pool.add(entries[0]);
            for (size_t i = 1; i < entries.size(); i++) cx.pool.add(entries[i]);
            // the entries sit back to back: SetParams is 16-byte aligned, so is the pool's every block
            static_assert(sizeof(SetParams) % 16 == 0, "SetParams packs back to back in the pool");
        }
        if (replaceColor){
            sp.replaceColor = 1;
            sp.color = cx.gradient(d.colorOverLifetime.color, ModuleId::ColorOverLifetime, 1);
        }
        prog.updateOps.push_back({OpCode::SetOverAge, cx.pool.add(sp)});
    }

    {
        ForceParams fp;
        fp.gravity = d.main.gravityModifier.isZero() ? 0 : 1;
        if (fp.gravity) fp.gravityModifier = cx.curve(d.main.gravityModifier, ModuleId::Main, 20);
        const ForceOverLifetimeModule& f = d.forceOverLifetime;
        if (stored.accel){
            fp.ownAcceleration = 1;
        }else if (on(f.present, f.enabled) && !(f.x.isZero() && f.y.isZero() && f.z.isZero())){
            fp.force = 1;
            fp.forceWorld = f.space == ValueSpace::World ? 1 : 0;
            fp.forcePerTick = f.randomizePerTick ? 1 : 0;
            fp.fx = cx.curve(f.x, ModuleId::ForceOverLifetime, 1);
            fp.fy = cx.curve(f.y, ModuleId::ForceOverLifetime, 2);
            fp.fz = cx.curve(f.z, ModuleId::ForceOverLifetime, 3);
        }
        if (!d.main.inheritVelocity.isZero() && d.main.inheritVelocityMode == InheritVelocityMode::Current){
            fp.inheritCurrent = 1;
            fp.inherit = cx.curve(d.main.inheritVelocity, ModuleId::Main, 2);
        }
        if (fp.gravity || fp.force || fp.ownAcceleration || fp.inheritCurrent)
            prog.updateOps.push_back({OpCode::Forces, cx.pool.add(fp)});
    }

    if (on(d.externalForces.present, d.externalForces.enabled)){
        prog.hasExternalForces = true;
        prog.externalForceMask = d.externalForces.layerMask;
        prog.externalForceMultiplier = d.externalForces.multiplier;
        ProgramExternalForces ef;
        ef.multiplier = d.externalForces.multiplier;
        prog.updateOps.push_back({OpCode::ExternalForces, cx.pool.add(ef)});
    }

    uint32_t animatedOffset = 0;
    bool speedModifier = false;
    {
        const VelocityOverLifetimeModule& v = d.velocityOverLifetime;
        if (on(v.present, v.enabled)){
            AnimatedVelocityParams ap;
            ap.linear = !(v.x.isZero() && v.y.isZero() && v.z.isZero()) ? 1 : 0;
            ap.linearWorld = v.space == ValueSpace::World ? 1 : 0;
            ap.vx = cx.curve(v.x, ModuleId::VelocityOverLifetime, 1);
            ap.vy = cx.curve(v.y, ModuleId::VelocityOverLifetime, 2);
            ap.vz = cx.curve(v.z, ModuleId::VelocityOverLifetime, 3);
            ap.orbital = !(v.orbitalX.isZero() && v.orbitalY.isZero() && v.orbitalZ.isZero()) ? 1 : 0;
            ap.ox = cx.curve(v.orbitalX, ModuleId::VelocityOverLifetime, 4, kDegToRad);
            ap.oy = cx.curve(v.orbitalY, ModuleId::VelocityOverLifetime, 5, kDegToRad);
            ap.oz = cx.curve(v.orbitalZ, ModuleId::VelocityOverLifetime, 6, kDegToRad);
            ap.offsetConst = (v.offsetX.isConstant() && v.offsetY.isConstant() && v.offsetZ.isConstant()) ? 1 : 0;
            ap.offset[0] = v.offsetX.constant; ap.offset[1] = v.offsetY.constant; ap.offset[2] = v.offsetZ.constant;
            cx.raw(&v.offsetX);
            cx.raw(&v.offsetY);
            cx.raw(&v.offsetZ);
            if (!ap.offsetConst){
                ap.offX = cx.curve(v.offsetX, ModuleId::VelocityOverLifetime, 9);
                ap.offY = cx.curve(v.offsetY, ModuleId::VelocityOverLifetime, 10);
                ap.offZ = cx.curve(v.offsetZ, ModuleId::VelocityOverLifetime, 11);
            }
            ap.radial = v.radial.isZero() ? 0 : 1;
            ap.radialSpeed = cx.curve(v.radial, ModuleId::VelocityOverLifetime, 7);
            ap.speedModifier = (v.speedModifier.isConstant() && v.speedModifier.constant == 1.0f) ? 0 : 1;
            ap.speed = cx.curve(v.speedModifier, ModuleId::VelocityOverLifetime, 8);
            animatedOffset = cx.pool.add(ap);
            speedModifier = ap.speedModifier != 0;
            if (ap.linear || ap.orbital || ap.radial)
                prog.updateOps.push_back({OpCode::AnimatedVelocity, animatedOffset});
        }
    }

    if (noiseOn){
        const NoiseModule& n = d.noise;
        NoiseParams np;
        np.quality = n.quality;
        np.separateAxes = n.separateAxes ? 1 : 0;
        np.octaves = (uint8_t)std::clamp(n.octaves, 1, 4);
        np.strengthX = cx.curve(n.strength, ModuleId::Noise, 1);
        np.strengthY = cx.curve(n.strengthY, ModuleId::Noise, 2);
        np.strengthZ = cx.curve(n.strengthZ, ModuleId::Noise, 3);
        np.frequency = n.frequency;
        np.strengthScale = n.damping ? 1.0f / std::max(n.frequency, 1.0e-4f) : 1.0f;
        np.scrollSpeed = n.scrollSpeed.constant;
        cx.raw(&n.scrollSpeed);
        np.scrollAccumulated = prog.noiseScroll ? 1 : 0;
        np.octaveMultiplier = n.octaveMultiplier;
        np.octaveScale = n.octaveScale;
        np.seed = d.seed * 0x2545f491u + 0x6a09e667u;
        np.positionAmount = cx.curve(n.positionAmount, ModuleId::Noise, 4);
        np.rotationAmount = cx.curve(n.rotationAmount, ModuleId::Noise, 5);
        np.sizeAmount = cx.curve(n.sizeAmount, ModuleId::Noise, 6);
        np.affectsRotation = (!n.rotationAmount.isZero() && stored.rotation) ? 1 : 0;
        np.affectsSize = n.sizeAmount.isZero() ? 0 : 1;
        if (n.quality == NoiseQuality::Low)
            np.lowField = bakeLowField(cx, np.seed);
        prog.updateOps.push_back({OpCode::Noise, cx.pool.add(np)});
    }

    {
        IntegrateOp io;
        const LimitVelocityModule& l = d.limitVelocity;
        if (on(l.present, l.enabled)){
            io.integrate.limit = 1;
            io.integrate.limitSpeed = cx.curve(l.limit, ModuleId::LimitVelocity, 1);
            if (l.separateAxes){
                io.integrate.limitAxes = 1;
                io.integrate.limitWorld = l.space == ValueSpace::World ? 1 : 0;
                io.integrate.limitY = cx.curve(l.limitY, ModuleId::LimitVelocity, 3);
                io.integrate.limitZ = cx.curve(l.limitZ, ModuleId::LimitVelocity, 4);
            }
            io.integrate.dampen = l.dampen;
            io.integrate.logKeep = l.dampen < 1.0f ? std::log(1.0f - l.dampen) : 0.0f;
            const bool drag = !l.drag.isZero() || l.quadraticDrag > 0.0f;
            io.integrate.drag = drag ? 1 : 0;
            io.integrate.dragLinear = cx.curve(l.drag, ModuleId::LimitVelocity, 2);
            io.integrate.dragQuadratic = l.quadraticDrag;
            io.integrate.dragBySize = l.multiplyBySize ? 1 : 0;
            io.integrate.dragByVelocity = l.multiplyByVelocity ? 1 : 0;
            // the limit only removes an excess with a dampen; at 0 it does nothing
            if (l.dampen <= 0.0f) io.integrate.limit = 0;
        }
        io.hasAnimatedVelocity = speedModifier ? 1 : 0;
        io.animatedVelocity = animatedOffset;
        prog.updateOps.push_back({OpCode::Integrate, cx.pool.add(io)});
    }

    if (on(d.orbit.present, d.orbit.enabled)){
        OrbitParams op;
        op.world = d.orbit.space == ValueSpace::World ? 1 : 0;
        const float len = d.orbit.axis.length();
        op.axis[0] = d.orbit.axis.x / len; op.axis[1] = d.orbit.axis.y / len; op.axis[2] = d.orbit.axis.z / len;
        op.center[0] = d.orbit.center.x; op.center[1] = d.orbit.center.y; op.center[2] = d.orbit.center.z;
        op.angular = cx.curve(d.orbit.angularSpeed, ModuleId::Orbit, 1, kDegToRad);
        op.pull = cx.curve(d.orbit.radialPull, ModuleId::Orbit, 2);
        prog.updateOps.push_back({OpCode::Orbit, cx.pool.add(op)});
    }

    {
        StoredAppearanceParams sa;
        const RotationBySpeedModule& rbs = d.rotationBySpeed;
        const RotationOverLifetimeModule& rol = d.rotationOverLifetime;
        auto bySpeed = [&](const MinMaxCurve& c, uint32_t stream){
            MinMaxCurve speed = c;
            speed.driver = CurveDriver::Speed;
            speed.driverMin = rbs.speedRange.x;
            speed.driverMax = rbs.speedRange.y;
            return cx.curve(speed, ModuleId::RotationBySpeed, stream, kDegToRad, false, &c);
        };
        if (on(rbs.present, rbs.enabled) && stored.quat){
            // a mesh turns its quaternion: Z, and X / Y with separate axes
            sa.rotationBySpeed3D = 1;
            sa.rotationSpeed = bySpeed(rbs.angularVelocity, 1);
            sa.rotationSpeedX = rbs.separateAxes ? bySpeed(rbs.angularVelocityX, 2) : CurveParam();
            sa.rotationSpeedY = rbs.separateAxes ? bySpeed(rbs.angularVelocityY, 3) : CurveParam();
        }else if (on(rbs.present, rbs.enabled) && stored.rotation){
            sa.rotationBySpeed = 1;
            sa.rotationSpeed = bySpeed(rbs.angularVelocity, 1);
        }
        if (on(rol.present, rol.enabled) && stored.rotation && !stored.quat){
            sa.angularVelocity = 1;
            sa.angular = cx.curve(rol.angularVelocity, ModuleId::RotationOverLifetime, 1, kDegToRad);
        }
        sa.angularVelocity3D = stored.angularVelocity3D ? 1 : 0;
        if (sa.rotationBySpeed || sa.angularVelocity || sa.angularVelocity3D || sa.rotationBySpeed3D)
            prog.updateOps.push_back({OpCode::StoredAppearance, cx.pool.add(sa)});
    }

    if (collisionOn){
        const CollisionModule& c = d.collision;
        CollisionParams cp;
        switch (c.type){
            case CollisionType::Planes: cp.kind = CollisionKind::Planes; break;
            case CollisionType::World: cp.kind = CollisionKind::World; break;
            case CollisionType::Terrain: cp.kind = CollisionKind::Terrain; break;
            case CollisionType::World2D: cp.kind = CollisionKind::World2D; break;
        }
        cp.quality = c.quality;
        cp.kill = c.killOnCollide ? 1 : 0;
        // contacts are recorded for Lua (Send Collision Messages) and for Collision sub-emitters
        bool collisionSubEmitters = false;
        if (on(d.subEmittersModule.present, d.subEmittersModule.enabled)){
            for (const SubEmitterEntry& e : d.subEmitters)
                collisionSubEmitters = collisionSubEmitters || e.trigger == SubEmitterTrigger::Collision;
        }
        cp.events = (c.sendEvents || collisionSubEmitters) ? 1 : 0;
        cp.planeCount = (uint32_t)std::min(c.planes.size(), kMaxCollisionPlanes);
        for (uint32_t i = 0; i < cp.planeCount; i++){
            Vector3 n = c.planes[i].normal;
            n.normalize();
            cp.planes[i][0] = n.x; cp.planes[i][1] = n.y; cp.planes[i][2] = n.z;
            cp.planes[i][3] = -n.dotProduct(c.planes[i].point);
        }
        cp.dampen = cx.curve(c.dampen, ModuleId::Collision, 1);
        cp.bounce = cx.curve(c.bounce, ModuleId::Collision, 2);
        cp.lifetimeLoss = cx.curve(c.lifetimeLoss, ModuleId::Collision, 3);
        cp.minKillSpeed = c.minKillSpeed;
        cp.maxKillSpeed = c.maxKillSpeed;
        cp.radiusScale = c.radiusScale;
        cp.layerMask = c.collidesWith;
        cp.maxQueries = c.quality == CollisionQuality::High ? 0xffffffffu : (uint32_t)std::max(c.maxQueriesPerTick, 1);
        cp.planeCacheTicks = (uint32_t)std::max(c.planeCacheTicks, 1);
        cp.maxEvents = (uint32_t)std::max(c.maxCollisionEvents, 1);
        cp.settleSpeed = std::max(c.settleSpeed, 0.0f);
        prog.hasCollision = true;
        prog.collisionKind = cp.kind;
        prog.collisionEvents = c.sendEvents;
        prog.maxCollisionEvents = cp.maxEvents;
        prog.updateOps.push_back({OpCode::Collision, cx.pool.add(cp)});
    }

    prog.updateOps.push_back({OpCode::Kill, 0});
    prog.subEmitters = on(d.subEmittersModule.present, d.subEmittersModule.enabled) && !d.subEmitters.empty();

    // ---- render extraction
    {
        RenderParams& r = prog.render;
        const RendererModule& rm = d.renderer;
        r.mode = rm.enabled ? rm.renderMode : RenderMode::None;
        r.blend = rm.blend;
        r.sort = (rm.blend == ParticleBlend::Alpha || rm.blend == ParticleBlend::Premultiplied) ? rm.sortMode : SortMode::None;
        r.lighting = rm.lighting;
        const SizeOverLifetimeModule& sol = d.sizeOverLifetime;
        if (on(sol.present, sol.enabled)){
            r.sizeOverLifetime = 1;
            r.sizeOverLifetime3D = sol.separateAxes ? 1 : 0;
            r.sizeX = cx.curve(sol.size, ModuleId::SizeOverLifetime, 1);
            r.sizeY = cx.curve(sol.separateAxes ? sol.sizeY : sol.size, ModuleId::SizeOverLifetime, 2);
            r.sizeZ = cx.curve(sol.separateAxes ? sol.sizeZ : sol.size, ModuleId::SizeOverLifetime, 3);
        }
        const SizeBySpeedModule& sbs = d.sizeBySpeed;
        if (on(sbs.present, sbs.enabled)){
            auto bySpeed = [&](const MinMaxCurve& source, uint32_t stream){
                MinMaxCurve c = source;
                c.driver = CurveDriver::Speed;
                c.driverMin = sbs.speedRange.x;
                c.driverMax = sbs.speedRange.y;
                return cx.curve(c, ModuleId::SizeBySpeed, stream, 1.0f, false, &source);
            };
            r.sizeBySpeed = 1;
            r.sizeBySpeed3D = sbs.separateAxes ? 1 : 0;
            r.speedSizeX = bySpeed(sbs.size, 1);
            r.speedSizeY = bySpeed(sbs.separateAxes ? sbs.sizeY : sbs.size, 2);
            r.speedSizeZ = bySpeed(sbs.separateAxes ? sbs.sizeZ : sbs.size, 3);
        }
        const ColorOverLifetimeModule& col = d.colorOverLifetime;
        if (on(col.present, col.enabled) && !col.setMode){
            r.colorOverLifetime = 1;
            r.color = cx.gradient(col.color, ModuleId::ColorOverLifetime, 1);
        }else if (replaceColor){
            // the alpha still multiplies: the gradient with white colour keys
            cx.raw(&col.color);
            MinMaxGradient alpha = col.color;
            for (Gradient* g : {&alpha.gradient, &alpha.gradientMax}){
                for (GradientColorKey& key : g->colorKeys) key.color = Vector3(1.0f, 1.0f, 1.0f);
            }
            alpha.color = Vector4(1.0f, 1.0f, 1.0f, alpha.color.w);
            alpha.colorMax = Vector4(1.0f, 1.0f, 1.0f, alpha.colorMax.w);
            r.colorOverLifetime = 1;
            r.color = cx.gradient(alpha, ModuleId::ColorOverLifetime, 2);
        }
        const ColorBySpeedModule& cbs = d.colorBySpeed;
        if (on(cbs.present, cbs.enabled)){
            MinMaxGradient g = cbs.color;
            g.driver = CurveDriver::Speed;
            g.driverMin = cbs.speedRange.x;
            g.driverMax = cbs.speedRange.y;
            r.colorBySpeed = 1;
            r.speedColor = cx.gradient(g, ModuleId::ColorBySpeed, 1, false, &cbs.color);
        }
        r.storedRotation = stored.rotation ? 1 : 0;
        r.storedSize = stored.size ? 1 : 0;
        r.storedColor = stored.color ? 1 : 0;
        r.storedFrame = stored.frame ? 1 : 0;
        r.startRotationStored = (stored.startRotation || env.inheritsRotation) ? 1 : 0;
        r.size3D = (d.main.startSize3D || stored.size3D) ? 1 : 0;
        r.noiseSize = (noiseOn && !d.noise.sizeAmount.isZero()) ? 1 : 0;
        r.quat = stored.quat ? 1 : 0;
        r.mesh = (r.mode == RenderMode::Mesh || r.mode == RenderMode::Ring) ? 1 : 0;
        if (!stored.rotation && !r.startRotationStored)
            r.startRotation = cx.curve(d.main.startRotation, ModuleId::Main, 7, kDegToRad, true);
        r.flipRotation = d.main.flipRotation;
        const RotationOverLifetimeModule& rol = d.rotationOverLifetime;
        if (on(rol.present, rol.enabled) && !stored.rotation && !stored.quat){
            r.rotationOverLifetime = 1;
            const MinMaxCurve& w = rol.angularVelocity;
            r.rotationRate = cx.curve(w, ModuleId::RotationOverLifetime, 1, kDegToRad);
            if (r.rotationRate.kind == CurveParam::Lut || r.rotationRate.kind == CurveParam::LutRange){
                cx.raw(&w);
                // the LUTs hold the integral over normalized age instead (times the lifetime at render)
                r.rotationRate.lut0 = integralLut(cx, w, false, r.rotationRate.lutSize);
                if (r.rotationRate.kind == CurveParam::LutRange)
                    r.rotationRate.lut1 = integralLut(cx, w, true, r.rotationRate.lutSize);
            }
        }
        const TextureSheetAnimationModule& ts = d.textureSheetAnimation;
        if (on(ts.present, ts.enabled)){
            r.sheet = ts.mode == TextureSheetMode::Grid ? SheetKind::Grid : SheetKind::Sprites;
            r.sheetTime = ts.timeMode;
            r.tilesX = (uint32_t)std::max(ts.tilesX, 1);
            r.tilesY = (uint32_t)std::max(ts.tilesY, 1);
            r.sheetSingleRow = (ts.mode == TextureSheetMode::Grid && ts.animation == TextureSheetAnimation::SingleRow) ? 1 : 0;
            r.sheetRandomRow = (r.sheetSingleRow && ts.rowMode == TextureSheetRowMode::Random) ? 1 : 0;
            r.rowIndex = (uint32_t)std::max(ts.rowIndex, 0);
            if (r.sheet == SheetKind::Grid){
                r.frameCount = r.sheetSingleRow ? r.tilesX : r.tilesX * r.tilesY;
            }else{
                r.frameCount = (uint32_t)std::max<size_t>(ts.sprites.size(), 1);
                std::vector<float> rects;
                for (const SpriteRect& s : ts.sprites){
                    rects.push_back(s.x); rects.push_back(s.y); rects.push_back(s.width); rects.push_back(s.height);
                }
                r.sprites = cx.pool.addFloats(rects.data(), rects.size());
                r.spriteCount = (uint32_t)ts.sprites.size();
            }
            MinMaxCurve fot = ts.frameOverTime;
            if (ts.timeMode == TextureSheetTimeMode::Speed){
                fot.driver = CurveDriver::Speed;
                fot.driverMin = ts.speedRange.x;
                fot.driverMax = ts.speedRange.y;
            }
            r.frameOverTime = cx.curve(fot, ModuleId::TextureSheetAnimation, 1, 1.0f, false, &ts.frameOverTime);
            r.fps = ts.fps;
            r.startFrame = cx.curve(ts.startFrame, ModuleId::TextureSheetAnimation, 2);
            r.cycles = ts.cycles;
            r.flipU = std::clamp(ts.flipU, 0.0f, 1.0f);
            r.flipV = std::clamp(ts.flipV, 0.0f, 1.0f);
            r.frameBlending = ts.frameBlending ? 1 : 0;
            if (!ts.startFrameList.empty()){
                std::vector<uint32_t> frames;
                for (int f : ts.startFrameList) frames.push_back((uint32_t)std::max(f, 0));
                r.startFrameList = cx.pool.addUints(frames.data(), frames.size());
                r.startFrameListCount = (uint32_t)frames.size();
            }
        }else if (stored.frame){
            // Set over Age frames need a frame layout: the old points' sprite rects
            r.sheet = ts.sprites.empty() ? SheetKind::None : SheetKind::Sprites;
            if (!ts.sprites.empty()){
                std::vector<float> rects;
                for (const SpriteRect& s : ts.sprites){
                    rects.push_back(s.x); rects.push_back(s.y); rects.push_back(s.width); rects.push_back(s.height);
                }
                r.sprites = cx.pool.addFloats(rects.data(), rects.size());
                r.spriteCount = (uint32_t)ts.sprites.size();
                r.frameCount = r.spriteCount;
            }
        }
        const CustomDataModule& cd = d.customData;
        if (on(cd.present, cd.enabled)){
            r.customData = 1;
            for (int i = 0; i < 4; i++) r.custom[i] = cx.curve(cd.data[i], ModuleId::CustomData, (uint32_t)(1 + i));
        }
        r.stretched = r.mode == RenderMode::StretchedBillboard ? 1 : 0;
        r.cameraScale = rm.cameraScale;
        r.speedScale = rm.speedScale;
        r.lengthScale = rm.lengthScale;
        r.freezeInView = rm.freezeInView ? 1 : 0;
        r.verticalEmitter = rm.verticalLock == VerticalLock::Emitter ? 1 : 0;
        r.meshSelection = (uint8_t)rm.meshSelection;
        r.meshAlignment = rm.meshAlignment;
        // a ring is one mesh, made from the ring settings
        r.meshCount = r.mode == RenderMode::Ring ? 1u : (uint32_t)std::min(rm.meshes.size(), kMaxRendererMeshes);
        r.pivot[0] = rm.pivot.x; r.pivot[1] = rm.pivot.y; r.pivot[2] = rm.pivot.z;
        r.flip[0] = rm.flip.x; r.flip[1] = rm.flip.y; r.flip[2] = rm.flip.z;
        r.minScreenSize = rm.minScreenSize;
        r.maxScreenSize = rm.maxScreenSize;
        r.sortingFudge = rm.sortingFudge;
        r.emissive = rm.emissionIntensity;
        compilePixel(cx, r, col, cbs, replaceColor, stored.color);
    }

    // ---- trails
    {
        TrailParams& t = prog.trails;
        const TrailsModule& tm = d.trails;
        t.enabled = on(tm.present, tm.enabled);
        if (t.enabled){
            t.mode = tm.mode;
            t.ratio = tm.ratio;
            t.ribbonCount = (uint32_t)std::clamp(tm.ribbonCount, 1, 8);
            t.lifetime = cx.curve(tm.lifetime, ModuleId::Trails, 1, 1.0f, true);
            t.minVertexDistance = tm.minVertexDistance;
            t.maxPoints = (uint32_t)std::clamp<int>(tm.maxPoints, 2, (int)kMaxTrailPoints);
            t.worldSpace = tm.worldSpace;
            t.dieWithParticles = tm.dieWithParticles;
            t.width = cx.curve(tm.widthOverTrail, ModuleId::Trails, 2);
            t.colorOverLifetime = cx.gradient(tm.colorOverLifetime, ModuleId::Trails, 3);
            t.colorOverTrail = cx.gradient(tm.colorOverTrail, ModuleId::Trails, 4);
            t.inheritParticleColor = tm.inheritParticleColor;
            t.sizeAffectsWidth = tm.sizeAffectsWidth;
            t.sizeAffectsLifetime = tm.sizeAffectsLifetime;
            t.textureMode = tm.textureMode;
        }
    }

    sizeHints(d, prog);
    if (!effect.exposedSlots.empty())
        locateSlots(prog, cx.patterns, effect.exposedSlots);
    // ---- Analytic (S49): the slot count must fit; each system compiles its seed's program
    if (d.simulationMode == SimulationMode::Analytic){
        prog.analytic = true;
        prog.render.pixel.holdFrameRate = 0.0f;   // its time is quantised instead (AnalyticParticles::setTime)
        float lo, hi;
        d.main.startLifetime.range(lo, hi);
        prog.maxLifetime = std::max(hi, 1.0e-4f);
        AnalyticProgram probe;
        if (!AnalyticParticles::compile(d, prog, 1u, probe, effect.diagnostics)){
            effect.valid = false;
            effect.program = ParticleProgram();
        }
    }
}

// ---------------------------------------------------------------- program queries

bool ParticleCompiler::applyExposed(CompiledEffect& effect, size_t parameter, const Vector4& value){
    if (!effect.valid || parameter >= effect.exposedBindings.size() || parameter >= effect.document.exposed.size())
        return false;
    const ExposedBinding& binding = effect.exposedBindings[parameter];
    if (!binding.rewritable)
        return false;
    ParticleEffectDocument& d = effect.document;
    const ParticleModules::Resolved r = ParticleModules::resolve(d, d.exposed[parameter].path);
    if (!r.field || !r.storage)
        return false;
    const bool gradient = r.field->type == FieldType::MinMaxGradient;
    MinMaxCurve nextCurve;
    MinMaxGradient nextColor;
    if (gradient){
        MinMaxGradient& field = *static_cast<MinMaxGradient*>(r.storage);
        nextColor = field;
        if (!ParticleModules::writeExposedMember(nextColor, binding.member, value) || gradientShape(nextColor) != gradientShape(field))
            return false;
    }else{
        MinMaxCurve& field = *static_cast<MinMaxCurve*>(r.storage);
        nextCurve = field;
        if (!ParticleModules::writeExposedMember(nextCurve, binding.member, value.x) || curveShape(nextCurve) != curveShape(field))
            return false;
        // out of the field's range the document is invalid: the compile says so
        float lo, hi;
        nextCurve.range(lo, hi);
        if (lo < r.field->minValue || hi > r.field->maxValue)
            return false;
    }

    // every slot's new param first (and its LUTs where the old ones are), then the writes
    ParticleProgram& prog = effect.program;
    unsigned char* pool = reinterpret_cast<unsigned char*>(prog.pool.data());
    unsigned char* object = reinterpret_cast<unsigned char*>(&prog);
    unsigned char* bursts = reinterpret_cast<unsigned char*>(prog.emission.bursts.data());
    struct Pending {
        uint32_t slot = 0;
        CurveParam curve;
        GradientParam color;
        MinMaxCurve curveValue;
        MinMaxGradient colorValue;
        std::vector<std::pair<uint32_t, std::vector<float>>> luts;
    };
    std::vector<Pending> pending;
    for (uint32_t index : binding.slots){
        ExposedSlot& slot = effect.exposedSlots[index];
        const unsigned char* first = !slot.inPool.empty() ? pool + slot.inPool[0] : !slot.inProgram.empty() ? object + slot.inProgram[0]
                                   : !slot.inBursts.empty() ? bursts + slot.inBursts[0] : nullptr;
        Pending p;
        p.slot = index;
        auto overLut = [&](uint32_t kind, uint32_t lutSize, uint32_t lut0, uint32_t lut1, uint32_t lutKind, uint32_t rangeKind){
            return [&p, kind, lutSize, lut0, lut1, lutKind, rangeKind, gradient](const std::vector<float>& values, bool second, uint32_t& offset){
                const size_t perEntry = gradient ? 4u : 1u;
                if ((kind != lutKind && kind != rangeKind) || (second && kind != rangeKind) || values.size() != (size_t)lutSize * perEntry)
                    return false;
                offset = second ? lut1 : lut0;
                p.luts.push_back({offset, values});
                return true;
            };
        };
        if (gradient){
            p.colorValue = slot.color;
            if (!ParticleModules::writeExposedMember(p.colorValue, binding.member, value))
                return false;
            GradientParam old;
            if (first) std::memcpy(&old, first, sizeof(old));
            if (!buildGradient(p.colorValue, slot.salt, slot.startValue,
                               overLut(old.kind, old.lutSize, old.lut0, old.lut1, GradientParam::Lut, GradientParam::LutRange), p.color))
                return false;
            p.color.pad = slot.tag;
        }else{
            p.curveValue = slot.curve;
            if (!ParticleModules::writeExposedMember(p.curveValue, binding.member, value.x))
                return false;
            CurveParam old;
            if (first) std::memcpy(&old, first, sizeof(old));
            if (!buildCurve(p.curveValue, slot.salt, slot.scale, slot.startValue,
                            overLut(old.kind, old.lutSize, old.lut0, old.lut1, CurveParam::Lut, CurveParam::LutRange), p.curve))
                return false;
            p.curve.pad = slot.tag;
        }
        pending.push_back(std::move(p));
    }

    for (Pending& p : pending){
        ExposedSlot& slot = effect.exposedSlots[p.slot];
        const void* bytes = gradient ? (const void*)&p.color : (const void*)&p.curve;
        const size_t size = gradient ? sizeof(GradientParam) : sizeof(CurveParam);
        for (uint32_t at : slot.inPool) std::memcpy(pool + at, bytes, size);
        for (uint32_t at : slot.inProgram) std::memcpy(object + at, bytes, size);
        for (uint32_t at : slot.inBursts) std::memcpy(bursts + at, bytes, size);
        for (const auto& lut : p.luts) std::memcpy(pool + lut.first, lut.second.data(), lut.second.size() * sizeof(float));
        if (gradient) slot.color = std::move(p.colorValue);
        else slot.curve = std::move(p.curveValue);
    }
    if (gradient)
        *static_cast<MinMaxGradient*>(r.storage) = nextColor;
    else
        *static_cast<MinMaxCurve*>(r.storage) = nextCurve;
    sizeHints(d, prog);
    return true;
}

bool ParticleProgram::sameStructure(const ParticleProgram& other) const{
    if (channels != other.channels || maxParticles != other.maxParticles || space != other.space || tickRate != other.tickRate)
        return false;
    if (spawnOps.size() != other.spawnOps.size() || updateOps.size() != other.updateOps.size())
        return false;
    for (size_t i = 0; i < spawnOps.size(); i++){
        if (spawnOps[i].code != other.spawnOps[i].code) return false;
    }
    for (size_t i = 0; i < updateOps.size(); i++){
        if (updateOps[i].code != other.updateOps[i].code) return false;
    }
    if (trails.enabled != other.trails.enabled || (trails.enabled && (trails.mode != other.trails.mode || trails.maxPoints != other.trails.maxPoints ||
                                                                      trails.ratio != other.trails.ratio)))
        return false;
    return render.mode == other.render.mode && render.mesh == other.render.mesh && subEmitters == other.subEmitters && analytic == other.analytic;
}

uint32_t ParticleProgram::capacityHint() const{
    // storage: the peak with room to spare, never under a chunk
    const double peak = std::ceil(peakEstimate() * 1.1) + 8.0;
    return (uint32_t)std::min<double>(std::max(peak, 64.0), maxParticles);
}

double ParticleProgram::peakEstimate() const{
    // the continuous peak: the largest rate times the longest lifetime, plus every burst
    const unsigned char* base = reinterpret_cast<const unsigned char*>(pool.data());
    float lifetime = 1.0f;
    for (const Op& op : spawnOps){
        if (op.code == OpCode::SpawnStartValues){
            const StartParams& s = *reinterpret_cast<const StartParams*>(base + op.params);
            const CurveParam& c = s.lifetime;
            lifetime = std::max(c.c0, c.c1);
            if (c.kind == CurveParam::Lut || c.kind == CurveParam::LutRange){
                const float* t0 = reinterpret_cast<const float*>(base + c.lut0);
                for (uint32_t i = 0; i < c.lutSize; i++) lifetime = std::max(lifetime, t0[i]);
                if (c.kind == CurveParam::LutRange){
                    const float* t1 = reinterpret_cast<const float*>(base + c.lut1);
                    for (uint32_t i = 0; i < c.lutSize; i++) lifetime = std::max(lifetime, t1[i]);
                }
            }
        }
    }
    double peak = 0.0;
    if (emission.enabled){
        const CurveParam& r = emission.rate;
        float rate = std::max(r.c0, r.c1);
        if (r.kind == CurveParam::Lut || r.kind == CurveParam::LutRange){
            const float* t0 = reinterpret_cast<const float*>(base + r.lut0);
            for (uint32_t i = 0; i < r.lutSize; i++) rate = std::max(rate, t0[i]);
            if (r.kind == CurveParam::LutRange){
                const float* t1 = reinterpret_cast<const float*>(base + r.lut1);
                for (uint32_t i = 0; i < r.lutSize; i++) rate = std::max(rate, t1[i]);
            }
        }
        peak += (double)rate * lifetime;
        for (const BurstParam& b : emission.bursts){
            // the cycles that overlap within one lifetime
            const double cycles = b.cycles == 0 ? std::ceil(lifetime / b.interval) : std::min<double>(b.cycles, std::ceil(lifetime / b.interval));
            peak += (b.countMax + 1.0) * std::max(cycles, 1.0);
        }
        // a looping effect's bursts come back once per duration
        if (looping && duration > 0.0f && !emission.bursts.empty())
            peak *= std::max(1.0, std::ceil((double)lifetime / duration));
        // rate over distance is unbounded: the cap
        if (!emission.rateOverDistance.isZero())
            peak = maxParticles;
    }
    return std::min<double>(peak, maxParticles);
}

size_t ParticleProgram::bytesPerParticle() const{
    size_t n = 0;
    for (int c = 0; c < ChCount; c++){
        if (channels & channelBit((Channel)c))
            n += 4;
    }
    return n;
}

const char* particles::channelName(Channel channel){
    static const char* names[ChCount] = {
        "posX", "posY", "posZ", "prevX", "prevY", "prevZ", "velX", "velY", "velZ", "age", "invLife", "spawnIndex", "startSize",
        "startColor", "rotation", "prevRotation", "startRotation", "sizeX", "sizeY", "sizeZ", "prevSizeX", "startSizeY",
        "startSizeZ", "quatX", "quatY", "quatZ", "quatW", "angVelX", "angVelY", "angVelZ", "colorR", "colorG", "colorB",
        "colorA", "accelX", "accelY", "accelZ", "frame", "noiseSize", "planeNX", "planeNY", "planeNZ", "planeD", "planeTick",
        "trailSlot", "animVelX", "animVelY", "animVelZ", "parentId", "followQuatX", "followQuatY", "followQuatZ", "followQuatW",
        "followSize", "settled"};
    return channel < ChCount ? names[channel] : "?";
}
