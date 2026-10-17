// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_COMPATATPROPERTYTYPES_H
#define COMPAT_COMPATATPROPERTYTYPES_H

// The registrations of @property rules a compile hands to the document load (CompatAtProperty.h,
// phase-5 §3.11): kept apart so that the rewrite result and the compiled document carry them
// without the compat session's declarations.

#include "htmlui/compat/CompatTypes.h"

#include <cstdint>
#include <string>

namespace jasperengine::htmlui::compat {

    // The syntaxes the fork registers, in the order of Rml::RegisteredPropertySyntax.
    enum class AtPropertySyntax : uint8_t { Universal, Number, Integer, Length, Percentage, LengthPercentage, Color, Angle, Time };

    // One registration of a document.
    struct RegisteredProperty {
        std::string name;                 // "--spin"
        std::string syntax;               // the syntax string, unquoted: "<angle>", "*"
        AtPropertySyntax type = AtPropertySyntax::Universal;
        bool inherits = true;
        bool hasInitialValue = false;     // false only for "*" without initial-value
        std::string initialValue;         // as the fork parses it (RCSS), "" without one
        SourceRef ref;                    // the rule (name to block)
    };

}

#endif //COMPAT_COMPATATPROPERTYTYPES_H
