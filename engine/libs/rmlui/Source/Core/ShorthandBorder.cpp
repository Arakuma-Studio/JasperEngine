#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
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

bool ParseCornerRadiusShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	if (definition.items.size() != 2 || definition.items[0].type != ShorthandItemType::Property ||
		definition.items[1].type != ShorthandItemType::Property)
		return false;
	StringList words;
	SplitWhitespaceList(value, words);
	if (words.empty() || words.size() > 2)
		return false;
	PropertyDictionary parsed;
	if (!specification.ParsePropertyDeclaration(parsed, definition.items[0].property_id, words[0]) ||
		!specification.ParsePropertyDeclaration(parsed, definition.items[1].property_id, words.back()))
		return false;
	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

bool ParseBorderRadiusShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	if (definition.items.size() != 4)
		return false;

	// The horizontal radii, and after a '/' outside of parentheses (calc() divides with it too) the vertical ones.
	String horizontal, vertical;
	bool has_vertical = false;
	int depth = 0;
	for (char c : value)
	{
		if (c == '(')
			depth++;
		else if (c == ')')
			depth--;
		if (c == '/' && depth == 0)
		{
			if (has_vertical)
				return false;
			has_vertical = true;
			continue;
		}
		(has_vertical ? vertical : horizontal) += c;
	}

	// One to four values: top-left, top-right, bottom-right, bottom-left; omitted ones as in a box shorthand.
	auto expand = [](const String& list, String corners[4]) {
		StringList words;
		SplitWhitespaceList(list, words);
		if (words.empty() || words.size() > 4)
			return false;
		corners[0] = words[0];
		corners[1] = (words.size() > 1 ? words[1] : words[0]);
		corners[2] = (words.size() > 2 ? words[2] : words[0]);
		corners[3] = (words.size() > 3 ? words[3] : corners[1]);
		return true;
	};
	String x[4], y[4];
	if (!expand(horizontal, x))
		return false;
	if (has_vertical)
	{
		if (!expand(vertical, y))
			return false;
	}
	else
	{
		for (int i = 0; i < 4; i++)
			y[i] = x[i];
	}

	PropertyDictionary parsed;
	for (int corner = 0; corner < 4; corner++)
	{
		const ShorthandItem& item = definition.items[corner];
		if (item.type != ShorthandItemType::Shorthand || !item.shorthand_definition || item.shorthand_definition->items.size() != 2)
			return false;
		if (!specification.ParsePropertyDeclaration(parsed, item.shorthand_definition->items[0].property_id, x[corner]) ||
			!specification.ParsePropertyDeclaration(parsed, item.shorthand_definition->items[1].property_id, y[corner]))
			return false;
	}
	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

namespace {
	class PropertyParserLineWidth : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override
		{
			const String keyword = StringUtilities::ToLower(StringUtilities::StripWhitespace(value));
			const float width = (keyword == "thin" ? 1.f : keyword == "medium" ? 3.f : keyword == "thick" ? 5.f : -1.f);
			if (width >= 0.f)
			{
				property.value = Variant(width);
				property.unit = Unit::PX;
				return true;
			}
			PropertyParser* length = StyleSheetSpecification::GetParser("length");
			return length && length->ParseValue(property, value, parameters);
		}
	};
} // namespace

PropertyParser* GetLineWidthParser()
{
	static PropertyParserLineWidth parser;
	return &parser;
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
