// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "noise/NoiseSwitch.h"

#if JASPERENGINE_NOISE_ENABLED

#include "noise/NoiseDetMath.h"
#include "noise/NoisePlan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// The analytic ranges (T2 R19): each must contain every value its op outputs, so results are widened
// past float rounding. An unbounded range is what turns the output's sanitiser on (R6).

namespace jasperengine { namespace noise {

    namespace {

        constexpr float kInf = std::numeric_limits<float>::infinity();

        Interval widen(float lo, float hi) {
            if (std::isnan(lo) || std::isnan(hi)) return Interval{-kInf, kInf};
            return Interval{lo - std::fabs(lo) * 3e-7f - 1e-30f, hi + std::fabs(hi) * 3e-7f + 1e-30f};
        }

        Interval corners(float a, float b, float c, float d) {
            return widen(std::min({a, b, c, d}), std::max({a, b, c, d}));
        }

        bool containsZero(const Interval& r) { return r.min <= 0.0f && r.max >= 0.0f; }

        Interval mulRange(const Interval& a, const Interval& b) {
            return corners(a.min * b.min, a.min * b.max, a.max * b.min, a.max * b.max);
        }

        Interval addRange(const Interval& a, const Interval& b) { return widen(a.min + b.min, a.max + b.max); }

        float powCorner(float base, float exponent) {
            return detmath::DetMath<Lane1>::pow(base, exponent);
        }

        Interval powRange(const Interval& a, const Interval& b) {
            if (a.max <= 0.0f) return Interval{0.0f, 1.0f};
            const float lo = std::max(a.min, 0.0f);
            float low = powCorner(a.max, b.min), high = low;
            for (float base : {lo, a.max}) {
                for (float exponent : {b.min, b.max}) {
                    const float v = powCorner(base, exponent);
                    low = std::min(low, v);
                    high = std::max(high, v);
                }
            }
            // x^0 is 1, at any base
            if (containsZero(b)) { low = std::min(low, 1.0f); high = std::max(high, 1.0f); }
            // A negative or zero base gives 0, and a base just above 0 with a negative exponent is as large as a float gets
            if (a.min <= 0.0f) {
                low = std::min(low, 0.0f);
                if (b.min < 0.0f) high = kInf;
            }
            if (!std::isfinite(high)) high = kInf;
            return Interval{low - std::fabs(low) * 2e-3f - 1e-6f, high + std::fabs(high) * 2e-3f + 1e-6f};
        }

        Interval mathRange(MathKind kind, const Interval& a, const Interval& b) {
            switch (kind) {
                case MathKind::Add: return addRange(a, b);
                case MathKind::Subtract: return widen(a.min - b.max, a.max - b.min);
                case MathKind::Multiply: return mulRange(a, b);
                case MathKind::Divide:
                    // Past a b of 0 the result has no bound, and 0 itself is what b = 0 gives
                    if (containsZero(b)) return Interval{-kInf, kInf};
                    return corners(a.min / b.min, a.min / b.max, a.max / b.min, a.max / b.max);
                case MathKind::Min: return widen(std::min(a.min, b.min), std::min(a.max, b.max));
                case MathKind::Max: return widen(std::max(a.min, b.min), std::max(a.max, b.max));
                case MathKind::Power: return powRange(a, b);
                case MathKind::Modulo: return widen(std::min(0.0f, b.min), std::max(0.0f, b.max));
                case MathKind::Abs: {
                    const float top = std::max(std::fabs(a.min), std::fabs(a.max));
                    return widen(containsZero(a) ? 0.0f : std::min(std::fabs(a.min), std::fabs(a.max)), top);
                }
                case MathKind::Negate: return widen(-a.max, -a.min);
                case MathKind::OneMinus: return widen(1.0f - a.max, 1.0f - a.min);
                case MathKind::Sqrt: return widen(std::sqrt(std::max(a.min, 0.0f)), std::sqrt(std::max(a.max, 0.0f)));
                case MathKind::Floor: return widen(std::floor(a.min), std::floor(a.max));
                case MathKind::Frac: return Interval{0.0f, 1.0f};
                default: return Interval{-1.0f, 1.0f};
            }
        }

        Interval unionRange(const Interval& a, const Interval& b) { return widen(std::min(a.min, b.min), std::max(a.max, b.max)); }

        float magnitude(const Interval& r) { return std::max(std::fabs(r.min), std::fabs(r.max)); }

    }

    void rangeOfOp(const PlanOp& op, const PlanData& plan, std::vector<Interval>& ranges) {
        const auto in = [&](int k) -> Interval { return op.inValue[k] >= 0 ? ranges[static_cast<size_t>(op.inValue[k])] : Interval{0.0f, 0.0f}; };
        const auto set = [&](int k, Interval value) { if (op.outValue[k] >= 0) ranges[static_cast<size_t>(op.outValue[k])] = value; };
        switch (op.kind) {
            case OpKind::Fill: set(0, Interval{op.f[0], op.f[0]}); break;
            case OpKind::Variable: set(0, Interval{plan.variables[static_cast<size_t>(op.i[0])].min, plan.variables[static_cast<size_t>(op.i[0])].max}); break;
            case OpKind::PositionField: set(0, in(0)); break;
            case OpKind::Noise:
            case OpKind::Fbm: set(0, basisRange(op.fractal.basis)); break;
            case OpKind::Ridged: set(0, Interval{0.0f, 1.0f}); break;
            case OpKind::DomainWarp: {
                const float strength = op.inValue[3] >= 0 ? magnitude(in(3)) : 1.0f;
                const float reach = magnitude(basisRange(op.fractal.basis)) * std::fabs(op.f[0]) * strength;
                set(0, widen(in(0).min - reach, in(0).max + reach));
                set(1, widen(in(1).min - reach, in(1).max + reach));
                set(2, widen(in(2).min - reach, in(2).max + reach));
                break;
            }
            case OpKind::DomainTransform: {
                const Interval sx = mulRange(in(0), Interval{op.f[0], op.f[0]});
                const Interval sy = mulRange(in(1), Interval{op.f[1], op.f[1]});
                const Interval sz = mulRange(in(2), Interval{op.f[2], op.f[2]});
                const Interval c{op.f[6], op.f[6]}, s{op.f[7], op.f[7]};
                set(0, addRange(addRange(mulRange(sx, c), mulRange(sz, s)), Interval{op.f[3], op.f[3]}));
                set(1, addRange(sy, Interval{op.f[4], op.f[4]}));
                set(2, addRange(addRange(mulRange(sz, c), mulRange(sx, Interval{-op.f[7], -op.f[7]})), Interval{op.f[5], op.f[5]}));
                break;
            }
            case OpKind::ScaleOffset: set(0, addRange(mulRange(in(0), Interval{op.f[0], op.f[0]}), Interval{op.f[1], op.f[1]})); break;
            case OpKind::MathOp: set(0, mathRange(op.math, in(0), in(1))); break;
            case OpKind::Clamp: {
                const auto clampTo = [&](float v) { return std::min(std::max(v, op.f[0]), op.f[1]); };
                set(0, Interval{clampTo(in(0).min), clampTo(in(0).max)});
                break;
            }
            case OpKind::Remap: {
                if (op.i[0] != 0) {
                    set(0, widen(std::min(op.f[2], op.f[5]), std::max(op.f[2], op.f[5])));
                } else {
                    const Interval t = mulRange(addRange(in(0), Interval{-op.f[0], -op.f[0]}), Interval{op.f[1], op.f[1]});
                    set(0, addRange(mulRange(t, Interval{op.f[3], op.f[3]}), Interval{op.f[2], op.f[2]}));
                }
                break;
            }
            case OpKind::Curve: {
                const std::vector<float>& lut = plan.luts[static_cast<size_t>(op.lut)];
                const auto bounds = std::minmax_element(lut.begin(), lut.end());
                set(0, widen(*bounds.first, *bounds.second));
                break;
            }
            case OpKind::Terrace: {
                const float n = op.f[0];
                set(0, widen(std::floor(in(0).min * n) / n, (std::floor(in(0).max * n) + 1.0f) / n));
                break;
            }
            case OpKind::Select:
            case OpKind::Blend: set(0, unionRange(in(0), in(1))); break;
            case OpKind::HeightMask: {
                const Interval r = in(0);
                if (r.min >= op.f[0] && r.max <= op.f[1]) set(0, Interval{1.0f, 1.0f});
                else if (r.max < op.f[0] - op.f[2] || r.min > op.f[1] + op.f[2]) set(0, Interval{0.0f, 0.0f});
                else set(0, Interval{0.0f, 1.0f});
                break;
            }
            case OpKind::TerrainHeight:
            case OpKind::SlopeMask: set(0, Interval{0.0f, 1.0f}); break;
            case OpKind::Hydraulic:
                set(0, Interval{op.f[8], op.f[9]});
                set(1, Interval{0.0f, 1.0f});
                set(2, Interval{0.0f, 1.0f});
                break;
            case OpKind::Thermal: set(0, widen(in(0).min, in(0).max)); break;
            case OpKind::Output:
                if (op.f[0] > 0.0f) {
                    const float scale = 1.0f / op.f[1];
                    set(0, mulRange(in(0), Interval{scale, scale}));
                } else {
                    set(0, in(0));
                }
                break;
        }
    }

}}

#endif // JASPERENGINE_NOISE_ENABLED
