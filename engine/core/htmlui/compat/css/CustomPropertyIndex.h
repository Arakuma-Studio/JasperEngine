// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CUSTOMPROPERTYINDEX_H
#define COMPAT_CSS_CUSTOMPROPERTYINDEX_H

// Every custom property declaration of a document (phase-2 §3.9.8): name -> its declared
// values (normalized), so the rewriter can type a var() where RCSS needs a different syntax
// than CSS (a gradient used as a background becomes decorator syntax), resolve static values
// for calc(), and write typed copies (--x--decorator) when one variable serves two kinds.

#include "Export.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/css/CssAst.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    class JASPERENGINE_API CustomPropertyIndex {
    public:
        struct Entry {
            ValueList value;   // normalized
            SourceRef ref;                       // the declaration
            uint32_t rule = 0;                   // FlatRule index + 1, 0 for a style attribute
        };

        void clear();
        void add(const std::string& name, Entry entry);
        // changes with every clear() and add() (results computed from the index can be kept
        // while it is the same)
        uint32_t revision() const { return revisionCount; }
        const std::vector<Entry>* find(const std::string& name) const;
        bool declared(const std::string& name) const { return find(name) != nullptr; }

        // The value every declaration of the variable shares, with nested var() resolved
        // (depth 8); nullopt when undeclared, declared with different values, or not static.
        std::optional<ValueList> staticValue(const std::string& name) const;
        // The var()'s static value, or its fallback when the variable is undeclared.
        bool resolve(const ComponentValue& var, ValueList& out) const;

        // Every declared value with nested var() replaced (each variable's declarations
        // expanded), up to 32 alternatives; false when a nested variable is undeclared.
        bool expandedValues(const std::string& name, std::vector<ValueList>& out) const;

        // ---- uses by structured rewrites (typed copies)
        // family: "decorator", "mask", "font-effect", "transition", "animation", "font-family",
        // "filter", "easing"; raw: a use where the value is written as declared
        void addUse(const std::string& name, const std::string& family);
        void addRawUse(const std::string& name);
        // the families a variable is typed for; inPlace: one family and no raw use
        std::set<std::string> families(const std::string& name) const;
        bool inPlace(const std::string& name) const;
        // how a use of the variable is written for a family: "--x" or "--x--family"
        std::string typedName(const std::string& name, const std::string& family) const;

        // A variable the engine sets on each document at runtime (env(safe-area-inset-*) is
        // var(--jasper-safe-area-inset-*), UIDocumentSystem sets it): it has no static value and is
        // never folded, so it reaches the fork as var() and follows the live value
        static bool isDynamic(const std::string& name);
        // the value a rewrite that must fold one statically uses for a dynamic variable (0px)
        static ValueList dynamicStaticValue();

        // the var() name and fallback of a var() function ("" when malformed)
        static std::string varName(const ComponentValue& var, ValueList* fallback = nullptr, bool* hasFallback = nullptr);
        // the variable names a value references (var(--x) anywhere in it)
        static void referencedVariables(const ValueList& value, std::vector<std::string>& names);

    private:
        std::map<std::string, std::vector<Entry>> entries;
        std::map<std::string, std::set<std::string>> uses;
        std::set<std::string> rawUses;
        uint32_t revisionCount = 0;
    };

}

#endif //COMPAT_CSS_CUSTOMPROPERTYINDEX_H
