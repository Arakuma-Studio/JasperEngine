// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cmath>
#include <limits>

namespace jasperengine::portablemath {

    // Elementary functions that give the same bits with every C library, for results the engine
    // promises are the same on every platform (the pixel rotation's goldens): MSVC's CRT and glibc
    // round sin, cos, log10, hypot and the others differently in the last bit, on 0.1% to 20% of
    // the inputs, and a value that decides at a step (a pixel at a boundary) then reads differently. Only +, -, *, /, sqrt,
    // floor, frexp, ldexp and comparisons run here, which IEEE 754 defines exactly, in a fixed
    // order. A source using these must be built without contraction of a * b + c into a fused
    // multiply-add (JASPERENGINE_NO_FMA_FLAGS), or a compiler could fuse the polynomials on one
    // target only. Accurate to a few units in the last place, not correctly rounded.

    namespace detail {

        constexpr double PI = 3.14159265358979323846;
        constexpr double HALF_PI = 1.57079632679489661923;
        constexpr double TWO_OVER_PI = 0.636619772367581343076;
        // pi / 2 in three parts of 33 bits: k * part is exact for |k| < 2^20, so the reduction
        // loses nothing for the angles these callers use
        constexpr double HALF_PI_1 = 1.57079632673412561417e+00;
        constexpr double HALF_PI_2 = 6.07710050630396597660e-11;
        constexpr double HALF_PI_3 = 2.02226624871116645580e-21;
        // ln 2 in two parts: k * LN2_HI is exact for the exponents of a double
        constexpr double LN2_HI = 6.93147180369123816490e-01;
        constexpr double LN2_LO = 1.90821492927058770002e-10;
        constexpr double INV_LN2 = 1.44269504088896338700e+00;
        constexpr double LN10 = 2.30258509299404568402;
        constexpr double SQRT_HALF = 0.707106781186547524401;

        // sin and cos on [-pi/4, pi/4]: their Taylor series to the 19th and 20th power, past the
        // last bit there, in Horner's order
        inline double sinKernel(double x) {
            const double z = x * x;
            double p = 1.0 / 121645100408832000.0;   // 1 / 19!
            p = 1.0 / 355687428096000.0 - z * p;     // 1 / 17!
            p = 1.0 / 1307674368000.0 - z * p;       // 1 / 15!
            p = 1.0 / 6227020800.0 - z * p;          // 1 / 13!
            p = 1.0 / 39916800.0 - z * p;            // 1 / 11!
            p = 1.0 / 362880.0 - z * p;              // 1 / 9!
            p = 1.0 / 5040.0 - z * p;                // 1 / 7!
            p = 1.0 / 120.0 - z * p;                 // 1 / 5!
            p = 1.0 / 6.0 - z * p;                   // 1 / 3!
            return x - x * z * p;
        }

        inline double cosKernel(double x) {
            const double z = x * x;
            double p = 1.0 / 2432902008176640000.0;  // 1 / 20!
            p = 1.0 / 6402373705728000.0 - z * p;    // 1 / 18!
            p = 1.0 / 20922789888000.0 - z * p;      // 1 / 16!
            p = 1.0 / 87178291200.0 - z * p;         // 1 / 14!
            p = 1.0 / 479001600.0 - z * p;           // 1 / 12!
            p = 1.0 / 3628800.0 - z * p;             // 1 / 10!
            p = 1.0 / 40320.0 - z * p;               // 1 / 8!
            p = 1.0 / 720.0 - z * p;                 // 1 / 6!
            p = 1.0 / 24.0 - z * p;                  // 1 / 4!
            return (1.0 - 0.5 * z) + z * z * p;
        }

        // x as k quarter turns plus a remainder in [-pi/4, pi/4]; quadrant is k mod 4
        inline double reduceQuarters(double x, int& quadrant) {
            const double k = std::floor(x * TWO_OVER_PI + 0.5);
            const double r = ((x - k * HALF_PI_1) - k * HALF_PI_2) - k * HALF_PI_3;
            const double turns = k - 4.0 * std::floor(k * 0.25);
            quadrant = static_cast<int>(turns);
            return r;
        }

        // atan on [0, 2 - sqrt(3)] (tan 15 degrees): its series
        inline double atanKernel(double t) {
            const double z = t * t;
            double p = 1.0 / 29.0;
            for (int n = 27; n >= 3; n -= 2) p = 1.0 / static_cast<double>(n) - z * p;
            return t - t * z * p;
        }

    }

    inline void sincos(double x, double& s, double& c) {
        if (!std::isfinite(x)) {
            s = c = std::numeric_limits<double>::quiet_NaN();
            return;
        }
        int quadrant = 0;
        const double r = detail::reduceQuarters(x, quadrant);
        const double sr = detail::sinKernel(r), cr = detail::cosKernel(r);
        switch (quadrant) {
            case 0: s = sr; c = cr; break;
            case 1: s = cr; c = -sr; break;
            case 2: s = -sr; c = -cr; break;
            default: s = -cr; c = sr; break;
        }
    }

    inline double sin(double x) {
        double s, c;
        sincos(x, s, c);
        return s;
    }

    inline double cos(double x) {
        double s, c;
        sincos(x, s, c);
        return c;
    }

    inline double tan(double x) {
        double s, c;
        sincos(x, s, c);
        return s / c;
    }

    inline double atan2(double y, double x) {
        if (std::isnan(x) || std::isnan(y)) return std::numeric_limits<double>::quiet_NaN();
        if (y == 0.0) return std::signbit(x) ? std::copysign(detail::PI, y) : y;
        if (x == 0.0) return std::copysign(detail::HALF_PI, y);
        const double ax = std::fabs(x), ay = std::fabs(y);
        // the angle of (ax, ay) in [0, pi/2]: an argument at most 1, then at most tan 15 degrees
        const bool swapped = ay > ax;
        double t = swapped ? ax / ay : ay / ax;
        double base = 0.0;
        constexpr double TAN_15 = 0.267949192431122706473;   // 2 - sqrt(3)
        constexpr double SQRT_3 = 1.73205080756887729353;
        if (t > TAN_15) {
            // atan(t) = pi/6 + atan((sqrt(3) t - 1) / (sqrt(3) + t))
            t = (SQRT_3 * t - 1.0) / (SQRT_3 + t);
            base = detail::PI / 6.0;
        }
        double angle = base + detail::atanKernel(t);
        if (swapped) angle = detail::HALF_PI - angle;
        if (x < 0.0) angle = detail::PI - angle;
        return std::copysign(angle, y);
    }

    inline double exp(double x) {
        if (std::isnan(x)) return x;
        if (x > 709.8) return std::numeric_limits<double>::infinity();
        if (x < -745.2) return 0.0;
        const double k = std::floor(x * detail::INV_LN2 + 0.5);
        const double r = (x - k * detail::LN2_HI) - k * detail::LN2_LO;
        // e^r for |r| <= ln 2 / 2: its series to r^14, 1 + r (1 + r/2 (1 + r/3 (...)))
        double p = 1.0;
        for (int n = 14; n >= 1; n--) p = 1.0 + r * p / static_cast<double>(n);
        return std::ldexp(p, static_cast<int>(k));
    }

    inline double log(double x) {
        if (std::isnan(x) || x < 0.0) return std::numeric_limits<double>::quiet_NaN();
        if (x == 0.0) return -std::numeric_limits<double>::infinity();
        if (std::isinf(x)) return x;
        int e = 0;
        double m = std::frexp(x, &e);   // [0.5, 1)
        if (m < detail::SQRT_HALF) {
            m *= 2.0;
            e--;
        }
        // log m = 2 atanh f, f = (m - 1) / (m + 1), |f| <= 0.172: its series to f^25
        const double f = (m - 1.0) / (m + 1.0);
        const double s = f * f;
        double p = 1.0 / 25.0;
        for (int n = 23; n >= 1; n -= 2) p = 1.0 / static_cast<double>(n) + s * p;
        const double k = static_cast<double>(e);
        return k * detail::LN2_HI + (k * detail::LN2_LO + 2.0 * f * p);
    }

    inline double log10(double x) {
        return log(x) / detail::LN10;
    }

    inline double log2(double x) {
        return log(x) * detail::INV_LN2;
    }

    // x^y for x > 0, and for x <= 0 where the result is defined (0 for x = 0 and y > 0, a whole y
    // for x < 0); pow(x, 0) is 1. A whole y up to 64 multiplies, so 2^-5 and 10^3 are exact.
    inline double pow(double x, double y) {
        if (y == 0.0 || x == 1.0) return 1.0;
        if (std::isnan(x) || std::isnan(y)) return std::numeric_limits<double>::quiet_NaN();
        if (x == 0.0) return y > 0.0 ? 0.0 : std::numeric_limits<double>::infinity();
        if (std::floor(y) == y && std::fabs(y) <= 64.0) {
            int n = static_cast<int>(std::fabs(y));
            double base = x, result = 1.0;
            while (n > 0) {
                if (n & 1) result *= base;
                base *= base;
                n >>= 1;
            }
            return y < 0.0 ? 1.0 / result : result;
        }
        if (x < 0.0) {
            if (std::floor(y) != y) return std::numeric_limits<double>::quiet_NaN();
            const double magnitude = exp(y * log(-x));
            return std::fmod(y, 2.0) != 0.0 ? -magnitude : magnitude;
        }
        return exp(y * log(x));
    }

    // without the scaling a C library's hypot does against overflow: for magnitudes far from it
    inline double hypot(double x, double y) {
        return std::sqrt(x * x + y * y);
    }

}
