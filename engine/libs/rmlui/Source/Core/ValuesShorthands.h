#pragma once

#include "../../Include/RmlUi/Core/PropertySpecification.h"

namespace Rml {

struct ShorthandDefinition;

// The Custom shorthands of the values package (phase 3, docs/html-ui/phase-3-fork-patches.md 3.3), registered by
// StyleSheetSpecification in its values section. Each sets every longhand of its definition, the omitted ones to their
// initial values, and fails without changing anything on an invalid value.

// font: [<style> || <variant> || <weight> || <stretch>]? <size> [/ <line-height>]? <family>, in ShorthandFont.cpp. The
// variant and the stretch take 'normal' only; system fonts (caption, menu, ...) are not supported.
bool ParseFontShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

} // namespace Rml

namespace Rml {

// transform-origin: a one- or two-value <position> (keywords in either order, 'top left') and an optional z length;
// perspective-origin: a <position> of one to four values (edge offsets 'right 10px bottom 20%'). In ShorthandOrigin.cpp.
bool ParseTransformOriginShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
bool ParsePerspectiveOriginShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

} // namespace Rml
