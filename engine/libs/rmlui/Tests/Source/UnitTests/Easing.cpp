#include "../../../Source/Core/Easing.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Tween.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (docs/html-ui/phase-3-fork-patches.md 3.13.1, FORK.md P3-A1): the CSS easing functions. Reference values of the
// cubic-bezier() curves were computed by bisection on the curve parameter in double precision.

namespace {

Tween Parsed(const String& value)
{
	Tween tween;
	REQUIRE_MESSAGE(Easing::Parse(value, tween), value);
	return tween;
}

} // namespace

TEST_CASE("Easing.cubic_bezier")
{
	struct Curve {
		const char* value;
		float expected[5]; // at 0.1, 0.25, 0.5, 0.75, 0.9
	};
	const Curve curves[] = {
		{"ease", {0.094796f, 0.408511f, 0.802403f, 0.960459f, 0.994316f}},
		{"ease-in", {0.017027f, 0.093465f, 0.315357f, 0.621862f, 0.839428f}},
		{"ease-out", {0.160572f, 0.378138f, 0.684643f, 0.906535f, 0.982973f}},
		{"ease-in-out", {0.019722f, 0.129162f, 0.5f, 0.870838f, 0.980278f}},
		{"cubic-bezier(0.68, -0.55, 0.265, 1.55)", {-0.066291f, -0.082807f, 0.60668f, 1.089166f, 1.062373f}},
		{"cubic-bezier(.16, 1, .3, 1)", {0.494390617f, 0.825622301f, 0.971779105f, 0.997677067f, 0.999878348f}},
		{"cubic-bezier(0.1, 0.7, 1.0, 0.1)", {0.244779f, 0.350421f, 0.417277f, 0.489876f, 0.609904f}},
	};
	const float inputs[5] = {0.1f, 0.25f, 0.5f, 0.75f, 0.9f};

	for (const Curve& curve : curves)
	{
		const Tween tween = Parsed(curve.value);
		CHECK(tween.GetKind() == Tween::Kind::CubicBezier);
		for (int i = 0; i < 5; i++)
		{
			INFO(curve.value, " at ", inputs[i]);
			CHECK(tween(inputs[i]) == doctest::Approx(curve.expected[i]).epsilon(1e-4));
		}
		CHECK(tween(0.f) == doctest::Approx(0.f));
		CHECK(tween(1.f) == doctest::Approx(1.f));
	}

	// Outside [0, 1] the curve is extended along its end tangents.
	const Tween ease_in = Parsed("ease-in");
	CHECK(ease_in(-0.5f) == doctest::Approx(0.f));
	CHECK(ease_in(1.5f) == doctest::Approx(1.f + 0.5f / 0.58f));
	const Tween overshoot = Parsed("cubic-bezier(0.5, -1, 0.5, 2)");
	CHECK(overshoot(-0.5f) == doctest::Approx(1.f));
	CHECK(overshoot(1.5f) == doctest::Approx(0.f));
}

TEST_CASE("Easing.cubic_bezier_degenerate_tangents")
{
	CHECK(Parsed("cubic-bezier(0, 1, 0.5, 0.5)")(-0.5f) == doctest::Approx(-0.5f));
	CHECK(Parsed("cubic-bezier(0.5, 0.5, 1, 0)")(1.5f) == doctest::Approx(1.5f));
	CHECK(Parsed("cubic-bezier(0, 0, 0, 0)")(-0.5f) == doctest::Approx(0.f));
	CHECK(Parsed("cubic-bezier(1, 1, 1, 1)")(1.5f) == doctest::Approx(1.f));
	// This curve has x = t^3 and y = 1 - (1 - t)^3: a small error in x is amplified near zero.
	const Tween steep = Parsed("cubic-bezier(0, 1, 0, 1)");
	CHECK(steep(1e-9f) == doctest::Approx(0.002997001f).epsilon(1e-5));
	CHECK(steep(1e-6f) == doctest::Approx(0.029701f).epsilon(1e-5));
}

TEST_CASE("Easing.steps")
{
	struct Sample {
		float t;
		float expected;
	};
	struct Case {
		const char* value;
		Vector<Sample> samples;
	};
	const Case cases[] = {
		{"steps(4)", {{0.f, 0.f}, {0.24f, 0.f}, {0.25f, 0.25f}, {0.5f, 0.5f}, {0.99f, 0.75f}, {1.f, 1.f}}},
		{"steps(4, end)", {{0.f, 0.f}, {0.25f, 0.25f}, {1.f, 1.f}}},
		{"steps(4, jump-start)", {{0.f, 0.25f}, {0.24f, 0.25f}, {0.25f, 0.5f}, {0.99f, 1.f}, {1.f, 1.f}}},
		{"steps(4, start)", {{0.f, 0.25f}, {0.5f, 0.75f}}},
		{"steps(3, jump-both)", {{0.f, 0.25f}, {0.34f, 0.5f}, {0.67f, 0.75f}, {1.f, 1.f}}},
		{"steps(3, jump-none)", {{0.f, 0.f}, {0.33f, 0.f}, {0.34f, 0.5f}, {0.67f, 1.f}, {1.f, 1.f}}},
		{"step-start", {{0.f, 1.f}, {0.5f, 1.f}, {1.f, 1.f}}},
		{"step-end", {{0.f, 0.f}, {0.5f, 0.f}, {0.999f, 0.f}, {1.f, 1.f}}},
	};
	for (const Case& c : cases)
	{
		const Tween tween = Parsed(c.value);
		CHECK(tween.GetKind() == Tween::Kind::Steps);
		for (const Sample& sample : c.samples)
		{
			INFO(c.value, " at ", sample.t);
			CHECK(tween(sample.t) == doctest::Approx(sample.expected));
		}
	}

	// The before flag (an animation in its before phase): a jump at the input does not happen yet.
	const Tween step_start = Parsed("step-start");
	CHECK(step_start.Evaluate(0.f, false) == 1.f);
	CHECK(step_start.Evaluate(0.f, true) == 0.f);
	const Tween jump_both = Parsed("steps(2, jump-both)");
	CHECK(jump_both.Evaluate(0.f, false) == doctest::Approx(1.f / 3.f));
	CHECK(jump_both.Evaluate(0.f, true) == 0.f);
	CHECK(jump_both.Evaluate(0.25f, true) == doctest::Approx(1.f / 3.f));
}

TEST_CASE("Easing.linear")
{
	const Tween identity = Parsed("linear");
	CHECK(identity.GetKind() == Tween::Kind::LinearStops);
	CHECK(identity(0.3f) == doctest::Approx(0.3f));
	CHECK(identity.to_string() == "linear");

	const Tween knee = Parsed("linear(0, 0.25 75%, 1)");
	CHECK(knee(0.f) == doctest::Approx(0.f));
	CHECK(knee(0.375f) == doctest::Approx(0.125f));
	CHECK(knee(0.75f) == doctest::Approx(0.25f));
	CHECK(knee(0.875f) == doctest::Approx(0.625f));
	CHECK(knee(1.f) == doctest::Approx(1.f));
	// Extended beyond the ends.
	CHECK(knee(1.5f) == doctest::Approx(2.5f));
	CHECK(knee(-0.75f) == doctest::Approx(-0.25f));

	// Missing inputs are spread evenly; a stop may come with its percentage first.
	const Tween even = Parsed("linear(0, 0.5, 0.75, 1)");
	CHECK(even(1.f / 3.f) == doctest::Approx(0.5f));
	CHECK(even(2.f / 3.f) == doctest::Approx(0.75f));
	CHECK(Parsed("linear(0, 50% 0.8, 1)")(0.5f) == doctest::Approx(0.8f));

	// Two percentages make a flat segment; an input below an earlier one takes the earlier one.
	const Tween flat = Parsed("linear(0, 0.5 25% 75%, 1)");
	CHECK(flat(0.5f) == doctest::Approx(0.5f));
	CHECK(flat(0.125f) == doctest::Approx(0.25f));
	const Tween clamped = Parsed("linear(0 20%, 0.5 10%, 1)");
	CHECK(clamped(0.1f) == doctest::Approx(0.f)); // before coincident first inputs, keep the first output
	CHECK(clamped(0.6f) == doctest::Approx(0.75f));
}

TEST_CASE("Easing.linear_discontinuities")
{
	const Tween start_jump = Parsed("linear(0 0%, 0.5 0%, 1)");
	CHECK(start_jump(-0.5f) == doctest::Approx(0.f));
	CHECK(start_jump.Evaluate(0.f, true) == doctest::Approx(0.f));
	CHECK(start_jump.Evaluate(0.f, false) == doctest::Approx(0.5f));
	CHECK(start_jump(0.5f) == doctest::Approx(0.75f));

	const Tween end_jump = Parsed("linear(0, 0.5 100%, 1 100%)");
	CHECK(end_jump(0.5f) == doctest::Approx(0.25f));
	CHECK(end_jump(1.f) == doctest::Approx(1.f));
	CHECK(end_jump(1.5f) == doctest::Approx(1.f));

	const Tween middle_jump = Parsed("linear(0, 0.2 50%, 0.8 50%, 1)");
	CHECK(middle_jump(0.25f) == doctest::Approx(0.1f));
	CHECK(middle_jump.Evaluate(0.5f, true) == doctest::Approx(0.8f));
	CHECK(middle_jump(0.5f) == doctest::Approx(0.8f));
	CHECK(middle_jump(0.75f) == doctest::Approx(0.9f));
}

TEST_CASE("Easing.linear_constant")
{
	for (const char* value : {"linear(0.4)", "linear(0.4 25%)", "linear(0.4 25% 75%)"})
	{
		const Tween constant = Parsed(value);
		for (float t : {-1.f, 0.f, 0.25f, 0.5f, 1.f, 2.f})
			CHECK(constant(t) == doctest::Approx(0.4f));
		CHECK(Parsed(constant.to_string()) == constant);
		Tween reversed = constant;
		reversed.reverse();
		CHECK(reversed(0.5f) == doctest::Approx(0.6f));
	}
}

TEST_CASE("Easing.parse")
{
	// Keywords and function names are not case-sensitive; RmlUi's tween names stay accepted.
	CHECK(Parsed("EASE-IN") == Parsed("cubic-bezier(0.42, 0, 1, 1)"));
	CHECK(Parsed("Cubic-Bezier(0.42, 0, 1, 1)") == Parsed("ease-in"));
	CHECK(Parsed("step-start") == Parsed("steps(1, jump-start)"));
	CHECK(Parsed("step-end") == Parsed("steps(1)"));
	CHECK(Parsed("cubic-in-out") == Tween(Tween::Cubic, Tween::InOut));
	CHECK(Parsed("quadratic-out") == Tween(Tween::Quadratic, Tween::Out));
	CHECK(Parsed("cubic-bezier(.1, .2, .3, .4)") == Tween::CubicBezier(0.1f, 0.2f, 0.3f, 0.4f));

	const char* invalid[] = {
		"",
		"ease-sideways",
		"cubic-bezier(1.1, 0, 0, 1)",
		"cubic-bezier(-0.1, 0, 0, 1)",
		"cubic-bezier(0, 0, 1)",
		"cubic-bezier(0, 0, 1, 1, 1)",
		"cubic-bezier(0, 0px, 1, 1)",
		"cubic-bezier(0, , 1, 1)",
		"steps(0)",
		"steps(-1)",
		"steps(2.5)",
		"steps(1, jump-none)",
		"steps(2, sideways)",
		"steps(2, end, end)",
		"linear()",
		"linear(0, 1 2 3)",
		"linear(0, 50% 60% 70%, 1)",
		"linear(50%, 1)",
		"spring(1)",
		"ease)",
	};
	for (const char* value : invalid)
	{
		Tween tween;
		INFO(value);
		CHECK_FALSE(Easing::Parse(value, tween));
	}
}

TEST_CASE("Easing.serialization")
{
	const char* round_trip[] = {
		"ease",
		"ease-in",
		"ease-out",
		"ease-in-out",
		"linear",
		"step-start",
		"step-end",
		"cubic-bezier(0.1, 0.7, 1, 0.1)",
		"steps(4)",
		"steps(3, jump-start)",
		"steps(3, jump-none)",
		"steps(3, jump-both)",
		"linear(0 0%, 0.25 75%, 1 100%)",
		"cubic-in-out",
	};
	for (const char* value : round_trip)
	{
		INFO(value);
		const Tween tween = Parsed(value);
		CHECK(tween.to_string() == value);
		CHECK(Parsed(tween.to_string()) == tween);
	}
}

TEST_CASE("Easing.reverse")
{
	// A reversed function f becomes 1 - f(1 - t).
	for (const char* value : {"ease", "cubic-bezier(0.68, -0.55, 0.265, 1.55)", "linear(0, 0.25 75%, 1)", "steps(3, jump-start)"})
	{
		const Tween tween = Parsed(value);
		Tween reversed = tween;
		reversed.reverse();
		for (float t : {0.1f, 0.3f, 0.6f, 0.9f})
		{
			INFO(value, " at ", t);
			CHECK(reversed(t) == doctest::Approx(1.f - tween(1.f - t)).epsilon(1e-4));
		}
	}
}
