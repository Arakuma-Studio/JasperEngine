// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_COMPATATPROPERTY_H
#define COMPAT_COMPATATPROPERTY_H

// Registered custom properties (phase-5 §3.11, p5-property): `@property --x { syntax; inherits;
// initial-value }` with ForkFeature::RegisteredProperties. The compile reads every @property of
// the document (hoisted: a rule after the declarations that use the name registers it too; the
// last valid rule of a name wins, as in Chromium), reports invalid ones and drops them, and hands
// a syntax the fork does not register back to phase 2's root-rule emulation. The document load
// registers the rest with the fork (Rml::StyleSheetSpecification::RegisterCustomProperty) before
// RmlUi parses the sheets, so declarations, transitions and keyframes of the names are typed.
// Registrations are process-global: the first one of a name wins until RmlUi shuts down.

#include "Export.h"
#include "htmlui/compat/CompatAtPropertyTypes.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/css/CssAst.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    struct Session;
    struct MediaResult;
    class SourceFiles;   // SourceFiles.h

    class JASPERENGINE_API CompatAtProperty {
    public:
        // The flatten pass's @property branch (CssRewriter), with ForkFeature::RegisteredProperties:
        // Registered (recorded in the session), Dropped (invalid: HTMLUI-AT at-property-invalid, or
        // nested in a style rule, or inside a media query the viewport does not match) or Fallback
        // (a syntax the fork does not register: at-property-unsupported-syntax, and the caller
        // writes phase 2's root rule).
        enum class Outcome : uint8_t { Registered, Dropped, Fallback };
        static Outcome take(Session& session, const Rule& rule, bool nestedInStyleRule, const MediaResult& media);
        // After the flatten pass: the registrations into the rewrite result, their initial values
        // into the session's custom property index (the names are declared).
        static void finish(Session& session);

        // The session's registration of a custom property name (nullptr: not registered).
        static const RegisteredProperty* find(const Session& session, std::string_view name);

        // The syntax string of a syntax descriptor (unquoted): the syntax when the fork registers
        // it; nullopt with `valid` false when it is not a valid CSS syntax (the rule is invalid),
        // with `valid` true for a valid one the fork does not register ('<length> | auto',
        // '<length>+', '<image>', ...), which compat hands back to the emulation.
        static std::optional<AtPropertySyntax> parseSyntax(std::string_view syntax, bool& valid);
        static const char* syntaxName(AtPropertySyntax syntax);   // "<length>", "*"

        // Static check of a declared value (or, with `initial`, of an initial-value, which must
        // also be computationally independent and have no var()): a value with var() matches
        // (it is checked when substituted).
        static bool matches(AtPropertySyntax syntax, const ValueList& value, bool initial = false);

        // Registers a compile's properties with the fork (needs RmlUi initialised; nothing
        // otherwise): a name registered before with another definition, a registration without
        // a free property id and an initial value the fork does not parse come back as
        // diagnostics located in `files`.
        static std::vector<Diagnostic> registerAll(const std::vector<RegisteredProperty>& properties, const SourceFiles& files);
    };

}

#endif //COMPAT_COMPATATPROPERTY_H
