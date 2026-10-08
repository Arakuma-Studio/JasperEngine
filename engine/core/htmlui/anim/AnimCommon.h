// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMCOMMON_H
#define ANIMCOMMON_H

// Constants and the JavaScript number semantics the anime port relies on (Anime.js 4.5.0
// core/consts.js, core/helpers.js): Math.round, unary plus, parseFloat and Number#toString.

#include "Export.h"

#include <cmath>
#include <limits>
#include <string>

namespace jasperengine {

    namespace AnimConst {
        constexpr double K = 1e3;
        constexpr double minValue = 1e-11;
        constexpr double maxValue = 1e12;
        constexpr double maxFps = 240;
        constexpr double tickThreshold = 200;   // ms: a bigger jump renders every tween (seek-like)
        constexpr int precision = 4;            // decimals of the in-between values written as text
        constexpr double infinity = std::numeric_limits<double>::infinity();
    }

    class JASPERENGINE_API AnimMath {
    public:
        static double clamp(double v, double min, double max){ return v < min ? min : v > max ? max : v; }
        // JavaScript Math.round: halves go up (-1.5 -> -1)
        static double jsRound(double v);
        // Anime's round(v, decimals): decimals < 0 keeps v, 0 rounds to an integer
        static double round(double v, int decimals);
        static double lerp(double start, double end, double factor){
            return factor == 1 ? end : factor == 0 ? start : start + (end - start) * factor;
        }
        static double clampInfinity(double v){
            return v == AnimConst::infinity ? AnimConst::maxValue : v == -AnimConst::infinity ? -AnimConst::maxValue : v;
        }
        static double normalizeTime(double v){
            return v <= AnimConst::minValue ? AnimConst::minValue : clampInfinity(round(v, 11));
        }
        // JavaScript `~~v` for the iteration counts (truncation toward zero)
        static double truncate(double v){ return std::isfinite(v) ? std::trunc(v) : 0.0; }

        // JavaScript unary plus on a string: whitespace trimmed, "" -> 0, invalid -> NaN
        static double toNumber(const std::string& text);
        // JavaScript parseFloat: the longest leading number, NaN when none
        static double parseFloat(const std::string& text);
        // JavaScript Number#toString (shortest round trip, exponent outside [1e-6, 1e21))
        static std::string toString(double v);
    };

}

#endif //ANIMCOMMON_H
