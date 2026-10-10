#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"

namespace Rml {

/**
    Parses the CSS Box Alignment 3 values of the align-* and justify-* properties: one keyword of the parameter list, the
    multi-word forms 'first baseline' (the 'baseline' keyword) and 'last baseline', the 'legacy left|right|center' forms
    of justify-items (the 'legacy' keyword), and 'safe' or 'unsafe' before a positional keyword: their values carry
    AlignmentSafe or AlignmentUnsafe above the keyword's (ComputedValues reads them apart). The parameter lists are made
    by MakeAlignmentKeywords.
 */
class PropertyParserAlignment : public PropertyParser {
public:
	static constexpr int AlignmentSafe = 1 << 8;
	static constexpr int AlignmentUnsafe = 2 << 8;

	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;
};

/// The parameter list of an alignment property: the keywords in order (their values 0, 1, ...; 'first' is the first
/// value), then the 'safe' and 'unsafe' forms of those that are positional (center, start, end, self-start, self-end,
/// flex-start, flex-end, left, right).
String MakeAlignmentKeywords(const char* keywords, int first = 0);

/// The parser of the 'place-content', 'place-items' and 'place-self' shorthands (ShorthandType::Custom): one value sets
/// both longhands, two values set the align longhand then the justify longhand. A single 'baseline' value sets
/// justify-content to 'start' (CSS: place-content).
bool ParsePlaceShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

} // namespace Rml
