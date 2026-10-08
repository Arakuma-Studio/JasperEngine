// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMUTILS_H
#define ANIMUTILS_H

// anime.stagger and anime.utils, ported from Anime.js 4.5.0 (utils/stagger.js,
// utils/random.js, utils/number.js, utils/target.js, core/helpers.js).

#include "Export.h"
#include "AnimParams.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class AnimAnimation;

    // a random generator: (min, max, decimals) -> value in [min, max]
    using AnimRandomFn = std::function<double(double, double, int)>;

    // a stagger function as a script value (anime.stagger): usable as a property value,
    // a delay/duration or a timeline position; calling it from Lua evaluates it
    class JASPERENGINE_API AnimStaggerFunction {
    public:
        AnimStaggerFunction() = default;
        explicit AnimStaggerFunction(AnimParam::Function fn): fn(std::move(fn)) {}
        const AnimParam::Function& getFunction() const { return fn; }

    private:
        AnimParam::Function fn;
    };

    class JASPERENGINE_API AnimUtils {
    public:
        // stagger(value | [from, to], {start, from = "first"|"center"|"last"|"random"|index|[x, y(, z)],
        //   reversed, ease, grid = true|[cols, rows(, depth)], axis = "x"|"y"|"z", total, use, modifier,
        //   jitter = n|[a, b], seed = true|n}): a function value
        static AnimParam stagger(const AnimParam& value, const AnimParam& params = AnimParam::object());

        // random(min = 0, max = 1, decimals = 0), both ends included
        static double random(double min = 0, double max = 1, int decimals = 0);
        // a uniform number in [0, 1) (Math.random)
        static double randomUnit();
        // createSeededRandom(seed): a deterministic generator (mulberry32, as Anime)
        static AnimRandomFn createSeededRandom(double seed, double defaultMin = 0, double defaultMax = 1, int defaultDecimals = 0);
        // one step of a seeded generator whose state is `state` (the seed at first)
        static double seededRandomNext(double& state, double min, double max, int decimals);
        // Fisher-Yates in place
        static void shuffle(std::vector<AnimParam>& items, const AnimRandomFn& rng = nullptr);
        static void shuffle(std::vector<double>& items, const AnimRandomFn& rng = nullptr);

        static double clamp(double v, double min, double max);
        static double round(double v, int decimals);
        static double snap(double v, double increment);
        static double snap(double v, const std::vector<double>& points);
        static double wrap(double v, double min, double max);
        static double mapRange(double value, double inLow, double inHigh, double outLow, double outHigh);
        static double lerp(double start, double end, double factor);
        static double damp(double start, double end, double deltaTime, double factor);
        static double degToRad(double degrees);
        static double radToDeg(double radians);
        static std::string roundPad(double v, int decimals);
        static std::string padStart(const std::string& v, int totalLength, const std::string& padString);
        static std::string padEnd(const std::string& v, int totalLength, const std::string& padString);

        // utils.get(target, property[, unit]): the current value; unit "" keeps it as read,
        // a unit converts it ("12px"), numberOnly gives the number
        static AnimRawValue get(const AnimTargetList& targets, const std::string& property, const std::string& unit = "", bool numberOnly = false);
        // utils.set(targets, props)
        static std::shared_ptr<AnimAnimation> set(const AnimTargetList& targets, const AnimParam& params);
        // utils.remove(targets[, renderable][, property]); renderable null: every running one
        static void remove(const AnimTargetList& targets, AnimTimer* renderable = nullptr, const std::string& property = "");
    };

}

#endif //ANIMUTILS_H
