#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StyleTypes.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class Property;
class PropertyDictionary;
struct ShorthandDefinition;

/*
    The text properties of phase 3 (p3-text, docs/html-ui/phase-3-fork-patches.md 3.10): their parsers, the parsers of
    their shorthands, and the computation of their values (called from the text sections of StyleSheetSpecification and
    ElementStyle::ComputeValue).
*/

/// Parser "text_decoration_line": 'none', or 'underline', 'overline' and 'line-through' in any order, each at most once
/// ('blink' is accepted and draws nothing, as in browsers). The value is an int of Style::TextDecoration flags.
class PropertyParserTextDecorationLine : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;
};

/// 'white-space' (CSS Text 4): a CSS 2.1 keyword ('normal', 'pre', 'nowrap', 'pre-wrap', 'pre-line', 'break-spaces'), or
/// '<white-space-collapse> || <text-wrap-mode>'; sets 'white-space-collapse' and 'text-wrap-mode'.
bool ParseWhiteSpaceShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
/// 'text-wrap': '<text-wrap-mode> || <text-wrap-style>'; the styles ('auto', 'balance', 'pretty', 'stable') are accepted
/// and wrap as 'auto' does.
bool ParseTextWrapShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
/// 'text-decoration': '<line> || <style> || <color> || <thickness>', where the line may be several keywords.
bool ParseTextDecorationShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

/// 'text-align': 'start' and 'end' are kept (ComputedValues::text_align() resolves them against the direction),
/// 'match-parent' takes the parent's value (resolved against this element's direction, as in Chromium), '-webkit-center'
/// is 'center'.
Style::TextAlign ComputeTextAlign(const Property* property, Element* element);
/// 'letter-spacing' and 'word-spacing': 'normal' is 0, a length, a percentage of the font size, or a calc() of them.
float ComputeTextSpacing(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions);

} // namespace Rml
