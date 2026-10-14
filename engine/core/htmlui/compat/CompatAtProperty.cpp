// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/CompatAtProperty.h"

#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/CustomPropertyIndex.h"
#include "htmlui/compat/css/MediaQuery.h"
#include "htmlui/compat/css/RewriteSession.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <algorithm>

using namespace jasperengine::htmlui::compat;

namespace {

    bool isWs(const ComponentValue& v){
        return v.isWhitespace() || v.isToken(TokenType::Comment);
    }

    std::vector<const ComponentValue*> significant(const ValueList& values){
        std::vector<const ComponentValue*> out;
        for (const ComponentValue& v : values){
            if (!isWs(v))
                out.push_back(&v);
        }
        return out;
    }

    bool containsVar(const ValueList& values){
        for (const ComponentValue& v : values){
            if (v.kind == ComponentValue::Kind::Function && equalsNoCase(v.name(), "var"))
                return true;
            if (v.kind != ComponentValue::Kind::Token && containsVar(v.children))
                return true;
        }
        return false;
    }

    bool isMathFunction(const ComponentValue& v){
        if (v.kind != ComponentValue::Kind::Function)
            return false;
        const std::string name = lowerAscii(v.name());
        return name == "calc" || name == "min" || name == "max" || name == "clamp";
    }

    // The unit categories a math function's terms use (numbers: UnitCategory::None).
    void mathCategories(const ValueList& values, std::vector<UnitCategory>& out, bool& other){
        for (const ComponentValue& v : values){
            if (isWs(v))
                continue;
            if (v.kind == ComponentValue::Kind::Function){
                if (!isMathFunction(v) && !equalsNoCase(v.name(), "var")){
                    other = true;
                    continue;
                }
                mathCategories(v.children, out, other);
                continue;
            }
            if (v.kind == ComponentValue::Kind::Block){
                mathCategories(v.children, out, other);
                continue;
            }
            switch (v.token.type){
                case TokenType::Number: out.push_back(UnitCategory::None); break;
                case TokenType::Percentage: out.push_back(UnitCategory::Percentage); break;
                case TokenType::Dimension: out.push_back(unitCategory(v.token.unit)); break;
                case TokenType::Delim:
                case TokenType::Comma: break;
                case TokenType::Ident:
                    // e, pi, infinity, ... (CSS Values 4 constants)
                    break;
                default: other = true; break;
            }
        }
    }

    bool lengthCategory(UnitCategory category, bool initial){
        switch (category){
            case UnitCategory::AbsoluteLength:
            case UnitCategory::ViewportRelative:
            case UnitCategory::EngineLength:
                return true;
            case UnitCategory::FontRelative:
                // not computationally independent (CSS Properties and Values API 1 §3.1)
                return !initial;
            default:
                return false;
        }
    }

    // One numeric component of the syntax's type.
    bool matchesNumeric(AtPropertySyntax syntax, const ComponentValue& v, bool initial){
        if (isMathFunction(v)){
            std::vector<UnitCategory> categories;
            bool other = false;
            mathCategories(v.children, categories, other);
            if (other)
                return false;
            for (UnitCategory category : categories){
                switch (syntax){
                    case AtPropertySyntax::Number:
                    case AtPropertySyntax::Integer:
                        if (category != UnitCategory::None)
                            return false;
                        break;
                    case AtPropertySyntax::Length:
                        if (category != UnitCategory::None && !lengthCategory(category, initial))
                            return false;
                        break;
                    case AtPropertySyntax::Percentage:
                        if (category != UnitCategory::None && category != UnitCategory::Percentage)
                            return false;
                        break;
                    case AtPropertySyntax::LengthPercentage:
                        if (category != UnitCategory::None && category != UnitCategory::Percentage && !lengthCategory(category, initial))
                            return false;
                        break;
                    case AtPropertySyntax::Angle:
                        if (category != UnitCategory::None && category != UnitCategory::Angle)
                            return false;
                        break;
                    case AtPropertySyntax::Time:
                        if (category != UnitCategory::None && category != UnitCategory::Time)
                            return false;
                        break;
                    default: return false;
                }
            }
            return true;
        }
        if (v.kind != ComponentValue::Kind::Token)
            return false;
        const Token& t = v.token;
        switch (syntax){
            case AtPropertySyntax::Number: return t.type == TokenType::Number;
            case AtPropertySyntax::Integer: return t.type == TokenType::Number && t.isInteger;
            case AtPropertySyntax::Percentage: return t.type == TokenType::Percentage;
            case AtPropertySyntax::Length:
            case AtPropertySyntax::LengthPercentage:
                if (t.type == TokenType::Number)
                    return t.number == 0;
                if (t.type == TokenType::Percentage)
                    return syntax == AtPropertySyntax::LengthPercentage;
                return t.type == TokenType::Dimension && lengthCategory(unitCategory(t.unit), initial);
            case AtPropertySyntax::Angle: return t.type == TokenType::Dimension && unitCategory(t.unit) == UnitCategory::Angle;
            case AtPropertySyntax::Time: return t.type == TokenType::Dimension && unitCategory(t.unit) == UnitCategory::Time;
            default: return false;
        }
    }

    // The CSS-wide keywords are not values of any syntax here (an initial-value) or are the cascade's (a declaration).
    bool isCssWideKeyword(const ComponentValue& v){
        if (!v.isToken(TokenType::Ident))
            return false;
        for (const char* keyword : {"initial", "inherit", "unset", "revert", "revert-layer"}){
            if (equalsNoCase(v.token.value, keyword))
                return true;
        }
        return false;
    }

    // ---- the syntax string (CSS Properties and Values API 1 §5)

    bool isDataTypeName(std::string_view name){
        static const char* const names[] = {"length", "number", "percentage", "length-percentage", "color", "image", "url", "integer",
                                            "angle", "time", "resolution", "transform-function", "custom-ident", "transform-list", "string"};
        for (const char* n : names){
            if (name == n)
                return true;
        }
        return false;
    }

    bool isIdentStart(char c){
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || (unsigned char)c >= 0x80;
    }

    bool isIdentChar(char c){
        return isIdentStart(c) || (c >= '0' && c <= '9');
    }

    std::string_view trimView(std::string_view text){
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r' || text.front() == '\f'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r' || text.back() == '\f'))
            text.remove_suffix(1);
        return text;
    }

    // The rule's prelude: one custom property name ("" when it is not one).
    std::string propertyName(const Rule& rule){
        std::vector<const ComponentValue*> words = significant(rule.prelude);
        if (words.size() != 1 || !words[0]->isToken(TokenType::Ident))
            return std::string();
        const std::string name = words[0]->token.value;
        if (name.size() < 3 || name[0] != '-' || name[1] != '-')
            return std::string();
        return name;
    }

    // A media result for the compile's viewport and themes (registrations cannot follow a query at runtime).
    bool mediaMatches(const MediaResult& media, const CompatOptions& options, bool& viewportDependent){
        if (media.always)
            return true;
        if (media.never)
            return false;
        const double width = options.viewport.x, height = options.viewport.y;
        auto within = [](const MediaConjunction::Range& range, double value){
            return (!range.min || value >= *range.min) && (!range.max || value <= *range.max);
        };
        auto themeActive = [&](const std::string& theme){
            return theme == options.colorScheme || (theme == "reduced-motion" && options.reducedMotion);
        };
        bool any = false;
        for (const MediaConjunction& block : media.blocks){
            if (block.width.min || block.width.max || block.height.min || block.height.max || block.aspect.min || block.aspect.max ||
                !block.orientation.empty())
                viewportDependent = true;
            if (block.contradiction || block.inexpressible)
                continue;
            bool match = within(block.width, width) && within(block.height, height) && within(block.resolution, options.dpRatio) &&
                         (height <= 0 || within(block.aspect, width / height));
            if (!block.orientation.empty())
                match = match && ((block.orientation == "portrait") == (height >= width));
            if (!block.theme.empty())
                match = match && themeActive(block.theme);
            if (!block.negatedTheme.empty())
                match = match && !themeActive(block.negatedTheme);
            any = any || match;
        }
        return any;
    }

    Rml::RegisteredPropertySyntax toRml(AtPropertySyntax syntax){
        return (Rml::RegisteredPropertySyntax)(uint8_t)syntax;
    }

    std::string describe(AtPropertySyntax syntax, bool inherits, const std::string& initial, bool hasInitial){
        return std::string("syntax '") + CompatAtProperty::syntaxName(syntax) + "', inherits " + (inherits ? "true" : "false") +
               (hasInitial ? ", initial-value " + initial : ", no initial-value");
    }

}

static_assert((int)AtPropertySyntax::Time == (int)Rml::RegisteredPropertySyntax::Time &&
              (int)AtPropertySyntax::Universal == (int)Rml::RegisteredPropertySyntax::Universal &&
              (int)AtPropertySyntax::Color == (int)Rml::RegisteredPropertySyntax::Color,
              "AtPropertySyntax follows Rml::RegisteredPropertySyntax");

const char* CompatAtProperty::syntaxName(AtPropertySyntax syntax){
    switch (syntax){
        case AtPropertySyntax::Universal: return "*";
        case AtPropertySyntax::Number: return "<number>";
        case AtPropertySyntax::Integer: return "<integer>";
        case AtPropertySyntax::Length: return "<length>";
        case AtPropertySyntax::Percentage: return "<percentage>";
        case AtPropertySyntax::LengthPercentage: return "<length-percentage>";
        case AtPropertySyntax::Color: return "<color>";
        case AtPropertySyntax::Angle: return "<angle>";
        case AtPropertySyntax::Time: return "<time>";
    }
    return "*";
}

std::optional<AtPropertySyntax> CompatAtProperty::parseSyntax(std::string_view syntax, bool& valid){
    valid = false;
    const std::string_view text = trimView(syntax);
    if (text.empty())
        return std::nullopt;
    if (text == "*"){
        valid = true;
        return AtPropertySyntax::Universal;
    }
    // <syntax-component> [ '|' <syntax-component> ]*, a component <type-name> or an ident, with '+' or '#'
    size_t i = 0;
    size_t components = 0;
    bool multiplied = false;
    std::string single;
    while (true){
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n'))
            i++;
        if (i >= text.size())
            return std::nullopt;   // empty component (a trailing '|')
        std::string component;
        if (text[i] == '<'){
            const size_t close = text.find('>', i);
            if (close == std::string_view::npos)
                return std::nullopt;
            const std::string_view name = text.substr(i + 1, close - i - 1);
            // data type names are case-sensitive (Chromium: '<LENGTH>' is invalid)
            if (!isDataTypeName(name))
                return std::nullopt;
            component = std::string(name);
            i = close + 1;
        }else if (isIdentStart(text[i])){
            const size_t start = i;
            while (i < text.size() && isIdentChar(text[i]))
                i++;
            const std::string_view ident = text.substr(start, i - start);
            // CSS-wide keywords and 'default' are not idents of a syntax
            for (const char* reserved : {"initial", "inherit", "unset", "revert", "revert-layer", "default"}){
                if (equalsNoCase(ident, reserved))
                    return std::nullopt;
            }
            component = "ident";
        }else{
            return std::nullopt;
        }
        if (i < text.size() && (text[i] == '+' || text[i] == '#')){
            // '<transform-list>' takes no multiplier
            if (component == "transform-list")
                return std::nullopt;
            multiplied = true;
            i++;
        }
        components++;
        single = component;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n'))
            i++;
        if (i >= text.size())
            break;
        if (text[i] != '|')
            return std::nullopt;
        i++;
    }
    valid = true;
    if (components != 1 || multiplied)
        return std::nullopt;
    if (single == "length")
        return AtPropertySyntax::Length;
    if (single == "number")
        return AtPropertySyntax::Number;
    if (single == "integer")
        return AtPropertySyntax::Integer;
    if (single == "percentage")
        return AtPropertySyntax::Percentage;
    if (single == "length-percentage")
        return AtPropertySyntax::LengthPercentage;
    if (single == "color")
        return AtPropertySyntax::Color;
    if (single == "angle")
        return AtPropertySyntax::Angle;
    if (single == "time")
        return AtPropertySyntax::Time;
    return std::nullopt;
}

bool CompatAtProperty::matches(AtPropertySyntax syntax, const ValueList& value, bool initial){
    if (containsVar(value))
        return !initial;
    if (syntax == AtPropertySyntax::Universal)
        return true;
    std::vector<const ComponentValue*> words = significant(value);
    if (words.size() != 1)
        return false;
    const ComponentValue& v = *words[0];
    if (isCssWideKeyword(v))
        return !initial;
    if (syntax == AtPropertySyntax::Color)
        return ColorValue::isColorSyntax(v);
    return matchesNumeric(syntax, v, initial);
}

CompatAtProperty::Outcome CompatAtProperty::take(Session& session, const Rule& rule, bool nestedInStyleRule, const MediaResult& media){
    const SourceRef at = rule.preludeRef.valid() ? SourceRef{rule.ref.file, rule.ref.offset, rule.preludeRef.offset + rule.preludeRef.length - rule.ref.offset}
                                                 : rule.ref;
    if (nestedInStyleRule){
        // (Chromium ignores it: @property is valid at the top level and in conditional group rules only)
        session.sink->reportAs(Severity::Error, codes::AtPropertyInvalid, at, "@property inside a style rule is invalid; dropped",
                               "move it to the top level");
        return Outcome::Dropped;
    }
    const std::string name = propertyName(rule);
    if (name.empty()){
        session.sink->reportAs(Severity::Error, codes::AtPropertyInvalid, at,
                               "@property needs one custom property name (--name); the rule is dropped", "write @property --name { ... }");
        return Outcome::Dropped;
    }

    // the descriptors (the last of each wins; others are ignored, as in Chromium)
    const Declaration* syntaxDescriptor = nullptr;
    const Declaration* inheritsDescriptor = nullptr;
    const Declaration* initialDescriptor = nullptr;
    for (const Declaration& d : rule.declarations){
        if (d.name == "syntax")
            syntaxDescriptor = &d;
        else if (d.name == "inherits")
            inheritsDescriptor = &d;
        else if (d.name == "initial-value")
            initialDescriptor = &d;
    }
    auto invalid = [&](SourceRef ref, const std::string& message, const std::string& suggestion){
        session.sink->reportAs(Severity::Error, codes::AtPropertyInvalid, ref.valid() ? ref : at, "@property " + name + ": " + message +
                               "; the rule is dropped", suggestion, std::string(), name);
        return Outcome::Dropped;
    };

    if (!syntaxDescriptor)
        return invalid(at, "the syntax descriptor is missing (required)", "add syntax: '<length>'; (or '*')");
    std::vector<const ComponentValue*> syntaxWords = significant(syntaxDescriptor->value);
    if (syntaxWords.size() != 1 || !syntaxWords[0]->isToken(TokenType::String))
        return invalid(syntaxDescriptor->ref, "syntax must be a string", "quote it: syntax: '<length>';");
    const std::string syntaxText = std::string(trimView(syntaxWords[0]->token.value.view()));
    bool validSyntax = false;
    const std::optional<AtPropertySyntax> type = parseSyntax(syntaxText, validSyntax);
    if (!validSyntax)
        return invalid(syntaxDescriptor->ref, "'" + syntaxText + "' is not a valid syntax",
                       "use a type such as '<length>', '<number>', '<color>', '<angle>', or '*'");

    if (!inheritsDescriptor)
        return invalid(at, "the inherits descriptor is missing (required)", "add inherits: true; or inherits: false;");
    std::vector<const ComponentValue*> inheritsWords = significant(inheritsDescriptor->value);
    const bool inheritsTrue = inheritsWords.size() == 1 && inheritsWords[0]->isToken(TokenType::Ident) &&
                              equalsNoCase(inheritsWords[0]->token.value, "true");
    const bool inheritsFalse = inheritsWords.size() == 1 && inheritsWords[0]->isToken(TokenType::Ident) &&
                               equalsNoCase(inheritsWords[0]->token.value, "false");
    if (!inheritsTrue && !inheritsFalse)
        return invalid(inheritsDescriptor->ref, "inherits must be true or false", "inherits: false;");

    // the initial value: required unless the syntax is '*', computationally independent, no var()
    const bool universal = syntaxText == "*";
    if (!initialDescriptor && !universal)
        return invalid(at, "initial-value is missing (required for the syntax '" + syntaxText + "')", "add initial-value: <a value of the syntax>;");
    if (initialDescriptor && containsVar(initialDescriptor->value))
        return invalid(initialDescriptor->ref, "initial-value cannot use var()", "write the value itself");
    if (initialDescriptor && type && !matches(*type, initialDescriptor->value, true))
        return invalid(initialDescriptor->ref, "initial-value '" + serialize(trimWhitespace(initialDescriptor->value)) + "' is not a " +
                                                   "computationally independent " + syntaxText + " (relative font units and var() are not)",
                       "use an absolute value such as " + std::string(*type == AtPropertySyntax::Color ? "#000000" :
                                                                     *type == AtPropertySyntax::Angle ? "0deg" :
                                                                     *type == AtPropertySyntax::Time ? "0s" :
                                                                     *type == AtPropertySyntax::Percentage ? "0%" :
                                                                     (*type == AtPropertySyntax::Length || *type == AtPropertySyntax::LengthPercentage) ? "0px" : "0"));

    if (!type){
        // a valid syntax the fork does not register: the root-rule emulation of phase 2 (the caller)
        session.sink->reportAs(Severity::Warning, codes::AtPropertyUnsupportedSyntax, syntaxDescriptor->ref,
                               "@property " + name + " { syntax: '" + syntaxText + "' } is not a syntax the engine registers; " + name +
                               " stays an unregistered custom property (inherited, changes discretely) with its initial value on the root",
                               "use one of <length>, <number>, <integer>, <percentage>, <length-percentage>, <color>, <angle>, <time> or *",
                               std::string(), name);
        return Outcome::Fallback;
    }

    // a conditional group rule: registered when it matches the compile's viewport (a later viewport compiles again)
    bool viewportDependent = false;
    const bool matching = mediaMatches(media, session.options, viewportDependent);
    if (viewportDependent)
        session.result->dependsOnViewport = true;
    if (!matching)
        return Outcome::Dropped;

    RegisteredProperty property;
    property.name = name;
    property.syntax = syntaxText;
    property.type = *type;
    property.inherits = inheritsTrue;
    property.ref = rule.ref;
    if (initialDescriptor && !trimWhitespace(initialDescriptor->value).empty()){
        // written as a declaration of the property is (units the fork computes, colors as it parses them)
        NormalizeContext context;
        context.customProperty = true;
        context.options = &session.options;
        context.calcNative = true;
        context.unitsNative = session.options.fork.has(ForkFeature::ExtraUnits);
        context.keepCurrentColorKeyword = *type == AtPropertySyntax::Color;
        NormalizeResult normalized = ValueNormalizer::normalize(initialDescriptor->value, context);
        ValueList value = normalized.ok ? std::move(normalized.value) : trimWhitespace(initialDescriptor->value);
        trimInPlace(value);
        property.initialValue = serialize(value);
        property.hasInitialValue = !property.initialValue.empty();
    }
    if (!property.hasInitialValue && !universal)
        return invalid(at, "initial-value is empty", "add initial-value: <a value of the syntax>;");

    // the last valid rule of a name wins (Chromium)
    for (RegisteredProperty& existing : session.registeredProperties){
        if (existing.name == name){
            existing = std::move(property);
            return Outcome::Registered;
        }
    }
    session.registeredProperties.push_back(std::move(property));
    return Outcome::Registered;
}

void CompatAtProperty::finish(Session& session){
    for (const RegisteredProperty& property : session.registeredProperties){
        if (!property.hasInitialValue)
            continue;
        // the initial value declares the name for the rewrites that read a variable's values (CustomPropertyIndex)
        std::optional<ValueList> value;
        if (std::optional<Declaration> declaration = parseDeclaration(0, property.name + ": " + property.initialValue, 0, nullptr))
            value = declaration->value;
        if (!value)
            continue;
        CustomPropertyIndex::Entry entry;
        entry.value = std::move(*value);
        entry.ref = property.ref;
        entry.rule = 0;
        session.vars.add(property.name, std::move(entry));
    }
    if (session.result)
        session.result->registeredProperties = session.registeredProperties;
}

const RegisteredProperty* CompatAtProperty::find(const Session& session, std::string_view name){
    for (const RegisteredProperty& property : session.registeredProperties){
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

std::vector<Diagnostic> CompatAtProperty::registerAll(const std::vector<RegisteredProperty>& properties, const SourceFiles& files){
    DiagnosticSink sink(files);
    // the system interface is set before Rml::Initialise and cleared by Rml::Shutdown
    if (properties.empty() || Rml::GetSystemInterface() == nullptr)
        return sink.take();
    for (const RegisteredProperty& property : properties){
        const std::string wanted = describe(property.type, property.inherits, property.initialValue, property.hasInitialValue);
        const Rml::PropertyId existing = Rml::StyleSheetSpecification::GetPropertyId(property.name);
        if (existing != Rml::PropertyId::Invalid){
            const Rml::RegisteredCustomProperty* registration = Rml::StyleSheetSpecification::GetRegisteredCustomProperty(existing);
            if (!registration)
                continue;   // (not a custom property name: never the case for --names)
            const bool same = registration->syntax == toRml(property.type) && registration->inherits == property.inherits &&
                              registration->has_initial_value == property.hasInitialValue &&
                              (!property.hasInitialValue || registration->initial_value == property.initialValue);
            if (!same){
                const std::string inEffect = describe((AtPropertySyntax)(uint8_t)registration->syntax, registration->inherits,
                                                      registration->initial_value, registration->has_initial_value);
                sink.report(codes::AtPropertyConflict, property.ref,
                            "@property " + property.name + " (" + wanted + ") conflicts with the registration in effect (" + inEffect +
                            "), which stays: registrations are process-global and the first one wins until the engine restarts",
                            "rename the property, or give every @property " + property.name + " the same definition", std::string(),
                            property.name);
            }
            continue;
        }
        if (Rml::StyleSheetSpecification::GetNumFreePropertyIds() <= 0){
            sink.reportAs(Severity::Warning, codes::AtPropertyUnsupportedSyntax, property.ref,
                          "@property " + property.name + " is not registered: the engine has no property id left for registered "
                          "custom properties; it stays an unregistered custom property",
                          "register fewer custom properties (they are process-global)", std::string(), property.name);
            continue;
        }
        const Rml::PropertyId id = Rml::StyleSheetSpecification::RegisterCustomProperty(property.name, toRml(property.type), property.inherits,
                                                                                         property.hasInitialValue ? property.initialValue : std::string());
        if (id == Rml::PropertyId::Invalid){
            sink.reportAs(Severity::Error, codes::AtPropertyInvalid, property.ref,
                          "@property " + property.name + ": the engine does not read initial-value '" + property.initialValue + "' as a " +
                          property.syntax + "; the property is not registered",
                          "write the initial value in px, deg, s or as a color the engine reads (#rrggbb)", std::string(), property.name);
        }
    }
    return sink.take();
}
