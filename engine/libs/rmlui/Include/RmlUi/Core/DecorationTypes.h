#pragma once

#include "NumericValue.h"
#include "Types.h"

namespace Rml {

struct ColorStop {
	ColourbPremultiplied color;
	NumericValue position;
	bool current_color = false; // 'currentcolor': the element's color replaces 'color' when the gradient is generated
	String color_mix;           // with current_color: a color-mix() of 'currentcolor' (CSS Color 5), resolved as it
#ifdef RMLUI_MATH_EXPRESSIONS
	CalculationPtr position_calculation = nullptr;
#endif
};
inline bool operator==(const ColorStop& a, const ColorStop& b)
{
	return a.color == b.color && a.position == b.position && a.current_color == b.current_color && a.color_mix == b.color_mix
#ifdef RMLUI_MATH_EXPRESSIONS
		&& a.position_calculation == b.position_calculation
#endif
		;
}
inline bool operator!=(const ColorStop& a, const ColorStop& b)
{
	return !(a == b);
}

struct BoxShadow {
	ColourbPremultiplied color;
	NumericValue offset_x, offset_y;
	NumericValue blur_radius;
	NumericValue spread_distance;
	bool inset = false;
	bool current_color = false; // 'currentcolor' (also when the color is omitted): the element's color replaces 'color'
	String color_mix;           // with current_color: a color-mix() of 'currentcolor' (CSS Color 5), resolved as it
#ifdef RMLUI_MATH_EXPRESSIONS
	CalculationPtr offset_x_calculation = nullptr, offset_y_calculation = nullptr;
	CalculationPtr blur_radius_calculation = nullptr, spread_distance_calculation = nullptr;
#endif
};
inline bool operator==(const BoxShadow& a, const BoxShadow& b)
{
	return a.color == b.color && a.offset_x == b.offset_x && a.offset_y == b.offset_y && a.blur_radius == b.blur_radius &&
		a.spread_distance == b.spread_distance
#ifdef RMLUI_MATH_EXPRESSIONS
		&& a.offset_x_calculation == b.offset_x_calculation && a.offset_y_calculation == b.offset_y_calculation &&
		a.blur_radius_calculation == b.blur_radius_calculation && a.spread_distance_calculation == b.spread_distance_calculation
#endif
		&& a.inset == b.inset && a.current_color == b.current_color && a.color_mix == b.color_mix;
}
inline bool operator!=(const BoxShadow& a, const BoxShadow& b)
{
	return !(a == b);
}

} // namespace Rml
