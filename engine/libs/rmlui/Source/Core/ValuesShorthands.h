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

// transform-origin: a one- or two-value <position> (keywords in either order, 'top left') and an optional z length;
// perspective-origin: a <position> of one, two or four values (edge offsets 'right 10px bottom 20%'). In
// ShorthandOrigin.cpp.
bool ParseTransformOriginShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
bool ParsePerspectiveOriginShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

// The logical properties of CSS Logical Properties 1 as shorthands of the physical ones, for a horizontal-tb, ltr
// writing mode (inline = left/right, block = top/bottom; layout is never mirrored, phase-3 spec 2.2). In
// LogicalProperties.cpp; StyleSheetSpecification registers the table in its values section.
struct LogicalShorthand {
	ShorthandId id;
	const char* name;
	const char* items; // the physical longhands or shorthands, start then end
	ShorthandType type;
	ShorthandParserFunction parser; // set for the Custom ones
};
const LogicalShorthand* GetLogicalShorthands(size_t& count);

} // namespace Rml
