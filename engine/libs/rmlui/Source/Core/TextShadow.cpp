#include "TextShadow.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/TypeConverter.h"

namespace Rml {

namespace {

	// A length (not a percentage): a dimension, a unitless zero or a calc() of lengths.
	bool ParseShadowLength(const String& token, LengthPercentageValue& result)
	{
		if (!ParseLengthPercentage(token, result))
			return false;
#ifdef RMLUI_MATH_EXPRESSIONS
		if (result.calculation)
			return true;
#endif
		return result.value.unit != Unit::PERCENT;
	}

	bool ParseShadowColour(const String& token, TextShadow& shadow)
	{
		if (StringUtilities::ToLower(token) == "currentcolor")
		{
			shadow.current_colour = true;
			return true;
		}
		PropertyParser* parser = StyleSheetSpecification::GetParser("color");
		Property property;
		if (!parser || !parser->ParseValue(property, token, ParameterMap()) || property.unit != Unit::COLOUR)
			return false;
		shadow.colour = property.Get<Colourb>();
		shadow.current_colour = false;
		return true;
	}

	bool ParseShadow(const String& item, TextShadow& shadow)
	{
		StringList tokens;
		SplitWhitespaceList(item, tokens);
		if (tokens.empty() || tokens.size() > 4)
			return false;

		// The lengths are given together, the colour before or after them.
		LengthPercentageValue lengths[3];
		int num_lengths = 0;
		bool have_colour = false, lengths_done = false;
		for (const String& token : tokens)
		{
			LengthPercentageValue length;
			if (ParseShadowLength(token, length))
			{
				if (lengths_done || num_lengths == 3)
					return false;
				lengths[num_lengths++] = length;
			}
			else if (!have_colour && ParseShadowColour(token, shadow))
			{
				have_colour = true;
				lengths_done = (num_lengths > 0);
			}
			else
				return false;
		}
		if (num_lengths < 2)
			return false;
		// The blur radius may not be negative.
		if (num_lengths == 3 && lengths[2].value.number < 0.f)
			return false;

		shadow.offset_x = lengths[0];
		shadow.offset_y = lengths[1];
		shadow.blur = (num_lengths == 3 ? lengths[2] : LengthPercentageValue(0.f, Unit::PX));
		if (!have_colour)
			shadow.current_colour = true;
		return true;
	}

	class PropertyParserTextShadow : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			StringList items;
			SplitCommaList(value, items);
			if (items.empty())
				return false;

			auto list = MakeShared<TextShadowListValue>();
			list->shadows.reserve(items.size());
			for (const String& item : items)
			{
				TextShadow shadow;
				if (!ParseShadow(item, shadow))
					return false;
				list->shadows.push_back(std::move(shadow));
			}

			property.value = Variant(StyleValuePtr(std::move(list)));
			property.unit = Unit::STYLE_VALUE;
			return true;
		}
	};

	float ToPixels(const LengthPercentageValue& value, Element& element)
	{
		return ResolveLengthPercentage(value, element, 0.f);
	}

	// Premultiplied interpolation, as CSS interpolates colours.
	Colourb InterpolateColour(Colourb from, Colourb to, float alpha)
	{
		const ColourbPremultiplied a = from.ToPremultiplied(), b = to.ToPremultiplied();
		auto mix = [alpha](byte x, byte y) { return (byte)Math::Clamp(Math::Round(float(x) + (float(y) - float(x)) * alpha), 0.f, 255.f); };
		const ColourbPremultiplied result(mix(a.red, b.red), mix(a.green, b.green), mix(a.blue, b.blue), mix(a.alpha, b.alpha));
		return result.ToNonPremultiplied();
	}

	LengthPercentageValue InterpolateLength(const LengthPercentageValue& from, const LengthPercentageValue& to, float alpha, Element& element)
	{
		bool same_unit = (from.value.unit == to.value.unit);
#ifdef RMLUI_MATH_EXPRESSIONS
		same_unit = same_unit && !from.calculation && !to.calculation;
#endif
		if (same_unit)
			return LengthPercentageValue(from.value.number + (to.value.number - from.value.number) * alpha, from.value.unit);
		const float a = ToPixels(from, element), b = ToPixels(to, element);
		return LengthPercentageValue(a + (b - a) * alpha, Unit::PX);
	}

} // namespace

String TextShadowListValue::ToString() const
{
	String result;
	for (const TextShadow& shadow : shadows)
	{
		if (!result.empty())
			result += ", ";
		result += shadow.offset_x.ToString() + " " + shadow.offset_y.ToString() + " " + shadow.blur.ToString();
		if (!shadow.current_colour)
		{
			String colour;
			TypeConverter<Colourb, String>::Convert(shadow.colour, colour);
			result += " " + colour;
		}
	}
	return result;
}

bool TextShadowListValue::Equals(const StyleValue& other) const
{
	const auto& list = static_cast<const TextShadowListValue&>(other);
	return shadows == list.shadows;
}

SharedPtr<const StyleValue> TextShadowListValue::Interpolate(const StyleValue& to_value, float alpha, Element& element) const
{
	// Lists of different lengths: the shorter one is padded with transparent shadows of no offset and no blur (CSS Text
	// Decoration 3 §4, interpolation of shadow lists).
	const auto& to = static_cast<const TextShadowListValue&>(to_value);
	const Colourb element_colour = element.GetComputedValues().color();
	TextShadow transparent;
	transparent.offset_x = transparent.offset_y = transparent.blur = LengthPercentageValue(0.f, Unit::PX);
	transparent.colour = Colourb(0, 0, 0, 0);
	transparent.current_colour = false;

	auto result = MakeShared<TextShadowListValue>();
	const size_t count = Math::Max(shadows.size(), to.shadows.size());
	for (size_t i = 0; i < count; i++)
	{
		const TextShadow& a = (i < shadows.size() ? shadows[i] : transparent);
		const TextShadow& b = (i < to.shadows.size() ? to.shadows[i] : transparent);
		TextShadow shadow;
		shadow.offset_x = InterpolateLength(a.offset_x, b.offset_x, alpha, element);
		shadow.offset_y = InterpolateLength(a.offset_y, b.offset_y, alpha, element);
		shadow.blur = InterpolateLength(a.blur, b.blur, alpha, element);
		if (a.current_colour && b.current_colour)
			shadow.current_colour = true;
		else
		{
			shadow.current_colour = false;
			shadow.colour = InterpolateColour(a.current_colour ? element_colour : a.colour, b.current_colour ? element_colour : b.colour, alpha);
		}
		result->shadows.push_back(std::move(shadow));
	}
	return result;
}

Units TextShadowListValue::GetUnitDependencies() const
{
	Units units = Unit::UNKNOWN;
	for (const TextShadow& shadow : shadows)
		units = units | shadow.offset_x.GetUnitDependencies() | shadow.offset_y.GetUnitDependencies() | shadow.blur.GetUnitDependencies();
	return units;
}

Vector<ResolvedTextShadow> ResolveTextShadows(Element& element)
{
	Vector<ResolvedTextShadow> result;
	const Property* property = element.GetProperty(PropertyId::TextShadow);
	if (!property || property->unit != Unit::STYLE_VALUE)
		return result;
	const StyleValuePtr value = property->value.Get<StyleValuePtr>();
	const auto* list = static_cast<const TextShadowListValue*>(value.get());
	if (!list)
		return result;

	const Colourb element_colour = element.GetComputedValues().color();
	for (const TextShadow& shadow : list->shadows)
	{
		ResolvedTextShadow resolved;
		resolved.offset = Vector2f(ToPixels(shadow.offset_x, element), ToPixels(shadow.offset_y, element));
		resolved.blur = Math::Max(ToPixels(shadow.blur, element), 0.f);
		resolved.colour = (shadow.current_colour ? element_colour : shadow.colour);
		result.push_back(resolved);
	}
	return result;
}

PropertyParser& GetTextShadowParser()
{
	static PropertyParserTextShadow parser;
	return parser;
}

} // namespace Rml
