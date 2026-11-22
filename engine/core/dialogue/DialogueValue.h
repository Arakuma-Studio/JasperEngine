// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIALOGUE_VALUE_H
#define DIALOGUE_VALUE_H

// A dialogue value (docs/specs/dialogue-graphs.md R4, R5): nothing, a bool, a number or a string.
// Types are dynamic; comparing different types is false, a condition reads its truthiness.
// A struct deriving from the variant (not an alias) so other headers can forward-declare it.

#include "Export.h"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace jasperengine::dialogue {

    struct Value : std::variant<std::monostate, bool, double, std::string> {
        using variant::variant;
        Value() = default;
        Value(int number): variant(static_cast<double>(number)) {}
        Value(int64_t number): variant(static_cast<double>(number)) {}
        Value(const char* text): variant(std::string(text ? text : "")) {}

        bool isNil() const { return index() == 0; }
        bool isBool() const { return index() == 1; }
        bool isNumber() const { return index() == 2; }
        bool isString() const { return index() == 3; }
        bool asBool() const { return std::get<1>(*this); }
        double asNumber() const { return std::get<2>(*this); }
        const std::string& asString() const { return std::get<3>(*this); }

        // false, 0, "" and nothing are false
        JASPERENGINE_API bool truthy() const;
        // the same type and value (nothing equals nothing)
        JASPERENGINE_API bool same(const Value& other) const;
        // "true", "3", "3.5", the text, "" for nothing: what a message argument and a log show
        JASPERENGINE_API std::string toText() const;
        // the JSON-like spelling: true, 3, "text", nil
        JASPERENGINE_API std::string describe() const;
        JASPERENGINE_API const char* typeName() const;
    };

}

#endif //DIALOGUE_VALUE_H
