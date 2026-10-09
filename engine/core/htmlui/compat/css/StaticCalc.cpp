// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/StaticCalc.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/Units.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine::htmlui::compat;

namespace {

    constexpr double kPi = 3.14159265358979323846;

    // the arithmetic type of a canonical unit; "%" matches any type except plain numbers
    enum class CalcType { Number, Length, Angle, Time, Resolution, Frequency, Flex, Percentage, Other };

    CalcType typeOf(const std::string& unit){
        if (unit.empty())
            return CalcType::Number;
        if (unit == "%")
            return CalcType::Percentage;
        if (unit == "deg")
            return CalcType::Angle;
        if (unit == "s")
            return CalcType::Time;
        if (unit == "x")
            return CalcType::Resolution;
        if (unit == "hz")
            return CalcType::Frequency;
        if (unit == "fr")
            return CalcType::Flex;
        if (unit == "px" || isLengthCategory(unitCategory(unit)))
            return CalcType::Length;
        return CalcType::Other;
    }

    // the type of a sum (Percentage when only percentages); nullopt when mixed
    std::optional<CalcType> typeOfSum(const CalcValue& value){
        std::optional<CalcType> result;
        bool percent = false;
        for (const auto& term : value.terms){
            const CalcType type = typeOf(term.first);
            if (type == CalcType::Percentage){
                percent = true;
                continue;
            }
            if (result && *result != type)
                return std::nullopt;
            result = type;
        }
        if (result && *result == CalcType::Number && percent)
            return std::nullopt;
        if (!result)
            return percent ? CalcType::Percentage : CalcType::Number;
        return result;
    }

    bool compatible(const CalcValue& a, const CalcValue& b){
        const std::optional<CalcType> ta = typeOfSum(a);
        const std::optional<CalcType> tb = typeOfSum(b);
        if (!ta || !tb)
            return false;
        if (a.terms.empty() || b.terms.empty())
            return true;   // zero is compatible with anything
        if (*ta == *tb)
            return true;
        if (*ta == CalcType::Percentage)
            return *tb != CalcType::Number;
        if (*tb == CalcType::Percentage)
            return *ta != CalcType::Number;
        return false;
    }

    // canonical unit and factor of a dimension
    std::pair<std::string, double> canonical(const std::string& unit){
        const UnitCategory category = unitCategory(unit);
        switch (category){
            case UnitCategory::AbsoluteLength:
            case UnitCategory::Angle:
            case UnitCategory::Time:
            case UnitCategory::Resolution:
            case UnitCategory::Frequency:
                return {canonicalUnit(category), canonicalFactor(unit).value_or(1.0)};
            default:
                return {lowerAscii(unit), 1.0};
        }
    }

    bool isOperator(const ComponentValue& value, char op){
        return value.isToken(TokenType::Delim) && value.token.isDelim(op);
    }

    struct Fail {
        std::string* error;
        std::optional<CalcValue> operator()(const std::string& message) const {
            if (error && error->empty())
                *error = message;
            return std::nullopt;
        }
    };

    std::vector<std::vector<ComponentValue>> arguments(const ComponentValue& function){
        std::vector<std::vector<ComponentValue>> result;
        std::vector<ComponentValue> current;
        for (const ComponentValue& child : function.children){
            if (child.isToken(TokenType::Comma)){
                result.push_back(std::move(current));
                current.clear();
                continue;
            }
            current.push_back(child);
        }
        result.push_back(std::move(current));
        // trim
        for (std::vector<ComponentValue>& argument : result){
            while (!argument.empty() && (argument.front().isWhitespace() || argument.front().isToken(TokenType::Comment)))
                argument.erase(argument.begin());
            while (!argument.empty() && (argument.back().isWhitespace() || argument.back().isToken(TokenType::Comment)))
                argument.pop_back();
        }
        if (result.size() == 1 && result[0].empty())
            result.clear();
        return result;
    }

    // value as a number (only "" term or empty)
    std::optional<double> asNumber(const CalcValue& value){
        if (!value.isNumber())
            return std::nullopt;
        return value.numberValue();
    }

    // a comparable pair as plain doubles: same single unit, or both resolved to px
    bool comparable(const CalcValue& a, const CalcValue& b, CalcContext& context, double& x, double& y){
        if (!compatible(a, b))
            return false;
        auto singleUnit = [](const CalcValue& v) -> std::string {
            return v.terms.empty() ? std::string() : v.terms.front().first;
        };
        const bool aSimple = a.terms.size() <= 1;
        const bool bSimple = b.terms.size() <= 1;
        if (aSimple && bSimple && (a.terms.empty() || b.terms.empty() || singleUnit(a) == singleUnit(b))){
            x = a.terms.empty() ? 0.0 : a.terms.front().second;
            y = b.terms.empty() ? 0.0 : b.terms.front().second;
            return true;
        }
        std::optional<CalcValue> pa = StaticCalc::resolveToPx(a, context);
        std::optional<CalcValue> pb = StaticCalc::resolveToPx(b, context);
        if (!pa || !pb || pa->terms.size() > 1 || pb->terms.size() > 1)
            return false;
        if (!pa->terms.empty() && !pb->terms.empty() && pa->terms.front().first != pb->terms.front().first)
            return false;
        context.usedContext = true;
        x = pa->terms.empty() ? 0.0 : pa->terms.front().second;
        y = pb->terms.empty() ? 0.0 : pb->terms.front().second;
        return true;
    }

    class Evaluator {
    public:
        Evaluator(CalcContext& context, std::string* error): context(context), fail{error} {}

        std::optional<CalcValue> sum(const std::vector<ComponentValue>& values){
            // split into products at whitespace-surrounded + and -
            std::vector<ComponentValue> items;
            for (const ComponentValue& value : values){
                if (value.isToken(TokenType::Comment))
                    continue;
                items.push_back(value);
            }
            while (!items.empty() && items.front().isWhitespace())
                items.erase(items.begin());
            while (!items.empty() && items.back().isWhitespace())
                items.pop_back();
            if (items.empty())
                return fail("empty expression");

            std::optional<CalcValue> total;
            double sign = 1.0;
            size_t start = 0;
            for (size_t i = 0; i <= items.size(); i++){
                const bool atEnd = i == items.size();
                bool split = false;
                char op = 0;
                if (!atEnd && (isOperator(items[i], '+') || isOperator(items[i], '-'))){
                    // CSS: + and - must be surrounded by whitespace
                    const bool before = i > 0 && items[i - 1].isWhitespace();
                    const bool after = i + 1 < items.size() && items[i + 1].isWhitespace();
                    if (before && after){
                        split = true;
                        op = items[i].token.value[0];
                    }else if (i > start){
                        return fail("'+' and '-' in calc() need spaces around them");
                    }
                }
                if (!atEnd && !split)
                    continue;
                std::vector<ComponentValue> part(items.begin() + (std::ptrdiff_t)start, items.begin() + (std::ptrdiff_t)i);
                while (!part.empty() && part.back().isWhitespace())
                    part.pop_back();
                while (!part.empty() && part.front().isWhitespace())
                    part.erase(part.begin());
                std::optional<CalcValue> value = product(part);
                if (!value)
                    return std::nullopt;
                if (!total){
                    total = value->scaled(sign);
                }else{
                    if (!compatible(*total, *value))
                        return fail("calc() adds values of different types");
                    total = total->plus(*value, sign);
                }
                if (split){
                    sign = op == '-' ? -1.0 : 1.0;
                    start = i + 1;
                }
            }
            return total;
        }

        std::optional<CalcValue> product(const std::vector<ComponentValue>& items){
            if (items.empty())
                return fail("missing operand in calc()");
            std::optional<CalcValue> result;
            char pending = '*';
            bool expectValue = true;
            for (const ComponentValue& item : items){
                if (item.isWhitespace())
                    continue;
                if (!expectValue){
                    if (isOperator(item, '*') || isOperator(item, '/')){
                        pending = item.token.value[0];
                        expectValue = true;
                        continue;
                    }
                    return fail("expected '*' or '/' in calc()");
                }
                std::optional<CalcValue> value = operand(item);
                if (!value)
                    return std::nullopt;
                if (!result){
                    result = value;
                }else if (pending == '*'){
                    if (std::optional<double> n = asNumber(*value))
                        result = result->scaled(*n);
                    else if (std::optional<double> m = asNumber(*result))
                        result = value->scaled(*m);
                    else
                        return fail("calc() multiplies two dimensions");
                }else{
                    std::optional<double> n = asNumber(*value);
                    if (!n)
                        return fail("calc() divides by a dimension");
                    if (*n == 0.0)
                        return fail("calc() divides by zero");
                    result = result->scaled(1.0 / *n);
                }
                expectValue = false;
            }
            if (expectValue)
                return fail("missing operand in calc()");
            return result;
        }

        std::optional<CalcValue> operand(const ComponentValue& item){
            if (item.kind == ComponentValue::Kind::Token){
                const Token& token = item.token;
                switch (token.type){
                    case TokenType::Number: return CalcValue::number(token.number);
                    case TokenType::Percentage: return CalcValue::of(token.number, "%");
                    case TokenType::Dimension: {
                        const std::pair<std::string, double> unit = canonical(token.unit);
                        if (unitCategory(token.unit) == UnitCategory::Unknown)
                            return fail("unknown unit '" + token.unit + "' in calc()");
                        return CalcValue::of(token.number * unit.second, unit.first);
                    }
                    case TokenType::Ident: {
                        const std::string name = lowerAscii(token.value);
                        if (context.identifier){
                            if (std::optional<double> v = context.identifier(token.value))
                                return CalcValue::number(*v);
                        }
                        if (name == "e")
                            return CalcValue::number(2.718281828459045);
                        if (name == "pi")
                            return CalcValue::number(kPi);
                        if (name == "infinity")
                            return CalcValue::number(std::numeric_limits<double>::max());
                        if (name == "-infinity")
                            return CalcValue::number(-std::numeric_limits<double>::max());
                        if (name == "nan")
                            return fail("NaN in calc()");
                        return fail("unexpected '" + token.value + "' in calc()");
                    }
                    default:
                        return fail("unexpected token in calc()");
                }
            }
            if (item.kind == ComponentValue::Kind::Block){
                if (item.token.type != TokenType::LeftParen)
                    return fail("unexpected block in calc()");
                return sum(item.children);
            }
            return function(item);
        }

        std::optional<CalcValue> function(const ComponentValue& item){
            const std::string name = lowerAscii(item.name());
            if (name == "var"){
                if (!context.resolveVar){
                    context.varUnresolved = true;
                    return fail("var() in a math function");
                }
                std::vector<ComponentValue> replacement;
                if (!context.resolveVar(item, replacement)){
                    context.varUnresolved = true;
                    return fail("var() without a static value in a math function");
                }
                if (++depth > 16)
                    return fail("var() nesting too deep");
                std::optional<CalcValue> value = sum(replacement);
                depth--;
                return value;
            }
            if (!StaticCalc::isMathFunction(name))
                return fail("unsupported function '" + item.name() + "()' in calc()");
            context.functions.push_back(name);
            std::vector<std::vector<ComponentValue>> args = arguments(item);

            auto evalArgs = [&](size_t minCount, size_t maxCount) -> std::optional<std::vector<CalcValue>> {
                if (args.size() < minCount || args.size() > maxCount){
                    fail(name + "() takes " + std::to_string(minCount) + (minCount == maxCount ? "" : "+") + " arguments");
                    return std::nullopt;
                }
                std::vector<CalcValue> values;
                for (const std::vector<ComponentValue>& arg : args){
                    std::optional<CalcValue> v = sum(arg);
                    if (!v)
                        return std::nullopt;
                    values.push_back(std::move(*v));
                }
                return values;
            };

            if (name == "calc"){
                if (args.size() != 1)
                    return fail("calc() takes one expression");
                return sum(args[0]);
            }
            if (name == "min" || name == "max"){
                std::optional<std::vector<CalcValue>> values = evalArgs(1, 64);
                if (!values)
                    return std::nullopt;
                size_t best = 0;
                for (size_t i = 1; i < values->size(); i++){
                    double x = 0, y = 0;
                    if (!comparable((*values)[i], (*values)[best], context, x, y))
                        return fail(name + "() compares values it cannot resolve statically");
                    if ((name == "min" && x < y) || (name == "max" && x > y))
                        best = i;
                }
                return (*values)[best];
            }
            if (name == "clamp"){
                std::optional<std::vector<CalcValue>> values = evalArgs(3, 3);
                if (!values)
                    return std::nullopt;
                double lo = 0, v = 0, hi = 0, lo2 = 0, hi2 = 0;
                if (!comparable((*values)[0], (*values)[1], context, lo, v) || !comparable((*values)[2], (*values)[1], context, hi, v) ||
                    !comparable((*values)[0], (*values)[2], context, lo2, hi2))
                    return fail("clamp() compares values it cannot resolve statically");
                // CSS: max(MIN, min(VAL, MAX)); MIN wins over MAX
                if (v > hi){
                    if (lo2 > hi2)
                        return (*values)[0];
                    return (*values)[2];
                }
                if (v < lo)
                    return (*values)[0];
                return (*values)[1];
            }
            if (name == "round"){
                std::string strategy = "nearest";
                if (!args.empty() && args[0].size() == 1 && args[0][0].isToken(TokenType::Ident)){
                    const std::string s = lowerAscii(args[0][0].token.value);
                    if (s == "nearest" || s == "up" || s == "down" || s == "to-zero"){
                        strategy = s;
                        args.erase(args.begin());
                    }
                }
                std::optional<std::vector<CalcValue>> values = evalArgs(1, 2);
                if (!values)
                    return std::nullopt;
                CalcValue a = (*values)[0];
                CalcValue b = values->size() > 1 ? (*values)[1] : CalcValue::number(1.0);
                double x = 0, y = 0;
                if (!comparable(a, b, context, x, y))
                    return fail("round() mixes values it cannot resolve statically");
                if (y == 0.0)
                    return fail("round() by zero");
                const double q = x / y;
                double r = 0;
                if (strategy == "up")
                    r = std::ceil(q);
                else if (strategy == "down")
                    r = std::floor(q);
                else if (strategy == "to-zero")
                    r = std::trunc(q);
                else
                    r = std::floor(q + 0.5);
                return unitLike(a, b, r * y);
            }
            if (name == "mod" || name == "rem"){
                std::optional<std::vector<CalcValue>> values = evalArgs(2, 2);
                if (!values)
                    return std::nullopt;
                double x = 0, y = 0;
                if (!comparable((*values)[0], (*values)[1], context, x, y))
                    return fail(name + "() mixes values it cannot resolve statically");
                if (y == 0.0)
                    return fail(name + "() by zero");
                double r = std::fmod(x, y);
                if (name == "mod" && r != 0.0 && ((r < 0) != (y < 0)))
                    r += y;
                return unitLike((*values)[0], (*values)[1], r);
            }
            if (name == "abs" || name == "sign"){
                std::optional<std::vector<CalcValue>> values = evalArgs(1, 1);
                if (!values)
                    return std::nullopt;
                const CalcValue& a = (*values)[0];
                double x = 0, zero = 0;
                if (!comparable(a, CalcValue(), context, x, zero))
                    return fail(name + "() of a value it cannot resolve statically");
                if (name == "sign")
                    return CalcValue::number(x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0));
                return x < 0 ? a.scaled(-1.0) : a;
            }
            if (name == "sin" || name == "cos" || name == "tan"){
                std::optional<std::vector<CalcValue>> values = evalArgs(1, 1);
                if (!values)
                    return std::nullopt;
                const CalcValue& a = (*values)[0];
                double radians = 0;
                if (a.isNumber())
                    radians = a.numberValue();
                else if (a.single() && a.terms.front().first == "deg")
                    radians = a.terms.front().second * kPi / 180.0;
                else
                    return fail(name + "() takes a number or an angle");
                const double r = name == "sin" ? std::sin(radians) : (name == "cos" ? std::cos(radians) : std::tan(radians));
                return CalcValue::number(r);
            }
            if (name == "asin" || name == "acos" || name == "atan"){
                std::optional<std::vector<CalcValue>> values = evalArgs(1, 1);
                if (!values)
                    return std::nullopt;
                std::optional<double> n = asNumber((*values)[0]);
                if (!n)
                    return fail(name + "() takes a number");
                const double r = name == "asin" ? std::asin(*n) : (name == "acos" ? std::acos(*n) : std::atan(*n));
                if (std::isnan(r))
                    return fail(name + "() out of range");
                return CalcValue::of(r * 180.0 / kPi, "deg");
            }
            if (name == "atan2"){
                std::optional<std::vector<CalcValue>> values = evalArgs(2, 2);
                if (!values)
                    return std::nullopt;
                double y = 0, x = 0;
                if (!comparable((*values)[0], (*values)[1], context, y, x))
                    return fail("atan2() mixes values it cannot resolve statically");
                return CalcValue::of(std::atan2(y, x) * 180.0 / kPi, "deg");
            }
            if (name == "pow"){
                std::optional<std::vector<CalcValue>> values = evalArgs(2, 2);
                if (!values)
                    return std::nullopt;
                std::optional<double> a = asNumber((*values)[0]);
                std::optional<double> b = asNumber((*values)[1]);
                if (!a || !b)
                    return fail("pow() takes numbers");
                return CalcValue::number(std::pow(*a, *b));
            }
            if (name == "sqrt" || name == "exp"){
                std::optional<std::vector<CalcValue>> values = evalArgs(1, 1);
                if (!values)
                    return std::nullopt;
                std::optional<double> a = asNumber((*values)[0]);
                if (!a)
                    return fail(name + "() takes a number");
                if (name == "sqrt" && *a < 0)
                    return fail("sqrt() of a negative number");
                return CalcValue::number(name == "sqrt" ? std::sqrt(*a) : std::exp(*a));
            }
            if (name == "log"){
                std::optional<std::vector<CalcValue>> values = evalArgs(1, 2);
                if (!values)
                    return std::nullopt;
                std::optional<double> a = asNumber((*values)[0]);
                if (!a || *a <= 0)
                    return fail("log() takes a positive number");
                double r = std::log(*a);
                if (values->size() == 2){
                    std::optional<double> b = asNumber((*values)[1]);
                    if (!b || *b <= 0 || *b == 1.0)
                        return fail("log() base must be a positive number other than 1");
                    r /= std::log(*b);
                }
                return CalcValue::number(r);
            }
            if (name == "hypot"){
                std::optional<std::vector<CalcValue>> values = evalArgs(1, 64);
                if (!values)
                    return std::nullopt;
                double total = 0;
                CalcValue first = (*values)[0];
                for (const CalcValue& v : *values){
                    double x = 0, ignored = 0;
                    if (!comparable(v, first, context, x, ignored))
                        return fail("hypot() mixes values it cannot resolve statically");
                    total += x * x;
                }
                double unitValue = 0, ignored = 0;
                comparable(first, first, context, unitValue, ignored);
                return unitLike(first, first, std::sqrt(total));
            }
            return fail("unsupported math function '" + name + "()'");
        }

        // `number` in the unit a and b share (after the comparison decided the unit)
        std::optional<CalcValue> unitLike(const CalcValue& a, const CalcValue& b, double number){
            auto unitOf = [](const CalcValue& v) -> std::optional<std::string> {
                if (v.terms.size() == 1)
                    return v.terms.front().first;
                return std::nullopt;
            };
            std::optional<std::string> ua = unitOf(a), ub = unitOf(b);
            if (a.terms.empty() && b.terms.empty())
                return CalcValue::number(number);
            if (ua && ub && *ua == *ub)
                return CalcValue::of(number, *ua);
            if (ua && b.terms.empty())
                return CalcValue::of(number, *ua);
            if (ub && a.terms.empty())
                return CalcValue::of(number, *ub);
            // resolved through the context: px
            return CalcValue::of(number, "px");
        }

        CalcContext& context;
        Fail fail;
        int depth = 0;
    };

}

CalcValue CalcValue::number(double value){
    CalcValue result;
    result.terms.emplace_back(std::string(), value);
    return result;
}

CalcValue CalcValue::of(double value, std::string unit){
    CalcValue result;
    result.terms.emplace_back(std::move(unit), value);
    return result;
}

bool CalcValue::isZero() const{
    for (const auto& term : terms){
        if (term.second != 0.0)
            return false;
    }
    return true;
}

bool CalcValue::isNumber() const{
    for (const auto& term : terms){
        if (!term.first.empty())
            return false;
    }
    return true;
}

double CalcValue::numberValue() const{
    return get("");
}

double CalcValue::get(std::string_view unit) const{
    for (const auto& term : terms){
        if (term.first == unit)
            return term.second;
    }
    return 0.0;
}

bool CalcValue::has(std::string_view unit) const{
    for (const auto& term : terms){
        if (term.first == unit)
            return true;
    }
    return false;
}

void CalcValue::add(const std::string& unit, double value){
    for (auto& term : terms){
        if (term.first == unit){
            term.second += value;
            return;
        }
    }
    terms.emplace_back(unit, value);
    std::sort(terms.begin(), terms.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
}

CalcValue CalcValue::plus(const CalcValue& other, double sign) const{
    CalcValue result = *this;
    for (const auto& term : other.terms)
        result.add(term.first, sign * term.second);
    // a number term of 0 next to dimensions is noise
    result.terms.erase(std::remove_if(result.terms.begin(), result.terms.end(), [&](const auto& term){
        return term.second == 0.0 && result.terms.size() > 1;
    }), result.terms.end());
    return result;
}

CalcValue CalcValue::scaled(double factor) const{
    CalcValue result = *this;
    for (auto& term : result.terms)
        term.second *= factor;
    return result;
}

bool StaticCalc::isMathFunction(std::string_view name){
    static constexpr std::string_view names[] = {"calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign", "sin", "cos",
                                                 "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp",
                                                 "-webkit-calc", "-moz-calc"};
    for (std::string_view n : names){
        if (equalsNoCase(name, n))
            return true;
    }
    return false;
}

bool StaticCalc::isCalcFamily(std::string_view name){
    return equalsNoCase(name, "calc") || equalsNoCase(name, "min") || equalsNoCase(name, "max") || equalsNoCase(name, "clamp");
}

std::optional<CalcValue> StaticCalc::evaluate(const ComponentValue& value, CalcContext& context, std::string* error){
    Evaluator evaluator(context, error);
    if (value.kind == ComponentValue::Kind::Function){
        // the prefixed calc() of old WebKit/Gecko
        if (equalsNoCase(value.name(), "-webkit-calc") || equalsNoCase(value.name(), "-moz-calc")){
            ComponentValue plain = value;
            plain.token.value = "calc";
            return evaluator.operand(plain);
        }
    }
    return evaluator.operand(value);
}

std::optional<CalcValue> StaticCalc::evaluateSum(const std::vector<ComponentValue>& values, CalcContext& context, std::string* error){
    Evaluator evaluator(context, error);
    return evaluator.sum(values);
}

std::optional<CalcValue> StaticCalc::resolveToPx(const CalcValue& value, CalcContext& context){
    CalcValue result;
    for (const auto& term : value.terms){
        if (term.first.empty() || term.first == "px" || term.first == "deg" || term.first == "s" || term.first == "x" ||
            term.first == "hz"){
            result.add(term.first, term.second);
            continue;
        }
        if (!context.pxPerUnit)
            return std::nullopt;
        std::optional<double> factor = context.pxPerUnit(term.first);
        if (!factor)
            return std::nullopt;
        if (unitCategory(term.first) == UnitCategory::ViewportRelative)
            context.usedViewport = true;
        result.add("px", term.second * *factor);
    }
    return result;
}
