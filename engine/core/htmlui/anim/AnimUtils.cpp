// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Port of Anime.js 4.5.0 (MIT) utils/stagger.js, utils/random.js, utils/number.js,
// utils/target.js (get, set, remove) and core/helpers.js (snap, lerp ...).

#include "AnimUtils.h"

#include "AnimAnimation.h"
#include "AnimCommon.h"
#include "AnimSpring.h"
#include "AnimTargets.h"
#include "AnimTimeline.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

using namespace jasperengine;

namespace {

    std::mt19937_64& generator(){
        static std::mt19937_64 g{std::random_device{}()};
        return g;
    }

    int32_t toInt32(double v){
        if (!std::isfinite(v))
            return 0;
        double m = std::fmod(std::trunc(v), 4294967296.0);
        if (m < 0) m += 4294967296.0;
        return (int32_t)(uint32_t)m;
    }

    int32_t imul(int32_t a, int32_t b){
        return (int32_t)((uint32_t)a * (uint32_t)b);
    }

    double pow10(int decimals){
        return std::pow(10.0, decimals);
    }

    EaseFn easeOf(const AnimParam& ease){
        if (ease.isEase()) return ease.asEase();
        if (ease.isString()) return AnimEasing::parse(ease.asString());
        if (ease.isSpring() && ease.asSpring()) return ease.asSpring()->getEaseFunction();
        return nullptr;
    }

}

double AnimUtils::randomUnit(){
    std::uniform_real_distribution<double> d(0.0, 1.0);
    return d(generator());
}

double AnimUtils::random(double min, double max, int decimals){
    const double m = pow10(decimals);
    return std::floor((randomUnit() * (max - min + (1 / m)) + min) * m) / m;
}

double AnimUtils::seededRandomNext(double& t, double min, double max, int decimals){
    t += 0x6D2B79F5;
    const int32_t a = toInt32(t);
    const int32_t x = a ^ (int32_t)((uint32_t)a >> 15);
    int32_t ti = imul(x, a | 1);
    const int32_t m = imul(ti ^ (int32_t)((uint32_t)ti >> 7), ti | 61);
    ti = ti ^ toInt32((double)ti + (double)m);
    t = ti;
    const uint32_t r = (uint32_t)(ti ^ (int32_t)((uint32_t)ti >> 14));
    const double p = pow10(decimals);
    return std::floor((((double)r / 4294967296.0) * (max - min + (1 / p)) + min) * p) / p;
}

AnimRandomFn AnimUtils::createSeededRandom(double seed, double defaultMin, double defaultMax, int defaultDecimals){
    auto state = std::make_shared<double>(seed);
    (void)defaultMin; (void)defaultMax; (void)defaultDecimals;
    return [state](double min, double max, int decimals){
        return AnimUtils::seededRandomNext(*state, min, max, decimals);
    };
}

void AnimUtils::shuffle(std::vector<AnimParam>& items, const AnimRandomFn& rng){
    size_t m = items.size();
    while (m){
        --m;
        const size_t i = (size_t)(rng ? rng(0, (double)m, 0) : random(0, (double)m, 0));
        std::swap(items[m], items[i]);
    }
}

void AnimUtils::shuffle(std::vector<double>& items, const AnimRandomFn& rng){
    size_t m = items.size();
    while (m){
        --m;
        const size_t i = (size_t)(rng ? rng(0, (double)m, 0) : random(0, (double)m, 0));
        std::swap(items[m], items[i]);
    }
}

double AnimUtils::clamp(double v, double min, double max){ return AnimMath::clamp(v, min, max); }
double AnimUtils::round(double v, int decimals){ return AnimMath::round(v, decimals); }

double AnimUtils::snap(double v, double increment){
    return increment ? AnimMath::jsRound(v / increment) * increment : v;
}

double AnimUtils::snap(double v, const std::vector<double>& points){
    if (points.empty())
        return v;
    double closest = points[0];
    for (size_t i = 1; i < points.size(); i++){
        if (std::fabs(points[i] - v) < std::fabs(closest - v))
            closest = points[i];
    }
    return closest;
}

double AnimUtils::wrap(double v, double min, double max){
    return std::fmod(std::fmod(v - min, max - min) + (max - min), max - min) + min;
}

double AnimUtils::mapRange(double value, double inLow, double inHigh, double outLow, double outHigh){
    return outLow + ((value - inLow) / (inHigh - inLow)) * (outHigh - outLow);
}

double AnimUtils::lerp(double start, double end, double factor){ return AnimMath::lerp(start, end, factor); }

double AnimUtils::damp(double start, double end, double deltaTime, double factor){
    return !factor ? start : factor == 1 ? end : AnimMath::lerp(start, end, 1 - std::exp(-factor * deltaTime * .1));
}

double AnimUtils::degToRad(double degrees){ return degrees * 3.14159265358979323846 / 180; }
double AnimUtils::radToDeg(double radians){ return radians * 180 / 3.14159265358979323846; }

std::string AnimUtils::roundPad(double v, int decimals){
    // Number#toFixed
    if (decimals < 0) decimals = 0;
    if (decimals > 100) decimals = 100;
    std::vector<char> buffer(400);
    std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, v);
    return buffer.data();
}

std::string AnimUtils::padStart(const std::string& v, int totalLength, const std::string& padString){
    if ((int)v.size() >= totalLength || padString.empty())
        return v;
    std::string pad;
    while ((int)(pad.size() + v.size()) < totalLength)
        pad += padString;
    pad.resize(totalLength - v.size());
    return pad + v;
}

std::string AnimUtils::padEnd(const std::string& v, int totalLength, const std::string& padString){
    if ((int)v.size() >= totalLength || padString.empty())
        return v;
    std::string out = v;
    while ((int)out.size() < totalLength)
        out += padString;
    out.resize(totalLength);
    return out;
}

AnimRawValue AnimUtils::get(const AnimTargetList& targets, const std::string& property, const std::string& unit, bool numberOnly){
    if (targets.empty() || !targets[0])
        return AnimRawValue();
    AnimTarget& target = *targets[0];
    const AnimTweenType tweenType = target.getTweenType(property);
    const std::string name = target.sanitizeProperty(property, tweenType);
    AnimRawValue original = target.getValue(name, tweenType, nullptr);
    if (unit.empty() && !numberOnly)
        return original;
    AnimDecomposed decomposed = AnimValue::decompose(original);
    if (decomposed.t == AnimValueType::NUMBER || decomposed.t == AnimValueType::UNIT){
        if (numberOnly)
            return AnimRawValue(decomposed.n);
        target.convertUnit(decomposed, unit, false);
        return AnimRawValue(AnimMath::toString(AnimMath::round(decomposed.n, AnimConst::precision)) + decomposed.u);
    }
    return AnimRawValue();
}

std::shared_ptr<AnimAnimation> AnimUtils::set(const AnimTargetList& targets, const AnimParam& params){
    return AnimAnimation::set(targets, params);
}

void AnimUtils::remove(const AnimTargetList& targets, AnimTimer* renderable, const std::string& property){
    AnimComposer::removeTargets(targets, renderable, property);
}

// ---------------------------------------------------------------------------------------------
// stagger

namespace {

    struct StaggerState {
        std::vector<double> values;
        double maxValue = 0;
        bool hasCachedOffset = false;
        double cachedOffset = 0;
        std::vector<double> jitterSamples;
        double fromIndex = 0;
    };

}

AnimParam AnimUtils::stagger(const AnimParam& val, const AnimParam& params){
    const AnimParam& from = params.get("from");
    const bool reversed = params.get("reversed").isTruthy();
    const AnimParam& easeParam = params.get("ease");
    const EaseFn staggerEase = easeOf(easeParam);
    const AnimParam& grid = params.get("grid");
    const bool autoGrid = grid.isBool() && grid.asBool();
    const std::string axis = params.get("axis").isString() ? params.get("axis").asString() : std::string();
    const AnimParam& customTotal = params.get("total");
    const bool fromFirst = from.isNil() || (from.isNumber() && from.asNumber() == 0) || (from.isString() && from.asString() == "first");
    const bool fromCenter = from.isString() && from.asString() == "center";
    const bool fromLast = from.isString() && from.asString() == "last";
    const bool fromRandom = from.isString() && from.asString() == "random";
    const bool fromArr = from.isArray();
    const bool isRange = val.isArray();
    const AnimParam& useProp = params.get("use");
    auto parseNumber = [](const AnimParam& p){ return p.isString() ? AnimMath::parseFloat(p.asString()) : p.toNumber(); };
    const double val1 = isRange ? parseNumber(val.at(0)) : parseNumber(val);
    const double val2 = isRange ? parseNumber(val.at(1)) : 0;
    double unitNumber = 0;
    std::string unit;
    const AnimParam& unitSource = isRange ? val.at(1) : val;
    const bool hasUnit = unitSource.isString() && AnimValue::splitUnit(unitSource.asString(), unitNumber, unit);
    const AnimParam& startParam = params.get("start");
    const double start = startParam.isTruthy() ? startParam.toNumber() : (isRange ? val1 : 0);
    const AnimParam& seed = params.get("seed");
    const bool hasSeed = !seed.isNil() && !(seed.isBool() && !seed.asBool());
    const AnimRandomFn rng = hasSeed ? createSeededRandom(seed.isBool() ? 0 : seed.toNumber())
                                     : AnimRandomFn([](double min, double max, int decimals){ return AnimUtils::random(min, max, decimals); });
    const AnimParam& jitter = params.get("jitter");
    const bool hasJitter = !jitter.isNil();
    const bool jitterIsArr = jitter.isArray();
    const double jitterStart = jitterIsArr ? jitter.at(0).toNumber() : (jitter.isNumber() ? jitter.asNumber() : 0);
    const double jitterEnd = jitterIsArr ? jitter.at(1).toNumber() : (jitter.isNumber() ? jitter.asNumber() : 0);
    const AnimParam& modifierParam = params.get("modifier");
    const EaseFn modifier = modifierParam.isEase() ? modifierParam.asEase() : nullptr;

    auto state = std::make_shared<StaggerState>();
    state->fromIndex = fromFirst ? 0 : from.isNumber() ? from.asNumber() : 0;

    AnimParam fromCopy = from;
    AnimParam gridCopy = grid;
    AnimParam useCopy = useProp;
    AnimParam startCopy = startParam;

    return AnimParam::function([=](const AnimFunctionArgs& args) -> AnimParam {
        StaggerState& s = *state;
        const int i = args.index;
        const int total = customTotal.isNil() ? args.total : (int)customTotal.toNumber();
        AnimParam customIndex;
        if (useCopy.isFunction()){
            AnimFunctionArgs useArgs = args;
            useArgs.total = total;
            customIndex = useCopy.asFunction()(useArgs);
        } else if (useCopy.isString() && args.target){
            AnimTweenType type = args.target->getTweenType(useCopy.asString());
            customIndex = AnimParam(args.target->getValue(args.target->sanitizeProperty(useCopy.asString(), type), type, nullptr));
        }
        double customIdx = i;
        if (customIndex.isNumber() || (customIndex.isString() && !std::isnan(customIndex.toNumber())))
            customIdx = customIndex.toNumber();
        // the natural index when the value lands outside [0, total)
        const int staggerIndex = customIdx >= 0 && customIdx < total ? (int)customIdx : i;
        if (fromCenter) s.fromIndex = (total - 1) / 2.0;
        if (fromLast) s.fromIndex = total - 1;
        if (s.values.empty()){
            if (autoGrid){
                bool hasPositions = true;
                bool has3D = false;
                double minX = AnimConst::infinity, minY = AnimConst::infinity, minZ = AnimConst::infinity;
                double maxX = -AnimConst::infinity, maxY = -AnimConst::infinity, maxZ = -AnimConst::infinity;
                std::vector<double> px, py, pz;
                for (int index = 0; index < total; index++){
                    double x = 0, y = 0, z = 0;
                    bool targetHas3D = false;
                    const AnimTargetPtr el = args.targets && index < (int)args.targets->size() ? (*args.targets)[index] : nullptr;
                    if (!el || !el->getStaggerPosition(x, y, z, targetHas3D)){
                        hasPositions = false;
                        break;
                    }
                    if (targetHas3D) has3D = true;
                    px.push_back(x); py.push_back(y); pz.push_back(z);
                    minX = std::min(minX, x); minY = std::min(minY, y); minZ = std::min(minZ, z);
                    maxX = std::max(maxX, x); maxY = std::max(maxY, y); maxZ = std::max(maxZ, z);
                }
                if (hasPositions && total > 0){
                    double fX = px[0], fY = py[0], fZ = pz[0];
                    if (fromArr){
                        fX = minX + fromCopy.at(0).toNumber() * (maxX - minX);
                        fY = minY + fromCopy.at(1).toNumber() * (maxY - minY);
                        fZ = has3D ? minZ + (fromCopy.size() >= 3 ? fromCopy.at(2).toNumber() : 0.5) * (maxZ - minZ) : 0;
                    } else if (fromCenter){
                        fX = (minX + maxX) / 2; fY = (minY + maxY) / 2; fZ = (minZ + maxZ) / 2;
                    } else if (fromLast){
                        fX = px[total - 1]; fY = py[total - 1]; fZ = pz[total - 1];
                    } else if (fromCopy.isNumber()){
                        const int f = (int)fromCopy.asNumber();
                        if (f >= 0 && f < total){ fX = px[f]; fY = py[f]; fZ = pz[f]; }
                    }
                    for (int index = 0; index < total; index++){
                        const double dx = fX - px[index], dy = fY - py[index], dz = fZ - pz[index];
                        double value = std::sqrt(dx * dx + dy * dy + (has3D ? dz * dz : 0));
                        if (axis == "x") value = -dx;
                        if (axis == "y") value = -dy;
                        if (axis == "z") value = -dz;
                        s.values.push_back(value);
                    }
                    double minDist = AnimConst::infinity;
                    for (double v : s.values){
                        const double a = std::fabs(v);
                        if (a > 0 && a < minDist) minDist = a;
                    }
                    if (minDist > 0 && minDist < AnimConst::infinity){
                        for (double& v : s.values) v = v / minDist;
                    }
                } else {
                    s.values.clear();
                    for (int index = 0; index < total; index++)
                        s.values.push_back(std::fabs(s.fromIndex - index));
                }
            } else {
                for (int index = 0; index < total; index++){
                    if (!gridCopy.isArray()){
                        s.values.push_back(std::fabs(s.fromIndex - index));
                    } else {
                        const size_t dims = gridCopy.size();
                        const double g0 = gridCopy.at(0).toNumber(), g1 = gridCopy.at(1).toNumber();
                        const double g2 = dims == 3 ? gridCopy.at(2).toNumber() : 0;
                        const double wh = g0 * g1;
                        double fromX, fromY, fromZ;
                        if (fromArr){
                            fromX = fromCopy.at(0).toNumber() * (g0 - 1);
                            fromY = fromCopy.at(1).toNumber() * (g1 - 1);
                            fromZ = dims == 3 ? (fromCopy.size() >= 3 ? fromCopy.at(2).toNumber() : 0.5) * (g2 - 1) : 0;
                        } else if (fromCenter){
                            fromX = (g0 - 1) / 2;
                            fromY = (g1 - 1) / 2;
                            fromZ = dims == 3 ? (g2 - 1) / 2 : 0;
                        } else {
                            fromX = std::fmod(s.fromIndex, g0);
                            fromY = std::fmod(std::floor(s.fromIndex / g0), g1);
                            fromZ = dims == 3 ? std::floor(s.fromIndex / wh) : 0;
                        }
                        const double toX = std::fmod((double)index, g0);
                        const double toY = std::fmod(std::floor(index / g0), g1);
                        const double toZ = dims == 3 ? std::floor(index / wh) : 0;
                        const double dx = fromX - toX, dy = fromY - toY, dz = fromZ - toZ;
                        double value = std::sqrt(dx * dx + dy * dy + (dims == 3 ? dz * dz : 0));
                        if (axis == "x") value = -dx;
                        if (axis == "y") value = -dy;
                        if (axis == "z") value = -dz;
                        s.values.push_back(value);
                    }
                }
            }
            s.maxValue = s.values.empty() ? 0 : s.values[0];
            for (int k = 1; k < (int)s.values.size(); k++){
                if (s.values[k] > s.maxValue) s.maxValue = s.values[k];
            }
            if (staggerEase || reversed){
                for (double& v : s.values){
                    if (staggerEase) v = staggerEase(v / s.maxValue) * s.maxValue;
                    if (reversed) v = !axis.empty() ? -v : std::fabs(s.maxValue - v);
                }
            }
            if (hasJitter){
                s.jitterSamples.resize(total);
                for (int k = 0; k < total; k++) s.jitterSamples[k] = rng(-1, 1, 4);
            }
            if (fromRandom)
                shuffle(s.values, rng);
        }
        const double spacing = isRange ? (val2 - val1) / s.maxValue : val1;
        if (!s.hasCachedOffset){
            s.hasCachedOffset = true;
            if (args.timeline){
                s.cachedOffset = args.timeline->parsePosition(startCopy.isNil() ? AnimParam(args.timeline->iterationDuration)
                                                                                   : (startCopy.isTruthy() ? startCopy : AnimParam(start)));
            } else {
                s.cachedOffset = start;
            }
        }
        const double staggerValue = staggerIndex < (int)s.values.size() ? s.values[staggerIndex] : 0;
        double offset = spacing * AnimMath::round(staggerValue, 2);
        if (std::isnan(offset) || !offset) offset = 0;
        double output = s.cachedOffset + offset;
        if (hasJitter && staggerIndex < (int)s.jitterSamples.size()){
            const double progress = s.maxValue ? staggerValue / s.maxValue : 0;
            const double mag = jitterStart + (jitterEnd - jitterStart) * progress;
            output = output + s.jitterSamples[staggerIndex] * mag;
        }
        if (modifier) output = modifier(output);
        if (hasUnit)
            return AnimParam(AnimMath::toString(output) + unit);
        return AnimParam(output);
    });
}
