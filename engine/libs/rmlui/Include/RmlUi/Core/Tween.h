#pragma once

#include "Header.h"
#include "Types.h"

namespace Rml {

/**
    A timing function: RmlUi's named tweens (cubic-in-out, ...), or one of the CSS easing functions (phase 3): cubic-bezier()
    and the keywords defined by it (ease, ease-in, ...), steps() and step-start/step-end, linear() and linear.
 */
class RMLUICORE_API Tween {
public:
	enum Type { None, Back, Bounce, Circular, Cubic, Elastic, Exponential, Linear, Quadratic, Quartic, Quintic, Sine, Callback, Count };
	enum Direction { In = 1, Out = 2, InOut = 3 };
	using CallbackFnc = float (*)(float);

	// The kind of timing function. 'Legacy' is RmlUi's named tweens (and the callback), the others are CSS Easing 2.
	enum class Kind : uint8_t { Legacy, CubicBezier, Steps, LinearStops };
	// The position of the jumps of steps().
	enum class StepPosition : uint8_t { JumpStart, JumpEnd, JumpNone, JumpBoth };
	// A control point of linear(): the output at the input progress.
	struct LinearStop {
		float input;
		float output;
	};

	Tween(Type type = Linear, Direction direction = Out);
	Tween(Type type_in, Type type_out);
	Tween(CallbackFnc callback, Direction direction = In);

	/// cubic-bezier(x1, y1, x2, y2); x1 and x2 must be in [0, 1].
	static Tween CubicBezier(float x1, float y1, float x2, float y2);
	/// steps(count, position); count >= 1 (>= 2 for jump-none).
	static Tween Steps(int count, StepPosition position);
	/// linear(stops): the control points with their inputs resolved (ascending). No stops: the identity ('linear').
	static Tween LinearFunction(Vector<LinearStop> stops);

	/// Evaluate the timing function at input progress t, nominally in [0, 1].
	float operator()(float t) const;
	/// Evaluate with the 'before flag' of CSS Easing 2 (steps() and linear()): set in an animation's before phase.
	float Evaluate(float t, bool before_flag) const;

	/// Reverse direction of the tweening function: the function f becomes 1 - f(1 - t).
	void reverse();

	Kind GetKind() const { return kind; }

	bool operator==(const Tween& other) const;
	bool operator!=(const Tween& other) const;

	/// The name of a legacy tween ('cubic-in-out'), or the CSS serialization of a CSS easing function ('ease',
	/// 'cubic-bezier(0.1, 0.7, 1, 0.1)', 'steps(4, jump-start)', 'linear(0, 0.25 75%, 1)').
	String to_string() const;

private:
	float tween(Type type, float t) const;
	float in(float t) const;
	float out(float t) const;
	float in_out(float t) const;

	Kind kind = Kind::Legacy;
	Type type_in = None;
	Type type_out = None;
	CallbackFnc callback = nullptr;

	// CubicBezier: x1, y1, x2, y2.
	float bezier[4] = {};
	// Steps.
	int step_count = 0;
	StepPosition step_position = StepPosition::JumpEnd;
	// LinearStops: the control points (empty: the identity).
	SharedPtr<const Vector<LinearStop>> stops;
};

} // namespace Rml
