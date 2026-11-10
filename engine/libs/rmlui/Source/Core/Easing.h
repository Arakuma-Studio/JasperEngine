#pragma once

#include "../../Include/RmlUi/Core/Tween.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

/**
    The CSS easing functions (CSS Easing 2, phase 3): evaluation of cubic-bezier(), steps() and linear(), and the parser of
    one <easing-function> value, which also takes RmlUi's own tween names (cubic-in-out, ...).
 */
namespace Easing {

	/// The output of cubic-bezier(x1, y1, x2, y2) at input progress x. Inputs outside [0, 1] are extrapolated along the
	/// curve's end tangents, as browsers do.
	float EvaluateCubicBezier(float x1, float y1, float x2, float y2, float x);

	/// The output of steps(count, position) at input progress t, with the 'before flag' of CSS Easing 2.
	float EvaluateSteps(int count, Tween::StepPosition position, float t, bool before_flag);

	/// The output of linear() with the given control points (inputs ascending) at input progress t; outside the first
	/// and last inputs the first or last segment is extended.
	float EvaluateLinear(const Vector<Tween::LinearStop>& stops, float t, bool before_flag = false);

	/// Parses one easing function: a CSS keyword (linear, ease, ease-in, ease-out, ease-in-out, step-start, step-end), a
	/// CSS function (cubic-bezier(), steps(), linear()) or an RmlUi tween name ('cubic-in-out'). Keywords and function
	/// names are matched without regard to case.
	/// @return True if the value is one valid easing function.
	bool Parse(const String& value, Tween& tween);

	/// The CSS initial easing, 'ease'.
	Tween Ease();

	/// Parses a CSS <number> token without unit ('1', '-0.5', '.5', '1e3').
	bool ParseNumber(const String& text, float& result);

} // namespace Easing

} // namespace Rml
