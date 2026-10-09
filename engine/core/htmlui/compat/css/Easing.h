// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_EASING_H
#define COMPAT_CSS_EASING_H

// CSS easing functions as RmlUi tweens (phase-2 §3.9.7 "Transitions"): RmlUi 6.3 knows only
// its own tween keywords (quadratic-out, ...), so until the fork has TimingFunctions every CSS
// easing becomes the tween closest to it: the smallest maximum error over 101 samples of
// t in [0, 1].

#include "Export.h"
#include "htmlui/compat/CompatTypes.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::htmlui::compat {

    struct TweenChoice {
        std::string name;    // RmlUi keyword: "quadratic-out"
        double error = 0;    // maximum absolute difference over the samples
    };

    class JASPERENGINE_API Easing {
    public:
        // RmlUi's tween keywords (Tween.cpp), linear-in-out first among the linear ones
        static Span<const char* const> tweenNames();
        // RmlUi's tween curve for a keyword (the formulas of Source/Core/Tween.cpp)
        static double tween(std::string_view name, double t);

        // CSS cubic-bezier(x1, y1, x2, y2) at t (x1, x2 in [0, 1])
        static double cubicBezier(double x1, double y1, double x2, double y2, double t);
        // CSS linear(): points (input, output), inputs in [0, 1] and increasing
        static double linearFunction(const std::vector<std::pair<double, double>>& points, double t);

        // The RmlUi tween with the smallest maximum error against curve over 101 samples
        static TweenChoice nearestTween(const std::function<double(double)>& curve);
        static double maxError(const std::function<double(double)>& curve, std::string_view tweenName);

        // The fixed table of the CSS keywords: linear -> linear-in-out, ease -> quadratic-out,
        // ease-in -> sine-in, ease-out -> sine-out, ease-in-out -> quadratic-in-out; nullopt
        // for other words
        static std::optional<std::string> keywordTween(std::string_view keyword);
        // the cubic-bezier control points of a CSS easing keyword
        static std::optional<std::array<double, 4>> keywordBezier(std::string_view keyword);
    };

}

#endif //COMPAT_CSS_EASING_H
