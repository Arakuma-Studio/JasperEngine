// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NumericExpression.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "tinyexpr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace jasperengine::editor;

namespace {

    constexpr double PI = 3.14159265358979323846;

    // ---- the functions tinyexpr does not have

    double fnMin(double a, double b){ return std::min(a, b); }
    double fnMax(double a, double b){ return std::max(a, b); }
    double fnClamp(double value, double low, double high){ return std::min(std::max(value, std::min(low, high)), std::max(low, high)); }
    double fnRound(double value){ return std::round(value); }
    double fnLerp(double a, double b, double t){ return a + (b - a) * t; }
    double fnTau(){ return 2.0 * PI; }

    // L(a, b): a linear ramp over the selection order
    double fnRamp(void* context, double a, double b){
        const NumericContext& ctx = *static_cast<const NumericContext*>(context);
        if (ctx.objectCount <= 1) return a;
        return a + (b - a) * static_cast<double>(ctx.objectIndex) / static_cast<double>(ctx.objectCount - 1);
    }

    uint64_t splitMix64(uint64_t x){
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    // R(a, b): uniform in [a, b), one value per object, the same for the same seed
    double fnRandom(void* context, double a, double b){
        const NumericContext& ctx = *static_cast<const NumericContext*>(context);
        const uint64_t bits = splitMix64((static_cast<uint64_t>(ctx.seed) << 32) ^ static_cast<uint64_t>(ctx.objectIndex + 1));
        const double unit = static_cast<double>(bits >> 11) * (1.0 / 9007199254740992.0);
        return a + (b - a) * unit;
    }

    // tinyexpr's own names (TE_NAT_LOG: log is ln)
    const char* const BUILTINS[] = {"abs", "acos", "asin", "atan", "atan2", "ceil", "cos", "cosh", "e", "exp", "fac", "floor",
                                    "ln", "log", "log10", "ncr", "npr", "pi", "pow", "sin", "sinh", "sqrt", "tan", "tanh"};
    const char* const EXTRAS[] = {"min", "max", "clamp", "round", "lerp", "tau", "L", "R"};

    bool isKnownName(const std::string& name){
        for (const char* builtin : BUILTINS) {
            if (name == builtin) return true;
        }
        for (const char* extra : EXTRAS) {
            if (name == extra) return true;
        }
        return false;
    }

    struct UnitDef {
        const char* name;
        NumericUnit unit;
        double factor;     // to the field's storage unit
    };

    const UnitDef UNITS[] = {
        {"mm", NumericUnit::Meters, 0.001}, {"cm", NumericUnit::Meters, 0.01}, {"m", NumericUnit::Meters, 1.0},
        {"km", NumericUnit::Meters, 1000.0}, {"in", NumericUnit::Meters, 0.0254}, {"ft", NumericUnit::Meters, 0.3048},
        {"deg", NumericUnit::Degrees, 1.0}, {"\xC2\xB0", NumericUnit::Degrees, 1.0}, {"rad", NumericUnit::Degrees, 180.0 / PI},
        {"turn", NumericUnit::Degrees, 360.0},
        {"ms", NumericUnit::Seconds, 0.001}, {"s", NumericUnit::Seconds, 1.0}, {"min", NumericUnit::Seconds, 60.0},
        {"h", NumericUnit::Seconds, 3600.0},
        {"px", NumericUnit::Pixels, 1.0},
    };

    const UnitDef* findUnit(const std::string& name){
        for (const UnitDef& unit : UNITS) {
            if (name == unit.name) return &unit;
        }
        return nullptr;
    }

    std::string trim(std::string_view text){
        size_t begin = 0, end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return std::string(text.substr(begin, end - begin));
    }

    std::string formatFactor(double factor){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", factor);
        return buffer;
    }

    NumericResult failure(std::string message, int position){
        NumericResult result;
        result.error = std::move(message);
        result.position = position;
        if (position > 0) result.error += " at " + std::to_string(position);
        return result;
    }

    // The text tinyexpr compiles: units turned into factors, and for each of its characters the
    // 1-based position in the typed text (errors point at what was typed)
    struct Rewritten {
        std::string text;
        std::vector<int> source;
        bool usesPerObject = false;
    };

    bool rewrite(const std::string& input, int offset, const NumericContext& ctx, Rewritten& out, NumericResult& error){
        auto emit = [&](const std::string& piece, int position) {
            out.text += piece;
            out.source.insert(out.source.end(), piece.size(), position);
        };
        // a unit right after a value: a number, a name that is not a function, a ')'
        bool afterValue = false;
        size_t i = 0;
        while (i < input.size()) {
            const unsigned char c = static_cast<unsigned char>(input[i]);
            const int position = offset + static_cast<int>(i) + 1;
            if (std::isspace(c)) {
                emit(std::string(1, input[i]), position);
                i++;
                continue;
            }
            const bool degreeSign = c == 0xC2 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xB0;
            if (std::isalpha(c) || c == '_' || degreeSign) {
                size_t end = i;
                if (degreeSign) {
                    end = i + 2;
                } else {
                    while (end < input.size() && (std::isalnum(static_cast<unsigned char>(input[end])) || input[end] == '_')) end++;
                }
                const std::string name = input.substr(i, end - i);
                size_t next = end;
                while (next < input.size() && std::isspace(static_cast<unsigned char>(input[next]))) next++;
                const bool call = next < input.size() && input[next] == '(';
                const UnitDef* unit = call ? nullptr : findUnit(name);
                if (unit && afterValue) {
                    if (ctx.unit == NumericUnit::None) {
                        error = failure("Unit '" + name + "' on a field without units", position);
                        return false;
                    }
                    if (unit->unit != ctx.unit) {
                        error = failure("Unit '" + name + "' does not apply to a " + std::string(NumericExpression::unitName(ctx.unit)) +
                                        " field (" + NumericExpression::unitSuffixes(ctx.unit) + ")", position);
                        return false;
                    }
                    emit("*" + formatFactor(unit->factor), position);
                    afterValue = true;
                    i = end;
                    continue;
                }
                if (!isKnownName(name)) {
                    error = failure("Unknown name '" + name + "'", position);
                    return false;
                }
                if (name == "L" || name == "R") out.usesPerObject = true;
                emit(name, position);
                // pi, e, tau are values; functions are followed by '('
                afterValue = !call;
                i = end;
                continue;
            }
            if (std::isdigit(c) || (c == '.' && i + 1 < input.size() && std::isdigit(static_cast<unsigned char>(input[i + 1])))) {
                size_t end = i;
                while (end < input.size() && (std::isdigit(static_cast<unsigned char>(input[end])) || input[end] == '.')) end++;
                if (end < input.size() && (input[end] == 'e' || input[end] == 'E')) {
                    size_t exponent = end + 1;
                    if (exponent < input.size() && (input[exponent] == '+' || input[exponent] == '-')) exponent++;
                    if (exponent < input.size() && std::isdigit(static_cast<unsigned char>(input[exponent]))) {
                        end = exponent;
                        while (end < input.size() && std::isdigit(static_cast<unsigned char>(input[end]))) end++;
                    }
                }
                // a unit glued to the number binds to it alone: 1/2cm = 1/(2 cm)
                size_t unitStart = end;
                while (unitStart < input.size() && std::isspace(static_cast<unsigned char>(input[unitStart]))) unitStart++;
                size_t unitEnd = unitStart;
                if (unitEnd + 1 < input.size() && static_cast<unsigned char>(input[unitEnd]) == 0xC2 && static_cast<unsigned char>(input[unitEnd + 1]) == 0xB0) {
                    unitEnd += 2;
                } else {
                    while (unitEnd < input.size() && std::isalpha(static_cast<unsigned char>(input[unitEnd]))) unitEnd++;
                }
                const std::string number = input.substr(i, end - i);
                const std::string unitName = input.substr(unitStart, unitEnd - unitStart);
                size_t afterUnit = unitEnd;
                while (afterUnit < input.size() && std::isspace(static_cast<unsigned char>(input[afterUnit]))) afterUnit++;
                const UnitDef* unit = !unitName.empty() && !(afterUnit < input.size() && input[afterUnit] == '(') ? findUnit(unitName) : nullptr;
                if (unit) {
                    const int unitPosition = offset + static_cast<int>(unitStart) + 1;
                    if (ctx.unit == NumericUnit::None) {
                        error = failure("Unit '" + unitName + "' on a field without units", unitPosition);
                        return false;
                    }
                    if (unit->unit != ctx.unit) {
                        error = failure("Unit '" + unitName + "' does not apply to a " + std::string(NumericExpression::unitName(ctx.unit)) +
                                        " field (" + NumericExpression::unitSuffixes(ctx.unit) + ")", unitPosition);
                        return false;
                    }
                    emit("(" + number + "*" + formatFactor(unit->factor) + ")", position);
                    i = unitEnd;
                } else {
                    emit(number, position);
                    i = end;
                }
                afterValue = true;
                continue;
            }
            emit(std::string(1, input[i]), position);
            afterValue = c == ')';
            i++;
        }
        return true;
    }

    // ---- the ImGui hook

    struct HookSession {
        ImGuiID id = 0;
        double activation = -1.0;
        double base = 0.0;
    };
    HookSession hookSession;

    struct PendingError {
        bool active = false;
        ImGuiID id = 0;
        std::string message;
        ImRect rect;
        double shownAt = -1.0;
    };
    PendingError pendingError;

    bool pendingMultiEdit = false;
    std::string pendingMultiEditText;

    std::vector<NumericContext>& scopes(){
        static std::vector<NumericContext> stack;
        return stack;
    }

    bool isIntegerType(ImGuiDataType type){
        return type != ImGuiDataType_Float && type != ImGuiDataType_Double;
    }

    double readValue(ImGuiDataType type, const void* data){
        switch (type) {
            case ImGuiDataType_S8: return *static_cast<const ImS8*>(data);
            case ImGuiDataType_U8: return *static_cast<const ImU8*>(data);
            case ImGuiDataType_S16: return *static_cast<const ImS16*>(data);
            case ImGuiDataType_U16: return *static_cast<const ImU16*>(data);
            case ImGuiDataType_S32: return *static_cast<const ImS32*>(data);
            case ImGuiDataType_U32: return *static_cast<const ImU32*>(data);
            case ImGuiDataType_S64: return static_cast<double>(*static_cast<const ImS64*>(data));
            case ImGuiDataType_U64: return static_cast<double>(*static_cast<const ImU64*>(data));
            case ImGuiDataType_Float: return *static_cast<const float*>(data);
            case ImGuiDataType_Double: return *static_cast<const double*>(data);
            default: return 0.0;
        }
    }

    template<class T>
    void writeClamped(void* data, double value){
        const double low = static_cast<double>(std::numeric_limits<T>::lowest());
        const double high = static_cast<double>(std::numeric_limits<T>::max());
        *static_cast<T*>(data) = static_cast<T>(std::min(std::max(value, low), high));
    }

    // false: the type is not a number ImGui knows
    bool writeValue(ImGuiDataType type, void* data, double value){
        switch (type) {
            case ImGuiDataType_S8: writeClamped<ImS8>(data, value); return true;
            case ImGuiDataType_U8: writeClamped<ImU8>(data, value); return true;
            case ImGuiDataType_S16: writeClamped<ImS16>(data, value); return true;
            case ImGuiDataType_U16: writeClamped<ImU16>(data, value); return true;
            case ImGuiDataType_S32: writeClamped<ImS32>(data, value); return true;
            case ImGuiDataType_U32: writeClamped<ImU32>(data, value); return true;
            case ImGuiDataType_S64: writeClamped<ImS64>(data, value); return true;
            case ImGuiDataType_U64: writeClamped<ImU64>(data, value); return true;
            case ImGuiDataType_Float: *static_cast<float*>(data) = static_cast<float>(value); return true;
            case ImGuiDataType_Double: *static_cast<double*>(data) = value; return true;
            default: return false;
        }
    }

    int applyFromTextHook(const char* buf, ImGuiDataType type, void* data, const char* format, void* userData){
        const std::string_view text(buf);
        ImGuiContext& g = *GImGui;
        if (NumericExpression::isPlainNumber(text)) {
            if (pendingError.active && pendingError.id == g.ActiveId) pendingError.active = false;
            pendingMultiEdit = false;
            return -1;
        }
        if (type >= ImGuiDataType_COUNT || type == ImGuiDataType_Bool || type == ImGuiDataType_String) return -1;

        // relative ops apply to the value the field had when it was activated (the field
        // applies every keystroke)
        const double current = readValue(type, data);
        const ImGuiID id = g.ActiveId;
        const double activation = g.Time - static_cast<double>(g.ActiveIdTimer);
        if (id == 0) {
            hookSession = {0, -1.0, current};
        } else if (id != hookSession.id || std::fabs(activation - hookSession.activation) > 0.05) {
            hookSession = {id, activation, current};
        }

        NumericContext context;
        if (const NumericContext* scope = NumericFieldScope::current()) context = *scope;
        context.integer = context.integer || isIntegerType(type);
        context.objectIndex = 0;

        const NumericResult result = NumericExpression::evaluate(text, hookSession.base, context);
        if (!result.ok) {
            pendingError = {true, id, result.error, g.LastItemData.Rect, -1.0};
            pendingMultiEdit = false;
            return 0;
        }
        if (pendingError.active && pendingError.id == id) pendingError.active = false;
        pendingMultiEdit = result.perObject && context.objectCount > 1;
        pendingMultiEditText = pendingMultiEdit ? std::string(text) : std::string();

        ImGuiDataTypeStorage before;
        const size_t size = ImGui::DataTypeGetInfo(type)->Size;
        std::memcpy(&before, data, size);
        writeValue(type, data, result.value);
        return std::memcmp(&before, data, size) != 0 ? 1 : 0;
    }

}

// ------------------------------------------------------------------ evaluation

bool NumericExpression::isPlainNumber(std::string_view text){
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) i++;
    bool digits = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        i++;
        digits = true;
    }
    if (i < text.size() && text[i] == '.') {
        i++;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            i++;
            digits = true;
        }
    }
    if (!digits) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        size_t exponent = i + 1;
        if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) exponent++;
        if (exponent >= text.size() || !std::isdigit(static_cast<unsigned char>(text[exponent]))) return false;
        i = exponent;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
    }
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
    return i == text.size();
}

const char* NumericExpression::unitSuffixes(NumericUnit unit){
    switch (unit) {
        case NumericUnit::Meters: return "mm cm m km in ft";
        case NumericUnit::Degrees: return "deg \xC2\xB0 rad turn";
        case NumericUnit::Seconds: return "ms s min h";
        case NumericUnit::Pixels: return "px";
        default: return "";
    }
}

const char* NumericExpression::unitName(NumericUnit unit){
    switch (unit) {
        case NumericUnit::Meters: return "Meters";
        case NumericUnit::Degrees: return "Degrees";
        case NumericUnit::Seconds: return "Seconds";
        case NumericUnit::Pixels: return "Pixels";
        default: return "None";
    }
}

bool NumericExpression::parseUnit(const std::string& name, NumericUnit& out){
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.empty() || lower == "none") out = NumericUnit::None;
    else if (lower == "meters" || lower == "m") out = NumericUnit::Meters;
    else if (lower == "degrees" || lower == "deg") out = NumericUnit::Degrees;
    else if (lower == "seconds" || lower == "s") out = NumericUnit::Seconds;
    else if (lower == "pixels" || lower == "px") out = NumericUnit::Pixels;
    else return false;
    return true;
}

NumericResult NumericExpression::evaluate(std::string_view textIn, double current, const NumericContext& context){
    std::string text(textIn);
    // the leading relative op: +=, -=, *=, /=
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start++;
    char op = 0;
    if (start + 1 < text.size() && text[start + 1] == '=' && std::strchr("+-*/", text[start])) {
        op = text[start];
        start += 2;
    }
    std::string body = text.substr(start);
    // a decimal comma where nothing takes arguments: 1,5 is 1.5
    if (body.find(',') != std::string::npos && body.find('(') == std::string::npos) std::replace(body.begin(), body.end(), ',', '.');
    if (trim(body).empty()) return failure(op ? "A value is missing after the operator" : "Nothing to evaluate", static_cast<int>(start) + 1);

    Rewritten rewritten;
    NumericResult result;
    if (!rewrite(body, static_cast<int>(start), context, rewritten, result)) return result;

    NumericContext closureContext = context;
    const te_variable variables[] = {
        {"min", reinterpret_cast<const void*>(&fnMin), TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
        {"max", reinterpret_cast<const void*>(&fnMax), TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
        {"clamp", reinterpret_cast<const void*>(&fnClamp), TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
        {"round", reinterpret_cast<const void*>(&fnRound), TE_FUNCTION1 | TE_FLAG_PURE, nullptr},
        {"lerp", reinterpret_cast<const void*>(&fnLerp), TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
        {"tau", reinterpret_cast<const void*>(&fnTau), TE_FUNCTION0 | TE_FLAG_PURE, nullptr},
        {"L", reinterpret_cast<const void*>(&fnRamp), TE_CLOSURE2, &closureContext},
        {"R", reinterpret_cast<const void*>(&fnRandom), TE_CLOSURE2, &closureContext},
    };
    int errorAt = 0;
    te_expr* expression = te_compile(rewritten.text.c_str(), variables, static_cast<int>(sizeof(variables) / sizeof(variables[0])), &errorAt);
    if (!expression) {
        int position = static_cast<int>(start) + 1;
        if (errorAt > 0 && !rewritten.source.empty()) {
            position = rewritten.source[std::min<size_t>(static_cast<size_t>(errorAt - 1), rewritten.source.size() - 1)];
        }
        return failure("Syntax error", position);
    }
    double value = te_eval(expression);
    te_free(expression);
    if (!std::isfinite(value)) return failure("The result is not a number", 0);

    switch (op) {
        case '+': value = current + value; break;
        case '-': value = current - value; break;
        case '*': value = current * value; break;
        case '/':
            if (value == 0.0) return failure("Division by zero", static_cast<int>(start));
            value = current / value;
            break;
        default: break;
    }
    if (context.integer) value = std::round(value);   // half away from zero
    if (!std::isfinite(value)) return failure("The result is not a number", 0);

    result.ok = true;
    result.value = value;
    result.perObject = op != 0 || rewritten.usesPerObject;
    return result;
}

NumericResult NumericExpression::evaluateWith(std::string_view textIn, const std::vector<std::pair<std::string, double>>& variables){
    const std::string text(textIn);
    if (trim(text).empty()) return failure("Nothing to evaluate", 1);
    // the values live here while the expression is compiled and evaluated
    std::vector<double> values;
    values.reserve(variables.size());
    for (const auto& [name, value] : variables) values.push_back(value);
    std::vector<te_variable> table = {
        {"min", reinterpret_cast<const void*>(&fnMin), TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
        {"max", reinterpret_cast<const void*>(&fnMax), TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
        {"clamp", reinterpret_cast<const void*>(&fnClamp), TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
        {"round", reinterpret_cast<const void*>(&fnRound), TE_FUNCTION1 | TE_FLAG_PURE, nullptr},
        {"lerp", reinterpret_cast<const void*>(&fnLerp), TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
        {"tau", reinterpret_cast<const void*>(&fnTau), TE_FUNCTION0 | TE_FLAG_PURE, nullptr},
    };
    for (size_t i = 0; i < variables.size(); i++) table.push_back({variables[i].first.c_str(), &values[i], TE_VARIABLE, nullptr});
    int errorAt = 0;
    te_expr* expression = te_compile(text.c_str(), table.data(), static_cast<int>(table.size()), &errorAt);
    if (!expression) return failure("Syntax error or unknown name", std::max(1, errorAt));
    const double value = te_eval(expression);
    te_free(expression);
    if (!std::isfinite(value)) return failure("The result is not a number", 0);
    NumericResult result;
    result.ok = true;
    result.value = value;
    return result;
}

bool NumericExpression::evaluateForObjects(const std::string& text, const std::vector<double>& current, NumericContext context,
                                           std::vector<double>& out){
    out.clear();
    context.objectCount = static_cast<int>(current.size());
    for (size_t i = 0; i < current.size(); i++) {
        context.objectIndex = static_cast<int>(i);
        const NumericResult result = evaluate(text, current[i], context);
        if (!result.ok) return false;
        out.push_back(result.value);
    }
    return true;
}

// ------------------------------------------------------------------ the editor side

void NumericExpression::installImGuiHook(){
    ImGui::SetDataTypeApplyFromTextHook(&applyFromTextHook, nullptr);
}

bool NumericExpression::takeMultiEdit(std::string& text){
    if (!pendingMultiEdit) return false;
    pendingMultiEdit = false;
    text = std::move(pendingMultiEditText);
    pendingMultiEditText.clear();
    return true;
}

void NumericExpression::drawErrorTooltip(){
    if (!pendingError.active || !GImGui) return;
    ImGuiContext& g = *GImGui;
    if (pendingError.shownAt < 0.0) {
        // shown once the field is left with the expression that failed
        if (g.ActiveId == pendingError.id) return;
        pendingError.shownAt = g.Time;
    }
    if (g.Time - pendingError.shownAt > 3.0) {
        pendingError.active = false;
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(pendingError.rect.Min.x, pendingError.rect.Max.y + 2.0f));
    if (ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(pendingError.message.c_str());
        ImGui::EndTooltip();
    }
}

NumericFieldScope::NumericFieldScope(NumericUnit unit, int objectCount, bool integer){
    NumericContext context;
    context.unit = unit;
    context.objectCount = std::max(objectCount, 1);
    context.integer = integer;
    context.seed = static_cast<uint32_t>(ImGui::GetFrameCount());
    scopes().push_back(context);
}

NumericFieldScope::~NumericFieldScope(){
    const NumericUnit unit = scopes().back().unit;
    scopes().pop_back();
    if (unit != NumericUnit::None && GImGui && !ImGui::IsItemActive()) {
        ImGui::SetItemTooltip("%s: %s (math works too: 2*1.5m, +=10)", NumericExpression::unitName(unit), NumericExpression::unitSuffixes(unit));
    }
}

const NumericContext* NumericFieldScope::current(){
    return scopes().empty() ? nullptr : &scopes().back();
}
