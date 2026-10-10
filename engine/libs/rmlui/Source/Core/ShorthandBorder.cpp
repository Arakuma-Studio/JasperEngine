#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "ComputeProperty.h"
#include "PaintProperties.h"
#include "PropertyShorthandDefinition.h"

namespace Rml {

bool ParseBorderShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& /*definition*/, const String& value,
	const PropertySpecification& specification)
{
	// CSS Backgrounds 3: border = <line-width> || <line-style> || <color>, each at most once, set on all four sides.
	StringList words;
	SplitWhitespaceList(value, words);
	if (words.empty() || words.size() > 3)
		return false;

	static const PropertyId parts[3] = {PropertyId::BorderTopWidth, PropertyId::BorderTopStyle, PropertyId::BorderTopColor};
	PropertyDictionary parsed;
	bool assigned[3] = {};
	for (const String& word : words)
	{
		bool matched = false;
		for (int i = 0; i < 3 && !matched; i++)
		{
			if (assigned[i])
				continue;
			if (specification.ParsePropertyDeclaration(parsed, parts[i], word))
				matched = assigned[i] = true;
		}
		if (!matched)
			return false;
	}

	static const PropertyId sides[3][4] = {
		{PropertyId::BorderTopWidth, PropertyId::BorderRightWidth, PropertyId::BorderBottomWidth, PropertyId::BorderLeftWidth},
		{PropertyId::BorderTopStyle, PropertyId::BorderRightStyle, PropertyId::BorderBottomStyle, PropertyId::BorderLeftStyle},
		{PropertyId::BorderTopColor, PropertyId::BorderRightColor, PropertyId::BorderBottomColor, PropertyId::BorderLeftColor},
	};
	for (int i = 0; i < 3; i++)
	{
		const PropertyDefinition* definition = specification.GetProperty(parts[i]);
		const Property* property = parsed.GetProperty(parts[i]);
		Property initial;
		if (!property)
		{
			// An omitted part takes its initial value.
			if (!definition || !definition->GetDefaultValue())
				return false;
			initial = *definition->GetDefaultValue();
			property = &initial;
		}
		for (PropertyId side : sides[i])
			dictionary.SetProperty(side, *property);
	}

	// The shorthand also resets 'border-image' to its initial value.
	if (const PropertyDefinition* border_image = specification.GetProperty(PropertyId::BorderImage))
	{
		if (const Property* initial = border_image->GetDefaultValue())
			dictionary.SetProperty(PropertyId::BorderImage, *initial);
	}
	return true;
}

float ComputeLineWidth(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions)
{
	if (property->unit == Unit::KEYWORD)
	{
		// thin, medium, thick
		switch (property->Get<int>())
		{
		case 0: return 1.f;
		case 2: return 5.f;
		default: return 3.f;
		}
	}

	float width = 0.f;
	if (!ComputeDefiniteLength(property, font_size, document_font_size, dp_ratio, vp_dimensions, width) || width <= 0.f)
		return 0.f;
	if (width < 1.f)
		return 1.f;
	// Rounded down in Chromium's layout units (1/64 px), so that a width of 3px computed from 0.1875em stays 3px.
	return float(int(width + 1.f / 128.f));
}

} // namespace Rml
