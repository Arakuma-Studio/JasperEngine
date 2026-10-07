// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Port of Anime.js 4.5.0 easings (MIT): eases/parser.js (eases adapted from Robert Penner's,
// (c) 2001 Robert Penner), cubic-bezier (after gre/bezier-easing, (c) 2014 Gaëtan Renaudeau),
// steps, linear and irregular.

#include "AnimEasing.h"

#include "AnimCommon.h"
#include "AnimSpring.h"
#include "AnimUtils.h"
#include "Log.h"

#include <cmath>
#include <map>

using namespace jasperengine;

namespace {

    const double PI = 3.14159265358979323846;
    const double halfPI = PI / 2;
    const double doublePI = PI * 2;

    using EaseType = std::function<EaseFn(EaseFn)>;

    EaseFn easeTypeIn(EaseFn easeIn){ return easeIn; }
    EaseFn easeTypeOut(EaseFn easeIn){ return [easeIn](double t){ return 1 - easeIn(1 - t); }; }
    EaseFn easeTypeInOut(EaseFn easeIn){
        return [easeIn](double t){ return t < .5 ? easeIn(t * 2) / 2 : 1 - easeIn(t * -2 + 2) / 2; };
    }
    EaseFn easeTypeOutIn(EaseFn easeIn){
        return [easeIn](double t){ return t < .5 ? (1 - easeIn(1 - t * 2)) / 2 : (easeIn(t * 2 - 1) + 1) / 2; };
    }

    struct EaseInFunction {
        // fixed curve, or a factory taking the parameters (missing ones: NaN = default)
        EaseFn fixed;
        std::function<EaseFn(const std::vector<double>&)> factory;
    };

    double param(const std::vector<double>& params, size_t index, double fallback){
        return index < params.size() && !std::isnan(params[index]) ? params[index] : fallback;
    }

    EaseFn bounceIn(){
        return [](double t){
            double pow2;
            double b = 4;
            while (t < ((pow2 = std::pow(2, --b)) - 1) / 11);
            return 1 / std::pow(4, 3 - b) - 7.5625 * std::pow((pow2 * 3 - 2) / 22 - t, 2);
        };
    }

    EaseFn backIn(double overshoot){
        return [overshoot](double t){ return (overshoot + 1) * t * t * t - overshoot * t * t; };
    }

    EaseFn elasticIn(double amplitude, double period){
        const double a = AnimMath::clamp(amplitude, 1, 10);
        const double p = AnimMath::clamp(period, AnimConst::minValue, 2);
        const double s = (p / doublePI) * std::asin(1 / a);
        const double e = doublePI / p;
        return [a, s, e](double t){
            return t == 0 || t == 1 ? t : -a * std::pow(2, -10 * (1 - t)) * std::sin(((1 - t) - s) * e);
        };
    }

    // Anime's easeInFunctions, in its order
    const std::vector<std::pair<std::string, EaseInFunction>>& easeInFunctions(){
        static const std::vector<std::pair<std::string, EaseInFunction>> functions = {
            {"", {nullptr, [](const std::vector<double>& p){ return AnimEasing::powerIn(param(p, 0, 1.68)); }}},
            {"Quad", {AnimEasing::powerIn(2), nullptr}},
            {"Cubic", {AnimEasing::powerIn(3), nullptr}},
            {"Quart", {AnimEasing::powerIn(4), nullptr}},
            {"Quint", {AnimEasing::powerIn(5), nullptr}},
            {"Sine", {[](double t){ return 1 - std::cos(t * halfPI); }, nullptr}},
            {"Circ", {[](double t){ return 1 - std::sqrt(1 - t * t); }, nullptr}},
            {"Expo", {[](double t){ return t ? std::pow(2, 10 * t - 10) : 0; }, nullptr}},
            {"Bounce", {bounceIn(), nullptr}},
            {"Back", {nullptr, [](const std::vector<double>& p){ return backIn(param(p, 0, 1.7)); }}},
            {"Elastic", {nullptr, [](const std::vector<double>& p){ return elasticIn(param(p, 0, 1), param(p, 1, .3)); }}},
        };
        return functions;
    }

    const std::vector<std::pair<std::string, EaseType>>& easeTypes(){
        static const std::vector<std::pair<std::string, EaseType>> types = {
            {"in", easeTypeIn}, {"out", easeTypeOut}, {"inOut", easeTypeInOut}, {"outIn", easeTypeOutIn},
        };
        return types;
    }

    struct EaseEntry {
        const EaseInFunction* easeIn = nullptr;
        const EaseType* type = nullptr;
    };

    const std::map<std::string, EaseEntry>& easeTable(){
        static const std::map<std::string, EaseEntry> table = [](){
            std::map<std::string, EaseEntry> t;
            for (const auto& type : easeTypes()){
                for (const auto& easeIn : easeInFunctions()){
                    t[type.first + easeIn.first] = EaseEntry{&easeIn.second, &type.second};
                }
            }
            return t;
        }();
        return table;
    }

    std::string trim(const std::string& text){
        size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return "";
        size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    // "name(a, b)" -> name + raw arguments; false when the parentheses do not close
    bool splitCall(const std::string& text, std::string& name, std::vector<std::string>& args, bool& hasParentheses){
        size_t open = text.find('(');
        hasParentheses = open != std::string::npos;
        if (!hasParentheses){
            name = trim(text);
            return true;
        }
        if (trim(text).back() != ')')
            return false;
        name = trim(text.substr(0, open));
        std::string inner = text.substr(open + 1, text.rfind(')') - open - 1);
        args.clear();
        if (trim(inner).empty())
            return true;
        size_t start = 0;
        while (true){
            size_t comma = inner.find(',', start);
            args.push_back(trim(inner.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return true;
    }

    std::vector<double> numbers(const std::vector<std::string>& args){
        std::vector<double> values;
        for (const std::string& arg : args)
            values.push_back(arg.empty() ? std::nan("") : AnimMath::toNumber(arg));
        return values;
    }

    EaseFn parseInternal(const std::string& text, bool warn, bool* ok){
        if (ok) *ok = true;
        std::string name;
        std::vector<std::string> args;
        bool hasParentheses = false;
        if (splitCall(text, name, args, hasParentheses)){
            if (name == "linear" || name == "none"){
                if (name == "none" || args.empty())
                    return AnimEasing::none();
                // linear(p0, "p1 25%", ...)
                std::vector<AnimLinearPoint> points;
                for (const std::string& arg : args){
                    AnimLinearPoint point;
                    size_t space = arg.find(' ');
                    point.value = AnimMath::parseFloat(arg);
                    if (space != std::string::npos){
                        point.hasPercent = true;
                        point.percent = AnimMath::parseFloat(trim(arg.substr(space + 1)));
                    }
                    points.push_back(point);
                }
                return AnimEasing::linear(points);
            }
            if (name == "cubicBezier"){
                std::vector<double> p = numbers(args);
                return AnimEasing::cubicBezier(param(p, 0, .5), param(p, 1, 0), param(p, 2, .5), param(p, 3, 1));
            }
            if (name == "steps"){
                double count = args.size() > 0 && !args[0].empty() ? AnimMath::toNumber(args[0]) : 10;
                bool fromStart = args.size() > 1 && (args[1] == "true" || args[1] == "1" || args[1] == "start" || args[1] == "jump-start");
                return AnimEasing::steps(count, fromStart);
            }
            if (name == "irregular"){
                std::vector<double> p = numbers(args);
                return AnimEasing::irregular((int)param(p, 0, 10), param(p, 1, 1));
            }
            if (name == "spring"){
                std::vector<double> p = numbers(args);
                AnimSpringParams params;
                if (p.size() > 0 && !std::isnan(p[0])) params.mass = p[0];
                if (p.size() > 1 && !std::isnan(p[1])) params.stiffness = p[1];
                if (p.size() > 2 && !std::isnan(p[2])) params.damping = p[2];
                if (p.size() > 3 && !std::isnan(p[3])) params.velocity = p[3];
                return std::make_shared<AnimSpring>(params)->getEaseFunction();
            }
            EaseFn ease = AnimEasing::get(name, numbers(args));
            if (ease)
                return ease;
        }
        if (ok) *ok = false;
        if (warn){
            Log::warn("[HTML UI] anime: unknown easing '%s', using 'linear' (valid: linear, in, out, inOut, outIn, "
                      "in/out/inOut/outIn + Quad Cubic Quart Quint Sine Expo Circ Bounce Back Elastic, cubicBezier(), "
                      "steps(), linear(), irregular(), spring())", text.c_str());
        }
        return AnimEasing::none();
    }

}

EaseFn AnimEasing::none(){
    return [](double t){ return t; };
}

EaseFn AnimEasing::parse(const std::string& name, bool* ok){
    return parseInternal(name, true, ok);
}

bool AnimEasing::isValid(const std::string& name){
    bool ok = false;
    parseInternal(name, false, &ok);
    return ok;
}

EaseFn AnimEasing::get(const std::string& name, const std::vector<double>& params){
    if (name == "linear" || name == "none")
        return none();
    const auto& table = easeTable();
    auto it = table.find(name);
    if (it == table.end())
        return nullptr;
    const EaseInFunction& easeIn = *it->second.easeIn;
    const EaseType& type = *it->second.type;
    return type(easeIn.factory ? easeIn.factory(params) : easeIn.fixed);
}

const std::vector<std::string>& AnimEasing::getNames(){
    static const std::vector<std::string> names = [](){
        std::vector<std::string> n = {"linear", "none"};
        for (const auto& type : easeTypes()){
            for (const auto& easeIn : easeInFunctions()){
                n.push_back(type.first + easeIn.first);
            }
        }
        return n;
    }();
    return names;
}

bool AnimEasing::hasParameters(const std::string& name){
    const auto& table = easeTable();
    auto it = table.find(name);
    return it != table.end() && it->second.easeIn->factory != nullptr;
}

EaseFn AnimEasing::powerIn(double power){
    return [power](double t){ return std::pow(t, power); };
}

namespace {

    double calcBezier(double aT, double aA1, double aA2){
        return (((1 - 3 * aA2 + 3 * aA1) * aT + (3 * aA2 - 6 * aA1)) * aT + (3 * aA1)) * aT;
    }

    double binarySubdivide(double aX, double mX1, double mX2){
        double aA = 0, aB = 1, currentX, currentT;
        int i = 0;
        do {
            currentT = aA + (aB - aA) / 2;
            currentX = calcBezier(currentT, mX1, mX2) - aX;
            if (currentX > 0){
                aB = currentT;
            } else {
                aA = currentT;
            }
        } while (std::fabs(currentX) > .0000001 && ++i < 100);
        return currentT;
    }

}

EaseFn AnimEasing::cubicBezier(double x1, double y1, double x2, double y2){
    if (x1 == y1 && x2 == y2)
        return none();
    return [x1, y1, x2, y2](double t){
        return t == 0 || t == 1 ? t : calcBezier(binarySubdivide(t, x1, x2), y1, y2);
    };
}

EaseFn AnimEasing::steps(double count, bool fromStart){
    return [count, fromStart](double t){
        double v = AnimMath::clamp(t, 0, 1) * count;
        return (fromStart ? std::ceil(v) : std::floor(v)) * (1 / count);
    };
}

EaseFn AnimEasing::linear(const std::vector<AnimLinearPoint>& points){
    if (points.empty())
        return none();
    const size_t total = points.size() - 1;
    std::vector<double> xPoints = {0};
    std::vector<double> yPoints = {points[0].value};
    for (size_t i = 1; i < total; i++){
        xPoints.push_back(points[i].hasPercent ? points[i].percent / 100 : (double)i / (double)total);
        yPoints.push_back(points[i].value);
    }
    yPoints.push_back(points[total].value);
    xPoints.push_back(1);
    return [xPoints, yPoints](double t){
        for (size_t i = 1; i < xPoints.size(); i++){
            const double currentX = xPoints[i];
            if (t <= currentX){
                const double prevX = xPoints[i - 1];
                const double prevY = yPoints[i - 1];
                return prevY + (yPoints[i] - prevY) * (t - prevX) / (currentX - prevX);
            }
        }
        return yPoints.back();
    };
}

EaseFn AnimEasing::irregular(int length, double randomness){
    std::vector<double> values = {0};
    const int total = length - 1;
    for (int i = 1; i < total; i++){
        const double previousValue = values[i - 1];
        const double spacing = (double)i / total;
        const double segmentEnd = (double)(i + 1) / total;
        const double randomVariation = spacing + (segmentEnd - spacing) * AnimUtils::randomUnit();
        const double randomValue = spacing * (1 - randomness) + randomVariation * randomness;
        values.push_back(AnimMath::clamp(randomValue, previousValue, 1));
    }
    values.push_back(1);
    std::vector<AnimLinearPoint> points;
    for (double v : values)
        points.push_back(AnimLinearPoint{v, false, 0.0});
    return linear(points);
}

AnimEaseFunction::AnimEaseFunction(EaseFn fn, std::string name, bool parametric):
        fn(std::move(fn)), name(std::move(name)), parametric(parametric) {}

double AnimEaseFunction::evaluate(double t) const{
    EaseFn f = getFunction();
    return f ? f(t) : t;
}

EaseFn AnimEaseFunction::getFunction() const{
    if (parametric)
        return AnimEasing::get(name);
    return fn ? fn : AnimEasing::none();
}

AnimEaseFunction AnimEaseFunction::withParameters(const std::vector<double>& params) const{
    if (!parametric)
        return *this;
    return AnimEaseFunction(AnimEasing::get(name, params), name + "(...)", false);
}

EaseFn jasperengine::parseEase(const std::string& name){
    return AnimEasing::parse(name);
}
