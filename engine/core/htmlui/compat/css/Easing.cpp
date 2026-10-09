// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/Easing.h"

#include "htmlui/compat/css/Units.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::htmlui::compat;

namespace {

    constexpr double kPi = 3.14159265358979323846;

    // Source/Core/Tween.cpp, in double precision
    double square(double t){ return t * t; }
    double back(double t){ return t * t * (2.70158 * t - 1.70158); }
    double bounce(double t){
        if (t > 1.0 - 1.0 / 2.75)
            return 1.0 - 7.5625 * square(1.0 - t);
        if (t > 1.0 - 2.0 / 2.75)
            return 1.0 - (7.5625 * square(1.0 - t - 1.5 / 2.75) + 0.75);
        if (t > 1.0 - 2.5 / 2.75)
            return 1.0 - (7.5625 * square(1.0 - t - 2.25 / 2.75) + 0.9375);
        return 1.0 - (7.5625 * square(1.0 - t - 2.625 / 2.75) + 0.984375);
    }
    double circular(double t){ return 1.0 - std::sqrt(std::max(0.0, 1.0 - t * t)); }
    double cubic(double t){ return t * t * t; }
    double elastic(double t){
        if (t == 0 || t == 1)
            return t;
        return -std::exp(7.24 * (t - 1.0)) * std::sin((t - 1.1) * 2.0 * kPi / 0.4);
    }
    double exponential(double t){
        if (t == 0 || t == 1)
            return t;
        return std::exp(7.24 * (t - 1.0));
    }
    double linear(double t){ return t; }
    double quadratic(double t){ return t * t; }
    double quartic(double t){ return t * t * t * t; }
    double quintic(double t){ return t * t * t * t * t; }
    double sine(double t){ return 1.0 - std::cos(t * kPi * 0.5); }

    struct TweenType {
        const char* name;
        double (*function)(double);
    };

    const TweenType kTypes[] = {
        {"back", back}, {"bounce", bounce}, {"circular", circular}, {"cubic", cubic}, {"elastic", elastic},
        {"exponential", exponential}, {"linear", linear}, {"quadratic", quadratic}, {"quartic", quartic},
        {"quintic", quintic}, {"sine", sine},
    };

    const char* const kNames[] = {
        "back-in", "back-out", "back-in-out", "bounce-in", "bounce-out", "bounce-in-out",
        "circular-in", "circular-out", "circular-in-out", "cubic-in", "cubic-out", "cubic-in-out",
        "elastic-in", "elastic-out", "elastic-in-out", "exponential-in", "exponential-out", "exponential-in-out",
        "linear-in-out", "linear-in", "linear-out", "quadratic-in", "quadratic-out", "quadratic-in-out",
        "quartic-in", "quartic-out", "quartic-in-out", "quintic-in", "quintic-out", "quintic-in-out",
        "sine-in", "sine-out", "sine-in-out",
    };

    constexpr int kSamples = 101;

}

Span<const char* const> Easing::tweenNames(){
    return Span<const char* const>(kNames);
}

double Easing::tween(std::string_view name, double t){
    for (const TweenType& type : kTypes){
        const std::string prefix = std::string(type.name) + "-";
        if (name.substr(0, prefix.size()) != prefix)
            continue;
        const std::string_view mode = name.substr(prefix.size());
        if (mode == "in")
            return type.function(t);
        if (mode == "out")
            return 1.0 - type.function(1.0 - t);
        if (mode == "in-out"){
            if (t < 0.5)
                return type.function(2.0 * t) * 0.5;
            return 0.5 + (1.0 - type.function(1.0 - (2.0 * t - 1.0))) * 0.5;
        }
    }
    return t;
}

double Easing::cubicBezier(double x1, double y1, double x2, double y2, double x){
    if (x <= 0)
        return 0;
    if (x >= 1)
        return 1;
    auto bez = [](double a, double b, double s){
        // B(s) for control values 0, a, b, 1
        const double u = 1.0 - s;
        return 3.0 * u * u * s * a + 3.0 * u * s * s * b + s * s * s;
    };
    auto derivative = [](double a, double b, double s){
        const double u = 1.0 - s;
        return 3.0 * u * u * a + 6.0 * u * s * (b - a) + 3.0 * s * s * (1.0 - b);
    };
    // Newton, then bisection as a fallback
    double s = x;
    for (int i = 0; i < 8; i++){
        const double error = bez(x1, x2, s) - x;
        if (std::fabs(error) < 1e-9)
            return bez(y1, y2, s);
        const double d = derivative(x1, x2, s);
        if (std::fabs(d) < 1e-9)
            break;
        s -= error / d;
        s = std::max(0.0, std::min(1.0, s));
    }
    double lo = 0, hi = 1;
    s = x;
    for (int i = 0; i < 64; i++){
        const double value = bez(x1, x2, s);
        if (std::fabs(value - x) < 1e-12)
            break;
        if (value < x)
            lo = s;
        else
            hi = s;
        s = (lo + hi) / 2.0;
    }
    return bez(y1, y2, s);
}

double Easing::linearFunction(const std::vector<std::pair<double, double>>& points, double t){
    if (points.empty())
        return t;
    if (t <= points.front().first)
        return points.front().second;
    for (size_t i = 1; i < points.size(); i++){
        if (t <= points[i].first){
            const double span = points[i].first - points[i - 1].first;
            if (span <= 0)
                return points[i].second;
            const double f = (t - points[i - 1].first) / span;
            return points[i - 1].second + (points[i].second - points[i - 1].second) * f;
        }
    }
    return points.back().second;
}

double Easing::maxError(const std::function<double(double)>& curve, std::string_view tweenName){
    double worst = 0;
    for (int i = 0; i < kSamples; i++){
        const double t = (double)i / (kSamples - 1);
        worst = std::max(worst, std::fabs(curve(t) - tween(tweenName, t)));
    }
    return worst;
}

TweenChoice Easing::nearestTween(const std::function<double(double)>& curve){
    TweenChoice best;
    best.error = 1e300;
    for (const char* name : kNames){
        const double error = maxError(curve, name);
        // strictly better: the earlier name wins a tie (linear-in-out before linear-in)
        if (error < best.error - 1e-12){
            best.error = error;
            best.name = name;
        }
    }
    return best;
}

std::optional<std::string> Easing::keywordTween(std::string_view keyword){
    const std::string k = lowerAscii(keyword);
    if (k == "linear")
        return std::string("linear-in-out");
    if (k == "ease")
        return std::string("quadratic-out");
    if (k == "ease-in")
        return std::string("sine-in");
    if (k == "ease-out")
        return std::string("sine-out");
    if (k == "ease-in-out")
        return std::string("quadratic-in-out");
    return std::nullopt;
}

std::optional<std::array<double, 4>> Easing::keywordBezier(std::string_view keyword){
    const std::string k = lowerAscii(keyword);
    if (k == "linear")
        return std::array<double, 4>{0.0, 0.0, 1.0, 1.0};
    if (k == "ease")
        return std::array<double, 4>{0.25, 0.1, 0.25, 1.0};
    if (k == "ease-in")
        return std::array<double, 4>{0.42, 0.0, 1.0, 1.0};
    if (k == "ease-out")
        return std::array<double, 4>{0.0, 0.0, 0.58, 1.0};
    if (k == "ease-in-out")
        return std::array<double, 4>{0.42, 0.0, 0.58, 1.0};
    return std::nullopt;
}
