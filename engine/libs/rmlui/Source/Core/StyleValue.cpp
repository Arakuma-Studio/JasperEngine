#include "../../Include/RmlUi/Core/StyleValue.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "Calculation.h"
	#include "CalculationResolver.h"
#endif

namespace Rml {

StyleValue::~StyleValue() {}

RMLUI_RTTI_Define(StyleValue)

SharedPtr<const StyleValue> StyleValue::Interpolate(const StyleValue& /*to*/, float /*alpha*/, Element& /*element*/) const
{
	return nullptr;
}

Units StyleValue::GetUnitDependencies() const
{
	return Unit::UNKNOWN;
}

bool LengthPercentageValue::operator==(const LengthPercentageValue& other) const
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (calculation || other.calculation)
		return calculation && other.calculation && *calculation == *other.calculation;
#endif
	return value == other.value;
}

String LengthPercentageValue::ToString() const
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (calculation)
		return calculation->ToString();
#endif
	return Property(value.number, value.unit).ToString();
}

Units LengthPercentageValue::GetUnitDependencies() const
{
	const Units context_units = Unit::EM | Unit::REM | Unit::VW | Unit::VH | Unit::VMIN | Unit::VMAX | Unit::CH | Unit::EX | Unit::DP_SCALABLE_LENGTH;
#ifdef RMLUI_MATH_EXPRESSIONS
	if (calculation)
		return calculation->GetDependencyMask() & context_units;
#endif
	return value.unit & context_units;
}

bool ParseLengthPercentage(const String& token, LengthPercentageValue& result)
{
	PropertyParser* parser = StyleSheetSpecification::GetParser("length_percent");
	if (!parser)
		return false;
	Property property;
	if (!parser->ParseValue(property, token, ParameterMap()))
		return false;
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property.unit == Unit::CALCULATION)
	{
		result = LengthPercentageValue();
		result.calculation = property.value.Get<CalculationPtr>();
		return bool(result.calculation);
	}
#endif
	if (!Any(property.unit & Unit::LENGTH_PERCENT))
		return false;
	result = LengthPercentageValue(property.GetNumericValue());
	return true;
}

float ResolveLengthPercentage(const LengthPercentageValue& value, Element& element, float percent_basis)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (value.calculation)
	{
		float result = 0.f;
		return ResolveElementCalculation(*value.calculation, element, percent_basis, result) ? result : 0.f;
	}
#endif
	if (value.value.unit == Unit::PERCENT)
		return value.value.number * 0.01f * percent_basis;
	return element.ResolveLength(value.value);
}

namespace {
	enum class PositionKeyword { None, Left, Right, Top, Bottom, Center };

	PositionKeyword ToPositionKeyword(const String& token)
	{
		const String lower = StringUtilities::ToLower(token);
		if (lower == "left")
			return PositionKeyword::Left;
		if (lower == "right")
			return PositionKeyword::Right;
		if (lower == "top")
			return PositionKeyword::Top;
		if (lower == "bottom")
			return PositionKeyword::Bottom;
		if (lower == "center")
			return PositionKeyword::Center;
		return PositionKeyword::None;
	}

	bool IsHorizontal(PositionKeyword keyword)
	{
		return keyword == PositionKeyword::Left || keyword == PositionKeyword::Right;
	}
	bool IsVertical(PositionKeyword keyword)
	{
		return keyword == PositionKeyword::Top || keyword == PositionKeyword::Bottom;
	}

	// The position along one axis: an edge keyword (or center) with an optional offset from that edge.
	bool AxisPosition(PositionKeyword edge, const String* offset, LengthPercentageValue& result)
	{
		if (edge == PositionKeyword::Center)
		{
			if (offset)
				return false;
			result = LengthPercentageValue(50.f, Unit::PERCENT);
			return true;
		}
		const bool far_edge = (edge == PositionKeyword::Right || edge == PositionKeyword::Bottom);
		if (!offset)
		{
			result = LengthPercentageValue(far_edge ? 100.f : 0.f, Unit::PERCENT);
			return true;
		}
		LengthPercentageValue parsed;
		if (!ParseLengthPercentage(*offset, parsed))
			return false;
		if (!far_edge)
		{
			result = parsed;
			return true;
		}
#ifdef RMLUI_MATH_EXPRESSIONS
		if (!parsed.calculation && parsed.value.unit == Unit::PERCENT)
		{
			result = LengthPercentageValue(100.f - parsed.value.number, Unit::PERCENT);
			return true;
		}
		// An offset from the far edge is 100% minus it.
		return ParseLengthPercentage("calc(100% - " + *offset + ")", result);
#else
		if (parsed.value.unit != Unit::PERCENT)
			return false;
		result = LengthPercentageValue(100.f - parsed.value.number, Unit::PERCENT);
		return true;
#endif
	}
} // namespace

bool ParsePosition(const StringList& tokens, LengthPercentageValue& x, LengthPercentageValue& y)
{
	const size_t count = tokens.size();
	if (count == 0 || count > 4)
		return false;

	PositionKeyword keywords[4] = {};
	for (size_t i = 0; i < count; i++)
		keywords[i] = ToPositionKeyword(tokens[i]);

	if (count == 1)
	{
		if (keywords[0] == PositionKeyword::None)
		{
			y = LengthPercentageValue(50.f, Unit::PERCENT);
			return ParseLengthPercentage(tokens[0], x);
		}
		if (IsVertical(keywords[0]))
			return AxisPosition(PositionKeyword::Center, nullptr, x) && AxisPosition(keywords[0], nullptr, y);
		return AxisPosition(keywords[0], nullptr, x) && AxisPosition(PositionKeyword::Center, nullptr, y);
	}

	if (count == 2)
	{
		const PositionKeyword a = keywords[0], b = keywords[1];
		// Two keywords in either order, the vertical one first: 'top left', 'bottom center'.
		if (a != PositionKeyword::None && b != PositionKeyword::None && (IsVertical(a) || IsHorizontal(b)))
		{
			if (IsVertical(a) && IsVertical(b))
				return false;
			if (IsHorizontal(a) && IsHorizontal(b))
				return false;
			return AxisPosition(b, nullptr, x) && AxisPosition(a, nullptr, y);
		}
		// Otherwise the horizontal position first, each a keyword or a length-percentage: 'left 10px', '10px top'.
		if (IsVertical(a) || IsHorizontal(b))
			return false;
		const bool x_ok = (a == PositionKeyword::None ? ParseLengthPercentage(tokens[0], x) : AxisPosition(a, nullptr, x));
		const bool y_ok = (b == PositionKeyword::None ? ParseLengthPercentage(tokens[1], y) : AxisPosition(b, nullptr, y));
		return x_ok && y_ok;
	}

	// Three or four values: keywords, each followed by an optional offset ('right 10px bottom', 'left 10% top 5px').
	struct Part {
		PositionKeyword edge;
		const String* offset;
	};
	Part parts[2] = {};
	int num_parts = 0;
	for (size_t i = 0; i < count; i++)
	{
		if (keywords[i] == PositionKeyword::None)
		{
			// an offset must follow an edge keyword (not center)
			if (num_parts == 0 || parts[num_parts - 1].offset || parts[num_parts - 1].edge == PositionKeyword::Center)
				return false;
			parts[num_parts - 1].offset = &tokens[i];
		}
		else
		{
			if (num_parts == 2)
				return false;
			parts[num_parts++] = Part{keywords[i], nullptr};
		}
	}
	if (num_parts != 2)
		return false;
	// The parts in horizontal, vertical order.
	if (IsVertical(parts[0].edge) || IsHorizontal(parts[1].edge))
		std::swap(parts[0], parts[1]);
	if (IsVertical(parts[0].edge) || IsHorizontal(parts[1].edge))
		return false;
	return AxisPosition(parts[0].edge, parts[0].offset, x) && AxisPosition(parts[1].edge, parts[1].offset, y);
}

} // namespace Rml
