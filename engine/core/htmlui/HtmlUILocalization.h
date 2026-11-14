// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_LOCALIZATION_H
#define HTMLUI_LOCALIZATION_H

// Contract (docs/specs/S95-lua-runtime-gaps.md REQ-28, D-13)
// - Purpose: String Tables in HTML UI documents. An element with `data-loc="key"` shows
//   Localization::get(key, args) as its text (escaped: a translation is never markup), its args
//   from `data-loc-args` (JSON: an object of named arguments, a list of positional ones, or one
//   value). The text is set when the document loads (before its scripts' onLoad) and again on
//   every Localization::generation() change (language, locales, tables).
// - setLocalized writes both attributes and the text now, so the element keeps following the
//   language: Lua's el:setLocalized(key [, args]).
// - An element whose text already is the value is not touched (no layout change).
// - Thread-safety: main thread (RmlUi).

#include "localization/Localization.h"

#include <string>

namespace Rml { class Element; }

namespace jasperengine{

    class HtmlUILocalization{
    public:
        static constexpr const char* KEY_ATTRIBUTE = "data-loc";
        static constexpr const char* ARGS_ATTRIBUTE = "data-loc-args";

        // every element of `root`'s tree (root too) with data-loc gets its text; how many have one
        static int apply(Rml::Element* root);
        static void setLocalized(Rml::Element* element, const std::string& key, const LocalizationArgs& args);
    };

}

#endif //HTMLUI_LOCALIZATION_H
