#pragma once

#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StyleTypes.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Property;
class PropertyDictionary;
struct ShorthandDefinition;

/*
    The paint properties of phase 3 (docs/html-ui/phase-3-fork-patches.md 3.7-3.9, package p3-paint): borders, radii,
    outlines, backgrounds and object-fit. The registrations are in the paint section of
    StyleSheetSpecification::RegisterDefaultProperties; their parsers and computed values are here.
*/

// 'border': width, style and color in any order, set on all four sides; resets 'border-image'.
bool ParseBorderShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

// 'border-<corner>-radius': the horizontal radius and the vertical one (the same when omitted).
bool ParseCornerRadiusShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
// 'border-radius': one to four horizontal radii (top-left, top-right, bottom-right, bottom-left, as a box shorthand
// repeats them) and after a '/' as many vertical ones (the horizontal ones when omitted).
bool ParseBorderRadiusShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

// A CSS <position> ('left 10px top', 'right 20% bottom 5px', 'center', '30px 40%', ...) into the shorthand's two items:
// the horizontal and the vertical offset as lengths or percentages (an offset from the right or bottom edge is a calc()).
bool ParsePositionShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
// The same, into the CSS text of the two axes.
bool ParsePosition(const String& value, String& out_x, String& out_y);

class PropertyParser;

// The parser of border and outline widths ("line_width"): thin, medium and thick are the lengths 1px, 3px and 5px (CSS
// computes them so, and they interpolate with lengths); else a length.
PropertyParser* GetLineWidthParser();

// The computed width of a border or outline: the keywords thin, medium and thick are 1px, 3px and 5px; a length is
// snapped as Chromium snaps border widths (whole pixels, rounded down, a width between 0 and 1px is 1px).
float ComputeLineWidth(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions);

} // namespace Rml
