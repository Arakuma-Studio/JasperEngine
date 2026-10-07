// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIVALUE_H
#define UIVALUE_H

#include "Export.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine{

    // A JSON-like value tree: the storage of data models (UIDataModel), event details and
    // the Lua conversion (defined by p1-lua, UIValue.cpp)
    class JASPERENGINE_API UIValue {
    public:
        enum class Type { NIL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

        UIValue(); UIValue(bool); UIValue(double); UIValue(int); UIValue(const char*); UIValue(std::string);
        static UIValue array(); static UIValue object();

        Type getType() const; bool isNil() const;
        bool asBool() const; double asNumber() const; const std::string& asString() const;
        size_t size() const;                                   // ARRAY/OBJECT
        UIValue& operator[](size_t);  UIValue& operator[](const std::string&);   // auto-vivify
        const UIValue* find(const std::string&) const;
        void push(UIValue); void erase(size_t); void erase(const std::string&);
        std::vector<std::string> keys() const;                 // insertion order

    private:
        Type type = Type::NIL;
        bool boolValue = false;
        double numberValue = 0.0;
        std::string stringValue;
        std::vector<UIValue> items;          // ARRAY elements, or OBJECT values (parallel to itemKeys)
        std::vector<std::string> itemKeys;   // OBJECT keys, insertion order
    };

}

#endif //UIVALUE_H
