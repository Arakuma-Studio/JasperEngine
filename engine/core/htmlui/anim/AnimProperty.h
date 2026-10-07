// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMPROPERTY_H
#define ANIMPROPERTY_H

// The HTML UI element side of the anime API: element targets reading and writing RmlUi
// properties (Anime.js 4.5.0 core/values.js getOriginalAnimatableValue, core/render.js
// setters, core/transforms.js, core/units.js, core/styles.js revertValues).
//
// - Individual transforms (translateX/Y/Z, rotate, rotateX/Y/Z, scale, scaleX/Y/Z, skew,
//   skewX/Y, perspective; x/y/z for translate) keep a per-element state and write one
//   Rml::Transform of Rml::TransformPrimitives in Anime's order (perspective, translate,
//   rotate, scale, skew) to PropertyId::Transform once per render pass (flush()).
// - opacity, single lengths and colours are written as typed Rml::Property values; custom
//   properties (--name) and everything else as text through Element::SetProperty.
// - Transform and opacity writes never dirty the layout.

#include "Export.h"
#include "AnimTargets.h"

#include <cstddef>

namespace Rml {
    class Element;
}

namespace jasperengine {

    class JASPERENGINE_INTERNAL AnimProperty {
    public:
        // the shared target of an element (nullptr for nullptr)
        static AnimTargetPtr elementTarget(Rml::Element* element);
        // the element of an element target (nullptr when gone or not an element target)
        static Rml::Element* getElement(const AnimTarget& target);
        // writes the transforms of the elements touched since the last flush (end of a top-level render)
        static void flush();
        // live element targets (tests)
        static size_t getElementTargetCount();
    };

}

#endif //ANIMPROPERTY_H
