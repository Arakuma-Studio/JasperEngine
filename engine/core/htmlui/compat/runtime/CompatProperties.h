// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_COMPATPROPERTIES_H
#define COMPAT_RUNTIME_COMPATPROPERTIES_H

#include "Export.h"

#include <RmlUi/Core/ID.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
    class Element;
    class Property;
}

namespace jasperengine::htmlui::compat {

    // The CSS properties RmlUi 6.3 lacks that compat runtime code reads (phase-2 §3.11.1).
    enum class CompatProperty : uint8_t {
        ListStyleType,       // list-style-type: keyword list + string (markers)
        ListStylePosition,   // list-style-position: inside | outside
        ListStyleImage,      // list-style-image: none | "<path>"
        ObjectFit,           // object-fit: fill | contain | cover | none | scale-down (img)
        ObjectPosition,      // object-position: the CSS value as a string, "50% 50%" (img)
        ImageRendering,      // image-rendering: auto | smooth | high-quality | pixelated | crisp-edges
        AnimationFill,       // -html-animation-fill: none | forwards | backwards | both (AnimationFill)
        Fill,                // fill: <color> | none | currentcolor (<svg>)
        Stroke,              // stroke: <color> | none | currentcolor
        StrokeWidth,         // stroke-width: <number> | <length> | <percentage>
        StrokeOpacity,       // stroke-opacity: <number> | <percentage>
        FillOpacity,         // fill-opacity: <number> | <percentage>
        StrokeLinecap,       // stroke-linecap: butt | round | square
        StrokeLinejoin,      // stroke-linejoin: miter | round | bevel | miter-clip | arcs
        StrokeDasharray,     // stroke-dasharray: none | the list as a string
        Count
    };

    // Registers them with Rml::StyleSheetSpecification::RegisterProperty right after
    // Rml::Initialise (registerRuntime), before any style sheet is parsed. A name the fork
    // already registers (StyleSheetSpecification::GetProperty or GetShorthand non-null: phase 3
    // adds list-style-*, object-fit and object-position) is left to the fork; the readers
    // below then read the fork's property, by name. The SVG paint properties default to
    // "unset" (no value: the SVG's own paint applies).
    class JASPERENGINE_INTERNAL CompatProperties {
    public:
        static void registerAll();          // idempotent while RmlUi stays initialised
        static void reset();                // after Rml::Shutdown (the specification is gone)
        static bool isRegistered();         // registerAll ran for this initialisation

        static const char* name(CompatProperty property);
        // the id RmlUi knows the property by (compat's or the fork's); Invalid when neither
        static Rml::PropertyId id(CompatProperty property);
        // registered by compat in this initialisation (false: the fork has it, or not registered)
        static bool registeredByCompat(CompatProperty property);
        static std::vector<std::string> registeredNames();

        // The element's computed value: nullptr when RmlUi has no such property; a property
        // with Rml::Unit::UNKNOWN is "unset" (the SVG paint properties' default)
        static const Rml::Property* get(Rml::Element& element, CompatProperty property);
        // the computed value as CSS text (keywords by name, strings as written); "" when unset
        static std::string text(Rml::Element& element, CompatProperty property);
        // a color value; false when unset or not a color (a keyword)
        static bool color(Rml::Element& element, CompatProperty property, Rml::Colourb& out);
    };

}

#endif //COMPAT_RUNTIME_COMPATPROPERTIES_H
