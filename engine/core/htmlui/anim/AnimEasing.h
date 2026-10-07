// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMEASING_H
#define ANIMEASING_H

// Easing functions of the anime API, ported from Anime.js 4.5.0 (MIT, easings/): the named
// eases (`out(2)`, `inOutQuad`, `outElastic(1, .6)` ...), cubicBezier, steps, linear and
// irregular. A function maps the time fraction (0..1) to the eased fraction.

#include "Export.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine {

    using EaseFn = std::function<double(double)>;

    // a point of linear(): a value, optionally with its position ("0.5 25%")
    struct AnimLinearPoint {
        double value = 0.0;
        bool hasPercent = false;  // false: evenly spaced
        double percent = 0.0;
    };

    class JASPERENGINE_API AnimEasing {
    public:
        // t -> t
        static EaseFn none();

        // Parses an easing name as Anime's string form, plus the function forms Anime 4.5
        // removed from strings: "linear", "none", "in", "out(3)", "inOutQuad", "outElastic(1, .6)",
        // "inBack(2)", "cubicBezier(x1, y1, x2, y2)", "steps(n[, fromStart])",
        // "linear(p0, p1 25%, ...)", "irregular(length, randomness)" and "spring(mass, stiffness,
        // damping, velocity)" (the spring's ease only; animations take its duration through
        // AnimParam::spring). Unknown names log a warning and give linear; ok (when given) is
        // false then.
        static EaseFn parse(const std::string& name, bool* ok = nullptr);
        // true when parse() knows the name (no warning)
        static bool isValid(const std::string& name);

        // eases.<name>(params...): "inQuad", "out" (power, default 1.68), "outBack" (overshoot,
        // default 1.7), "inElastic" (amplitude 1, period .3) ... Missing parameters take the
        // defaults; parameters of the fixed eases (inQuad ...) are ignored. Empty for unknown names
        static EaseFn get(const std::string& name, const std::vector<double>& params = {});
        // every name of Anime's `eases` table, in its order
        static const std::vector<std::string>& getNames();
        // whether eases.<name> takes parameters (the powers, Back and Elastic)
        static bool hasParameters(const std::string& name);

        static EaseFn powerIn(double power = 1.68);
        // gre/bezier-easing solver; equal control points give linear
        static EaseFn cubicBezier(double x1 = 0.5, double y1 = 0.0, double x2 = 0.5, double y2 = 1.0);
        // CSS steps() with jump-end (or jump-start when fromStart)
        static EaseFn steps(double steps = 10, bool fromStart = false);
        // piecewise linear through the points; no point: linear
        static EaseFn linear(const std::vector<AnimLinearPoint>& points);
        // random steps (std::rand-free: an internal generator)
        static EaseFn irregular(int length = 10, double randomness = 1.0);
    };

    // An easing as a script value (anime.eases.*, anime.cubicBezier() ...). Calling it from
    // Lua evaluates the curve; for the parametric eases (in, out, inOut, outIn, *Back,
    // *Elastic) it makes a new ease with the given parameters, as Anime's eases table does.
    class JASPERENGINE_API AnimEaseFunction {
    public:
        AnimEaseFunction() = default;
        explicit AnimEaseFunction(EaseFn fn, std::string name = "", bool parametric = false);

        double evaluate(double t) const;
        // the curve (parametric eases: with their default parameters)
        EaseFn getFunction() const;
        const std::string& getName() const { return name; }
        bool isParametric() const { return parametric; }
        AnimEaseFunction withParameters(const std::vector<double>& params) const;

    private:
        EaseFn fn;
        std::string name;
        bool parametric = false;
    };

    // spec name (phase-1 §3.11): AnimEasing::parse
    JASPERENGINE_API EaseFn parseEase(const std::string& name);

}

#endif //ANIMEASING_H
