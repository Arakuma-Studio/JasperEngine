// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NUMBERTEXT_H
#define NUMBERTEXT_H

#include "Export.h"
#include <string>

// Numbers as text the same way in every locale: '.' is the decimal point whatever LC_NUMERIC
// says (a German locale writes "0,5" with printf), so a file written on one device reads back
// on any other. Not std::to_chars / from_chars for floating point: Apple's libc++ only has them
// from macOS 13.3 / iOS 16.3.
namespace jasperengine::NumberText {

    // The shortest text that reads back as the same value ("0.1", "3", "1e+20"); "nan", "inf"
    // and "-inf" for the values that are not finite
    JASPERENGINE_API std::string formatFloat(float value);
    JASPERENGINE_API std::string formatDouble(double value);

    // The float as the double of its shortest text (0.1f -> 0.1, not 0.10000000149011612)
    JASPERENGINE_API double floatToDouble(float value);

    // The whole text as a number: optional sign, digits, '.', exponent; "nan", "inf",
    // "infinity" (any case, with a sign) too. False on anything else (spaces, a ',' decimal
    // point, hex, an empty text); parseFloat is also false out of the float range.
    JASPERENGINE_API bool parseDouble(const std::string& text, double& out);
    JASPERENGINE_API bool parseFloat(const std::string& text, float& out);
    // Decimal digits with an optional sign, within long long
    JASPERENGINE_API bool parseInteger(const std::string& text, long long& out);

}

#endif // NUMBERTEXT_H
