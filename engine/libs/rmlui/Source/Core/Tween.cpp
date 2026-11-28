#include "../../Include/RmlUi/Core/Tween.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "Easing.h"
#include <algorithm>
#include <utility>

namespace Rml {

namespace TweenFunctions {

	/**
	    Tweening functions below.
	    Partly based on http://libclaw.sourceforge.net/tweeners.html
	 */

	static inline float square(float t)
	{
		return t * t;
	}

	static float back(float t)
	{
		return t * t * (2.70158f * t - 1.70158f);
	}

	static float bounce(float t)
	{
		if (t > 1.f - 1.f / 2.75f)
			return 1.f - 7.5625f * square(1.f - t);
		else if (t > 1.f - 2.f / 2.75f)
			return 1.0f - (7.5625f * square(1.f - t - 1.5f / 2.75f) + 0.75f);
		else if (t > 1.f - 2.5f / 2.75f)
			return 1.0f - (7.5625f * square(1.f - t - 2.25f / 2.75f) + 0.9375f);
		return 1.0f - (7.5625f * square(1.f - t - 2.625f / 2.75f) + 0.984375f);
	}

	static float circular(float t)
	{
		return 1.f - Math::SquareRoot(1.f - t * t);
	}

	static float cubic(float t)
	{
		return t * t * t;
	}

	static float elastic(float t)
	{
		if (t == 0)
			return t;
		if (t == 1)
			return t;
		return -Math::Exp(7.24f * (t - 1.f)) * Math::Sin((t - 1.1f) * 2.f * Math::RMLUI_PI / 0.4f);
	}

	static float exponential(float t)
	{
		if (t == 0)
			return t;
		if (t == 1)
			return t;
		return Math::Exp(7.24f * (t - 1.f));
	}

	static float linear(float t)
	{
		return t;
	}

	static float quadratic(float t)
	{
		return t * t;
	}

	static float quartic(float t)
	{
		return t * t * t * t;
	}

	static float quintic(float t)
	{
		return t * t * t * t * t;
	}

	static float sine(float t)
	{
		return 1.f - Math::Cos(t * Math::RMLUI_PI * 0.5f);
	}

} // namespace TweenFunctions

Tween::Tween(Type type, Direction direction)
{
	if (direction & In)
		type_in = type;
	if (direction & Out)
		type_out = type;
}
Tween::Tween(Type type_in, Type type_out) : type_in(type_in), type_out(type_out) {}
Tween::Tween(CallbackFnc callback, Direction direction) : callback(callback)
{
	if (direction & In)
		type_in = Callback;
	if (direction & Out)
		type_out = Callback;
}
Tween Tween::CubicBezier(float x1, float y1, float x2, float y2)
{
	Tween result;
	result.kind = Kind::CubicBezier;
	result.type_in = None;
	result.type_out = None;
	result.bezier[0] = x1;
	result.bezier[1] = y1;
	result.bezier[2] = x2;
	result.bezier[3] = y2;
	return result;
}

Tween Tween::Steps(int count, StepPosition position)
{
	Tween result;
	result.kind = Kind::Steps;
	result.type_in = None;
	result.type_out = None;
	result.step_count = Math::Max(count, 1);
	result.step_position = position;
	return result;
}

Tween Tween::LinearFunction(Vector<LinearStop> stops)
{
	Tween result;
	result.kind = Kind::LinearStops;
	result.type_in = None;
	result.type_out = None;
	if (!stops.empty())
		result.stops = MakeShared<const Vector<LinearStop>>(std::move(stops));
	return result;
}

float Tween::operator()(float t) const
{
	return Evaluate(t, false);
}

float Tween::Evaluate(float t, bool before_flag) const
{
	switch (kind)
	{
	case Kind::CubicBezier: return Easing::EvaluateCubicBezier(bezier[0], bezier[1], bezier[2], bezier[3], t);
	case Kind::Steps: return Easing::EvaluateSteps(step_count, step_position, t, before_flag);
	case Kind::LinearStops: return stops ? Easing::EvaluateLinear(*stops, t, before_flag) : t;
	case Kind::Legacy: break;
	}

	if (type_in != None && type_out == None)
	{
		return in(t);
	}
	if (type_in == None && type_out != None)
	{
		return out(t);
	}
	if (type_in != None && type_out != None)
	{
		return in_out(t);
	}
	return t;
}

void Tween::reverse()
{
	switch (kind)
	{
	case Kind::CubicBezier:
	{
		const float x1 = bezier[0], y1 = bezier[1], x2 = bezier[2], y2 = bezier[3];
		bezier[0] = 1.f - x2;
		bezier[1] = 1.f - y2;
		bezier[2] = 1.f - x1;
		bezier[3] = 1.f - y1;
	}
	break;
	case Kind::Steps:
		if (step_position == StepPosition::JumpStart)
			step_position = StepPosition::JumpEnd;
		else if (step_position == StepPosition::JumpEnd)
			step_position = StepPosition::JumpStart;
		break;
	case Kind::LinearStops:
		if (stops)
		{
			Vector<LinearStop> reversed;
			reversed.reserve(stops->size());
			for (auto it = stops->rbegin(); it != stops->rend(); ++it)
				reversed.push_back(LinearStop{1.f - it->input, 1.f - it->output});
			stops = MakeShared<const Vector<LinearStop>>(std::move(reversed));
		}
		break;
	case Kind::Legacy: std::swap(type_in, type_out); break;
	}
}

bool Tween::operator==(const Tween& other) const
{
	if (kind != other.kind)
		return false;
	switch (kind)
	{
	case Kind::CubicBezier:
		return bezier[0] == other.bezier[0] && bezier[1] == other.bezier[1] && bezier[2] == other.bezier[2] && bezier[3] == other.bezier[3];
	case Kind::Steps: return step_count == other.step_count && step_position == other.step_position;
	case Kind::LinearStops:
	{
		if (stops == other.stops)
			return true;
		if (!stops || !other.stops || stops->size() != other.stops->size())
			return false;
		for (size_t i = 0; i < stops->size(); i++)
			if ((*stops)[i].input != (*other.stops)[i].input || (*stops)[i].output != (*other.stops)[i].output)
				return false;
		return true;
	}
	case Kind::Legacy: break;
	}
	return type_in == other.type_in && type_out == other.type_out && callback == other.callback;
}

bool Tween::operator!=(const Tween& other) const
{
	return !(*this == other);
}

static String FormatEasingNumber(float value)
{
	String result = CreateString("%g", value);
	if (result == "-0")
		result = "0";
	return result;
}

String Tween::to_string() const
{
	switch (kind)
	{
	case Kind::CubicBezier:
	{
		struct Keyword {
			const char* name;
			float p[4];
		};
		static const Keyword keywords[] = {
			{"ease", {0.25f, 0.1f, 0.25f, 1.f}},
			{"ease-in", {0.42f, 0.f, 1.f, 1.f}},
			{"ease-out", {0.f, 0.f, 0.58f, 1.f}},
			{"ease-in-out", {0.42f, 0.f, 0.58f, 1.f}},
		};
		for (const Keyword& keyword : keywords)
			if (std::equal(bezier, bezier + 4, keyword.p))
				return keyword.name;
		return "cubic-bezier(" + FormatEasingNumber(bezier[0]) + ", " + FormatEasingNumber(bezier[1]) + ", " + FormatEasingNumber(bezier[2]) + ", " +
			FormatEasingNumber(bezier[3]) + ")";
	}
	case Kind::Steps:
	{
		if (step_count == 1 && step_position == StepPosition::JumpStart)
			return "step-start";
		if (step_count == 1 && step_position == StepPosition::JumpEnd)
			return "step-end";
		static const char* const positions[] = {"jump-start", "jump-end", "jump-none", "jump-both"};
		String result = "steps(" + CreateString("%d", step_count);
		if (step_position != StepPosition::JumpEnd)
			result += String(", ") + positions[(int)step_position];
		return result + ")";
	}
	case Kind::LinearStops:
	{
		if (!stops)
			return "linear";
		String result = "linear(";
		for (size_t i = 0; i < stops->size(); i++)
		{
			if (i > 0)
				result += ", ";
			result += FormatEasingNumber((*stops)[i].output) + " " + FormatEasingNumber((*stops)[i].input * 100.f) + "%";
		}
		return result + ")";
	}
	case Kind::Legacy: break;
	}

	static const Array<String, size_t(Count)> type_str = {
		{"none", "back", "bounce", "circular", "cubic", "elastic", "exponential", "linear", "quadratic", "quartic", "quintic", "sine", "callback"}};

	if (size_t(type_in) < type_str.size() && size_t(type_out) < type_str.size())
	{
		if (type_in == None && type_out == None)
		{
			return "none";
		}
		else if (type_in == type_out)
		{
			return type_str[size_t(type_in)] + String("-in-out");
		}
		else if (type_in == None)
		{
			return type_str[size_t(type_out)] + String("-out");
		}
		else if (type_out == None)
		{
			return type_str[size_t(type_in)] + String("-in");
		}
		else if (type_in != type_out)
		{
			return type_str[size_t(type_in)] + String("-in-") + type_str[size_t(type_out)] + String("-out");
		}
	}
	return "unknown";
}

float Tween::tween(Type type, float t) const
{
	using namespace TweenFunctions;

	switch (type)
	{
	case Back: return back(t);
	case Bounce: return bounce(t);
	case Circular: return circular(t);
	case Cubic: return cubic(t);
	case Elastic: return elastic(t);
	case Exponential: return exponential(t);
	case Linear: return linear(t);
	case Quadratic: return quadratic(t);
	case Quartic: return quartic(t);
	case Quintic: return quintic(t);
	case Sine: return sine(t);
	case Callback:
		if (callback)
			return (*callback)(t);
		break;
	default: break;
	}
	return t;
}

float Tween::in(float t) const
{
	return tween(type_in, t);
}

float Tween::out(float t) const
{
	return 1.0f - tween(type_out, 1.0f - t);
}

float Tween::in_out(float t) const
{
	if (t < 0.5f)
		return tween(type_in, 2.0f * t) * 0.5f;
	else
		return 0.5f + out(2.0f * t - 1.0f) * 0.5f;
}

} // namespace Rml
