#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "Calculation.h"
#endif

namespace Rml {

Property::Property() : unit(Unit::UNKNOWN), specificity(-1)
{
	definition = nullptr;
	parser_index = -1;
}

String Property::ToString() const
{
	if (important)
	{
		Property normal = *this;
		normal.important = false;
		return normal.ToString() + " !important";
	}
	if (unit == Unit::CSS_WIDE_KEYWORD)
	{
		switch (CssWideKeyword(value.Get<int>()))
		{
		case CssWideKeyword::Inherit: return "inherit";
		case CssWideKeyword::Initial: return "initial";
		case CssWideKeyword::Unset: return "unset";
		case CssWideKeyword::Revert: return "revert";
		}
		return String();
	}
#ifdef RMLUI_MATH_EXPRESSIONS
	if (unit == Unit::CALCULATION)
	{
		const CalculationPtr calculation = value.Get<CalculationPtr>();
		return calculation ? calculation->ToString() : String();
	}
#endif
	if (unit == Unit::STYLE_VALUE)
	{
		const StyleValuePtr style_value = value.Get<StyleValuePtr>();
		return style_value ? style_value->ToString() : String();
	}
	if (!definition)
		return value.Get<String>() + Rml::ToString(unit);

	String string;
	definition->GetValue(string, *this);
	return string;
}

NumericValue Property::GetNumericValue() const
{
	NumericValue result;
	if (Any(unit & Unit::NUMERIC))
	{
		if (value.GetInto(result.number))
			result.unit = unit;
	}
	return result;
}

} // namespace Rml
