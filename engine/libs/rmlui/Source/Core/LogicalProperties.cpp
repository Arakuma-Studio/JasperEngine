#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "PropertyShorthandDefinition.h"
#include "ValuesShorthands.h"

namespace Rml {

// Sets one physical item, a property or a shorthand, from a value.
static bool ParseItem(PropertyDictionary& dictionary, const ShorthandItem& item, const String& value, const PropertySpecification& specification)
{
	if (item.type == ShorthandItemType::Property)
		return specification.ParsePropertyDeclaration(dictionary, item.property_id, value);
	if (item.type == ShorthandItemType::Shorthand)
		return specification.ParseShorthandDeclaration(dictionary, item.shorthand_id, value);
	return false;
}

// '<start> <end>?': the start item takes the first value, the end item the second or the first.
static bool ParseLogicalPair(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	if (definition.items.size() != 2)
		return false;
	StringList values;
	SplitWhitespaceList(value, values);
	if (values.empty() || values.size() > 2)
		return false;
	PropertyDictionary parsed;
	if (!ParseItem(parsed, definition.items[0], values[0], specification) ||
		!ParseItem(parsed, definition.items[1], values.size() == 2 ? values[1] : values[0], specification))
		return false;
	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

// The whole value to each item: 'border-inline: 1px solid red' is 'border-left' and 'border-right' with it.
static bool ParseLogicalEach(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	if (definition.items.empty())
		return false;
	PropertyDictionary parsed;
	for (const ShorthandItem& item : definition.items)
		if (!ParseItem(parsed, item, value, specification))
			return false;
	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

// The border styles: the physical longhands belong to the paint package (border-*-style), registered after this table,
// so these shorthands name them by id when they parse (and fail while they are not registered).
static bool ParseLogicalBorderStyle(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	PropertyId start = PropertyId::Invalid, end = PropertyId::Invalid;
	switch (definition.id)
	{
	case ShorthandId::BorderInlineStyle:
		start = PropertyId::BorderLeftStyle;
		end = PropertyId::BorderRightStyle;
		break;
	case ShorthandId::BorderBlockStyle:
		start = PropertyId::BorderTopStyle;
		end = PropertyId::BorderBottomStyle;
		break;
	case ShorthandId::BorderInlineStartStyle: start = PropertyId::BorderLeftStyle; break;
	case ShorthandId::BorderInlineEndStyle: start = PropertyId::BorderRightStyle; break;
	case ShorthandId::BorderBlockStartStyle: start = PropertyId::BorderTopStyle; break;
	case ShorthandId::BorderBlockEndStyle: start = PropertyId::BorderBottomStyle; break;
	default: return false;
	}
	StringList values;
	SplitWhitespaceList(value, values);
	if (values.empty() || values.size() > (end == PropertyId::Invalid ? 1u : 2u))
		return false;
	PropertyDictionary parsed;
	if (!specification.ParsePropertyDeclaration(parsed, start, values[0]))
		return false;
	if (end != PropertyId::Invalid && !specification.ParsePropertyDeclaration(parsed, end, values.size() == 2 ? values[1] : values[0]))
		return false;
	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

static const LogicalShorthand logical_shorthands[] = {
	{ShorthandId::MarginInline, "margin-inline", "margin-left, margin-right", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::MarginInlineStart, "margin-inline-start", "margin-left", ShorthandType::FallThrough, nullptr},
	{ShorthandId::MarginInlineEnd, "margin-inline-end", "margin-right", ShorthandType::FallThrough, nullptr},
	{ShorthandId::MarginBlock, "margin-block", "margin-top, margin-bottom", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::MarginBlockStart, "margin-block-start", "margin-top", ShorthandType::FallThrough, nullptr},
	{ShorthandId::MarginBlockEnd, "margin-block-end", "margin-bottom", ShorthandType::FallThrough, nullptr},
	{ShorthandId::PaddingInline, "padding-inline", "padding-left, padding-right", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::PaddingInlineStart, "padding-inline-start", "padding-left", ShorthandType::FallThrough, nullptr},
	{ShorthandId::PaddingInlineEnd, "padding-inline-end", "padding-right", ShorthandType::FallThrough, nullptr},
	{ShorthandId::PaddingBlock, "padding-block", "padding-top, padding-bottom", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::PaddingBlockStart, "padding-block-start", "padding-top", ShorthandType::FallThrough, nullptr},
	{ShorthandId::PaddingBlockEnd, "padding-block-end", "padding-bottom", ShorthandType::FallThrough, nullptr},
	{ShorthandId::InsetInline, "inset-inline", "left, right", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::InsetInlineStart, "inset-inline-start", "left", ShorthandType::FallThrough, nullptr},
	{ShorthandId::InsetInlineEnd, "inset-inline-end", "right", ShorthandType::FallThrough, nullptr},
	{ShorthandId::InsetBlock, "inset-block", "top, bottom", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::InsetBlockStart, "inset-block-start", "top", ShorthandType::FallThrough, nullptr},
	{ShorthandId::InsetBlockEnd, "inset-block-end", "bottom", ShorthandType::FallThrough, nullptr},

	{ShorthandId::BorderInline, "border-inline", "border-left, border-right", ShorthandType::Custom, &ParseLogicalEach},
	{ShorthandId::BorderInlineWidth, "border-inline-width", "border-left-width, border-right-width", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::BorderInlineStyle, "border-inline-style", "", ShorthandType::Custom, &ParseLogicalBorderStyle},
	{ShorthandId::BorderInlineColor, "border-inline-color", "border-left-color, border-right-color", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::BorderInlineStart, "border-inline-start", "border-left", ShorthandType::Custom, &ParseLogicalEach},
	{ShorthandId::BorderInlineStartWidth, "border-inline-start-width", "border-left-width", ShorthandType::FallThrough, nullptr},
	{ShorthandId::BorderInlineStartStyle, "border-inline-start-style", "", ShorthandType::Custom, &ParseLogicalBorderStyle},
	{ShorthandId::BorderInlineStartColor, "border-inline-start-color", "border-left-color", ShorthandType::FallThrough, nullptr},
	{ShorthandId::BorderInlineEnd, "border-inline-end", "border-right", ShorthandType::Custom, &ParseLogicalEach},
	{ShorthandId::BorderInlineEndWidth, "border-inline-end-width", "border-right-width", ShorthandType::FallThrough, nullptr},
	{ShorthandId::BorderInlineEndStyle, "border-inline-end-style", "", ShorthandType::Custom, &ParseLogicalBorderStyle},
	{ShorthandId::BorderInlineEndColor, "border-inline-end-color", "border-right-color", ShorthandType::FallThrough, nullptr},
	{ShorthandId::BorderBlock, "border-block", "border-top, border-bottom", ShorthandType::Custom, &ParseLogicalEach},
	{ShorthandId::BorderBlockWidth, "border-block-width", "border-top-width, border-bottom-width", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::BorderBlockStyle, "border-block-style", "", ShorthandType::Custom, &ParseLogicalBorderStyle},
	{ShorthandId::BorderBlockColor, "border-block-color", "border-top-color, border-bottom-color", ShorthandType::Custom, &ParseLogicalPair},
	{ShorthandId::BorderBlockStart, "border-block-start", "border-top", ShorthandType::Custom, &ParseLogicalEach},
	{ShorthandId::BorderBlockStartWidth, "border-block-start-width", "border-top-width", ShorthandType::FallThrough, nullptr},
	{ShorthandId::BorderBlockStartStyle, "border-block-start-style", "", ShorthandType::Custom, &ParseLogicalBorderStyle},
	{ShorthandId::BorderBlockStartColor, "border-block-start-color", "border-top-color", ShorthandType::FallThrough, nullptr},
	{ShorthandId::BorderBlockEnd, "border-block-end", "border-bottom", ShorthandType::Custom, &ParseLogicalEach},
	{ShorthandId::BorderBlockEndWidth, "border-block-end-width", "border-bottom-width", ShorthandType::FallThrough, nullptr},
	{ShorthandId::BorderBlockEndStyle, "border-block-end-style", "", ShorthandType::Custom, &ParseLogicalBorderStyle},
	{ShorthandId::BorderBlockEndColor, "border-block-end-color", "border-bottom-color", ShorthandType::FallThrough, nullptr},

	{ShorthandId::InlineSize, "inline-size", "width", ShorthandType::FallThrough, nullptr},
	{ShorthandId::BlockSize, "block-size", "height", ShorthandType::FallThrough, nullptr},
	{ShorthandId::MinInlineSize, "min-inline-size", "min-width", ShorthandType::FallThrough, nullptr},
	{ShorthandId::MaxInlineSize, "max-inline-size", "max-width", ShorthandType::FallThrough, nullptr},
	{ShorthandId::MinBlockSize, "min-block-size", "min-height", ShorthandType::FallThrough, nullptr},
	{ShorthandId::MaxBlockSize, "max-block-size", "max-height", ShorthandType::FallThrough, nullptr},
};

const LogicalShorthand* GetLogicalShorthands(size_t& count)
{
	count = sizeof(logical_shorthands) / sizeof(logical_shorthands[0]);
	return logical_shorthands;
}

} // namespace Rml
