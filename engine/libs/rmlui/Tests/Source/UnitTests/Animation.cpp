#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include "../Common/TypesToString.h"
#include <RmlUi/Core/Animation.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <algorithm>
#include <doctest.h>
#include <float.h>

using namespace Rml;

static const String document_decorator_rml = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			left: 0;
			top: 0;
			right: 0;
			bottom: 0;
		}

		@decorator from_rule : horizontal-gradient { %s }
		@decorator to_rule: horizontal-gradient{ %s }

		@keyframes mix {
			from { %s: %s; }
			to   { %s: %s; }
		}
		div {
			background: #333;
			height: 64px;
			width: 64px;
			animation: mix 0.1s linear;
		}
	</style>
</head>

<body>
	<div/>
</body>
</rml>
)";

TEST_CASE("animation.decorator")
{
	struct Test {
		String from_rule;
		String to_rule;
		String from;
		String to;
		String expected_25p; // expected interpolated value at 25% progression
	};

	Vector<Test> tests{
		// Only standard declaration
		{
			"",
			"",

			"horizontal-gradient(transparent transparent)",
			"horizontal-gradient(white white)",

			"horizontal-gradient(horizontal #ffffff40 #ffffff40)",
		},
		{
			"",
			"",

			"horizontal-gradient(transparent transparent) border-box",
			"horizontal-gradient(white white) border-box",

			"horizontal-gradient(horizontal #ffffff40 #ffffff40) border-box",
		},
		{
			"",
			"",

			"none",
			"horizontal-gradient(transparent transparent)",

			"horizontal-gradient(horizontal #ffffffbf #ffffffbf)",
		},
		{
			"",
			"",

			"none",
			"horizontal-gradient(transparent transparent), horizontal-gradient(transparent transparent)",

			"horizontal-gradient(horizontal #ffffffbf #ffffffbf), horizontal-gradient(horizontal #ffffffbf #ffffffbf)",
		},
		{
			"",
			"",

			"horizontal-gradient(transparent transparent), horizontal-gradient(transparent transparent)",
			"none",

			"horizontal-gradient(horizontal #ffffff40 #ffffff40), horizontal-gradient(horizontal #ffffff40 #ffffff40)",
		},

		/// Only rule declaration
		{
			"start-color: transparent; stop-color: transparent;",
			"start-color: white; stop-color: white;",

			"from_rule",
			"to_rule",

			"horizontal-gradient(horizontal #ffffff40 #ffffff40)",
		},
		{
			"",
			"start-color: transparent; stop-color: transparent;",

			"from_rule",
			"to_rule",

			"horizontal-gradient(horizontal #ffffffbf #ffffffbf)",
		},
		{
			"start-color: transparent; stop-color: transparent;",
			"",

			"from_rule",
			"to_rule",

			"horizontal-gradient(horizontal #ffffff40 #ffffff40)",
		},

		/// Mix rule and standard declaration
		{
			"start-color: transparent; stop-color: transparent;",
			"",

			"from_rule",
			"horizontal-gradient(white white)",

			"horizontal-gradient(horizontal #ffffff40 #ffffff40)",
		},
		{
			"",
			"start-color: transparent; stop-color: transparent;",

			"none",
			"to_rule",

			"horizontal-gradient(horizontal #ffffffbf #ffffffbf)",
		},
		{
			"start-color: transparent; stop-color: transparent;",
			"",

			"from_rule",
			"none",

			"horizontal-gradient(horizontal #ffffff40 #ffffff40)",
		},
		{
			"",
			"",

			"from_rule, to_rule",
			"horizontal-gradient(transparent transparent), horizontal-gradient(transparent transparent)",

			"horizontal-gradient(horizontal #ffffffbf #ffffffbf), horizontal-gradient(horizontal #ffffffbf #ffffffbf)",
		},
		{
			"",
			"",

			"horizontal-gradient(transparent transparent), horizontal-gradient(transparent transparent)",
			"from_rule, to_rule",

			"horizontal-gradient(horizontal #ffffff40 #ffffff40), horizontal-gradient(horizontal #ffffff40 #ffffff40)",
		},

		// Standard declaration of linear gradients (consider string conversion a best-effort for now)
		{
			"",
			"",

			"linear-gradient(transparent, transparent)",
			"linear-gradient(white, white)",

			"linear-gradient(180deg unspecified unspecified unspecified #ffffff40, #ffffff40)",
		},
		{
			"",
			"",

			"linear-gradient(0deg, transparent, transparent)",
			"linear-gradient(180deg, white, white)",

			"linear-gradient(45deg unspecified unspecified unspecified #ffffff40, #ffffff40)",
		},
		{
			"",
			"",

			"linear-gradient(105deg, #000000 0%, #ff0000 100%)",
			"linear-gradient(105deg, #ffffff 20%, #00ff00 60%)",

			"linear-gradient(105deg unspecified unspecified unspecified #404040 5%, #bf4000 90%)",
		},
		{
			"",
			"",

			"linear-gradient(105deg, #000000 0%, #ff0000 50%, #ff00ff 100%)",
			"linear-gradient(105deg, #ffffff 0%, #ffffff 10%, #ffffff 100%)",

			"linear-gradient(105deg unspecified unspecified unspecified #404040 0%, #ff4040 40%, #ff40ff 100%)",
		},
		{
			"",
			"",

			"linear-gradient(to right, transparent, transparent)",
			"linear-gradient(270deg, transparent, transparent)",

			// We don't really handle mixing direction keywords and angles here, the output will not be what one might expect.
			"linear-gradient(202.5deg to right unspecified #00000000, #00000000)",
		},
		{
			"",
			"",

			"linear-gradient(to right, transparent, transparent)",
			"linear-gradient(to left, transparent, transparent)",

			// This will effectively evaluate to "to right" with angle being ignored, resulting in discrete interpolation. Not ideal.
			"linear-gradient(180deg to right unspecified #00000000, #00000000)",
		},
		{
			"",
			"",

			"linear-gradient(#000000 0%, #ffffff 100%)",
			"repeating-linear-gradient(#000000 0%, #ffffff 100%)",

			"linear-gradient(#000000 0%, #ffffff 100%)",
		},
	};

	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	Context* context = TestsShell::GetContext();

	for (const String property_str : {"decorator", "mask-image"})
	{
		for (const Test& test : tests)
		{
			const double t_final = 0.1;

			system_interface->SetManualTime(0.0);
			String document_rml = Rml::CreateString(document_decorator_rml.c_str(), test.from_rule.c_str(), test.to_rule.c_str(),
				property_str.c_str(), test.from.c_str(), property_str.c_str(), test.to.c_str());

			ElementDocument* document = context->LoadDocumentFromMemory(document_rml, "assets/");
			Element* element = TestsShell::GetElementChild(document, 0);

			document->Show();
			TestsShell::RenderLoop();

			system_interface->SetManualTime(0.25 * t_final);
			TestsShell::RenderLoop();

			CAPTURE(property_str);
			CAPTURE(test.from);
			CAPTURE(test.to);
			CHECK(element->GetProperty<String>(property_str) == test.expected_25p);

			document->Close();
		}
	}

	TestsShell::ShutdownShell();
}

static const String document_filter_rml = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			left: 0;
			top: 0;
			right: 0;
			bottom: 0;
		}
		@keyframes mix {
			from { %s: %s; }
			to   { %s: %s; }
		}
		div {
			background: #333;
			height: 64px;
			width: 64px;
			decorator: image(high_scores_alien_1.tga);
			animation: mix 0.1s linear;
		}
	</style>
</head>

<body>
	<div/>
</body>
</rml>
)";

TEST_CASE("animation.filter")
{
	struct Test {
		String from;
		String to;
		String expected_25p; // expected interpolated value at 25% progression
	};

	Vector<Test> tests{
		{
			"blur( 0px)",
			"blur(40px)",
			"blur(10px)",
		},
		{
			"blur(10px)",
			"blur(25dp)", // assumes dp-ratio == 2
			"blur(20px)",
		},
		{
			"blur(40px)",
			"none",
			"blur(30px)",
		},
		{
			"none",
			"blur(40px)",
			"blur(10px)",
		},
		{
			"drop-shadow(#000 30px 20px 0px)",
			"drop-shadow(#f00 30px 20px 4px)", // colors interpolated in linear space
			"drop-shadow(#400000 30px 20px 1px)",
		},
		{
			"opacity(0) brightness(2)",
			"none",
			"opacity(0.25) brightness(1.75)",
		},
		{
			"opacity(0) brightness(0)",
			"opacity(0.5)",
			"opacity(0.125) brightness(0.25)",
		},
		{
			"opacity(0.5)",
			"opacity(0) brightness(0)",
			"opacity(0.375) brightness(0.75)",
		},
		{
			"opacity(0) brightness(0)",
			"brightness(1) opacity(0.5)", // discrete interpolation due to non-matching types
			"opacity(0) brightness(0)",
		},
		{
			"none", // Test initial values of various filters.
			"brightness(2.00) contrast(2.00) grayscale(1.00) hue-rotate(4rad) invert(1.00) opacity(0.00) sepia(1.00) saturate(2.00)",
			"brightness(1.25) contrast(1.25) grayscale(0.25) hue-rotate(1rad) invert(0.25) opacity(0.75) sepia(0.25) saturate(1.25)",
		},
	};

	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	Context* context = TestsShell::GetContext();
	context->SetDensityIndependentPixelRatio(2.0f);

	for (const char* property_str : {"filter", "backdrop-filter"})
	{
		for (const Test& test : tests)
		{
			const double t_final = 0.1;

			system_interface->SetManualTime(0.0);
			String document_rml = Rml::CreateString(document_filter_rml.c_str(), property_str, test.from.c_str(), property_str, test.to.c_str());

			ElementDocument* document = context->LoadDocumentFromMemory(document_rml, "assets/");
			Element* element = TestsShell::GetElementChild(document, 0);

			document->Show();

			system_interface->SetManualTime(0.25 * t_final);
			TestsShell::RenderLoop();

			CHECK_MESSAGE(element->GetProperty<String>(property_str) == test.expected_25p, property_str, " from: ", test.from, ", to: ", test.to);

			document->Close();
		}
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("animation.case_sensitivity")
{
	static const String document_rml_template = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			left: 0;
			top: 0;
			right: 0;
			bottom: 0;
		}
		@keyframes %s {
			to { visibility: visible; }
		}
		div {
			width: 300dp;
			height: 300dp;
			visibility: hidden;
			background-color: #666;
			animation: 1.5s %s;
		}
	</style>
</head>

<body>
	<div/>
</body>
</rml>
)";

	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	Context* context = TestsShell::GetContext();

	struct TestCase {
		String keyframe_name;
		String animation_name;
		bool expected_visible_end;
	};

	const Vector<TestCase> test_cases = {
		// TODO: Make name-lookups case sensitive:
		{"mix", "Mix", false},
		{"Mix", "mix", false},

		{"show", "show", true},
		{"Show", "Show", true},
	};

	for (const auto& test_case : test_cases)
	{
		system_interface->SetManualTime(0.0);
		INFO("@keyframes: ", test_case.keyframe_name);
		INFO("animation: ", test_case.animation_name);

		const String document_rml = CreateString(document_rml_template.c_str(), test_case.keyframe_name.c_str(), test_case.animation_name.c_str());
		ElementDocument* document = context->LoadDocumentFromMemory(document_rml, "assets/");
		Element* element = TestsShell::GetElementChild(document, 0);
		document->Show();
		REQUIRE(element->IsVisible() == false);

		constexpr double t_end = 1.0;
		constexpr double dt = 0.1;
		double t = 0;
		while (t < t_end)
		{
			t += dt;
			system_interface->SetManualTime(t);
			context->Update();
		}

		CHECK(element->IsVisible() == test_case.expected_visible_end);

		document->Close();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("animation.case_sensitive_distinct_keyframes")
{
	static const String document_rml = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			left: 0;
			top: 0;
			right: 0;
			bottom: 0;
		}
		@keyframes show {
			50%, 100% { opacity: 0.25; }
		}
		@keyframes Show {
			50%, 100% { opacity: 0.5; }
		}
		@keyframes SHOW {
			0%, 25% { opacity: 0.75; }
			75%, 100% { opacity: 1.0; }
		}
		div {
			width: 300dp;
			height: 300dp;
			opacity: 0.0;
			background-color: #666;
		}
		.show-lower {
			animation: 2s show;
		}
		.show-upper {
			animation: 2s Show;
		}
		.show-infinite {
			animation: 0.5s infinite alternate SHOW;
		}
		.show-infinite-upper-keywords {
			animation: 0.5s INFINITE ALTERNATE SHOW;
		}
	</style>
</head>

<body>
	<div/>
</body>
</rml>
)";

	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	Context* context = TestsShell::GetContext();

	struct TestCase {
		String class_name;
		float expected_opacity_start; // The first keyframe applies from the start (CSS).
		float expected_opacity_end;
	};

	const Vector<TestCase> test_cases = {
		{"show-lower", 0.f, 0.25f},
		{"show-upper", 0.f, 0.5f},
		{"show-infinite", 0.75f, 0.75f},
		{"show-infinite-upper-keywords", 0.75f, 0.75f},
	};

	for (const auto& test_case : test_cases)
	{
		INFO("Class name: ", test_case.class_name);
		system_interface->SetManualTime(0.0);
		ElementDocument* document = context->LoadDocumentFromMemory(document_rml, "assets/");
		Element* element = TestsShell::GetElementChild(document, 0);
		element->SetClass(test_case.class_name, true);
		document->Show();
		REQUIRE(element->GetProperty<float>("opacity") == test_case.expected_opacity_start);

		constexpr double t_end = 1.0;
		constexpr double dt = 0.1;
		double t = 0;
		while (t < t_end)
		{
			t += dt;
			system_interface->SetManualTime(t);
			context->Update();
		}

		CHECK(element->GetProperty<float>("opacity") == test_case.expected_opacity_end);

		document->Close();
	}

	TestsShell::ShutdownShell();
}

static const String document_multiple_values_rml = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			left: 0;
			top: 0;
			right: 0;
			bottom: 0;
		}
		@keyframes fade {
			from { opacity: 0; }
			to { opacity: 1; }
		}
		@keyframes colorize {
			from { background-color: #f00; }
			25% { background-color: #0f0; }
			to { background-color: #00f; }
		}
		div {
			width: 300dp;
			height: 300dp;
			background-color: #000;
			animation: 2s linear fade, 4s 1s linear colorize;
		}
	</style>
</head>

<body>
	<div/>
</body>
</rml>
)";

TEST_CASE("animation.multiple_values")
{
	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	Context* context = TestsShell::GetContext();

	const double dt = 0.1;
	const double t_fade_start = 0;
	const double t_colorize_start = 1;
	const double t_fade_final = 2;
	const double t_colorize_final = 5;

	struct TestCase {
		double time;
		float opacity_min;
		Colourb background_color;
	};
	const std::vector<TestCase> tests = {
		{t_fade_start, 0.f, {0, 0, 0}},
		{t_fade_start + dt, 0.01f, {0, 0, 0}},

		// Before its delay ends an animation without backwards fill applies nothing; from its start it applies its first keyframe.
		{t_colorize_start - dt, 0.4f, {0, 0, 0}},
		{t_colorize_start, 0.49f, {0xff, 0, 0}},
		{t_colorize_start + dt, 0.5f, {0xe6, 0x1a, 0}},

		{t_fade_final - dt, 0.9f, {0x1a, 0xe6, 0}},
		{t_fade_final, 0.99f, {0, 0xff, 0x00}},
		{t_fade_final + dt, 1.0f, {0, 0xf7, 0x09}},

		// At its end an animation without forwards fill applies nothing.
		{t_colorize_final - dt, 1.0f, {0, 0x08, 0xf7}},
		{t_colorize_final, 1.0f, {0, 0, 0}},
		{t_colorize_final + dt, 1.0f, {0, 0, 0}},
	};

	{
		system_interface->SetManualTime(0.0);
		ElementDocument* document = context->LoadDocumentFromMemory(document_multiple_values_rml, "assets/");
		Element* element = TestsShell::GetElementChild(document, 0);

		document->Show();

		double t = 0.f;
		for (const auto& test : tests)
		{
			// Simulate progression in small time steps since animations are constrained to 0.1s maximum step size.
			while (t < test.time)
			{
				t = Math::Min(t + dt, test.time);
				system_interface->SetManualTime(t);
				context->Update();
			}

			INFO("Time: ", test.time);
			CHECK(element->GetProperty<float>("opacity") >= test.opacity_min);
			CHECK(element->GetProperty<Colourb>("background-color") == test.background_color);
		}

		document->Close();
	}

	TestsShell::ShutdownShell();
}

static const String document_animation_multiple_values_rml = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			left: 0;
			top: 0;
			right: 0;
			bottom: 0;
		}
		@keyframes fadein {
			from {
				opacity: 0;
				visibility: hidden;
			}
			to {
				opacity: 1;
				visibility: visible;
			}
		}
		@keyframes fadeout {
			from {
				opacity: 1;
				visibility: visible;
			}
			to {
				opacity: 0;
				visibility: hidden;
			}
		}

		div {
			background-color: #c66;
			width: 300dp;
			height: 300dp;
			margin: auto;
			animation: 1s linear fadein, 1s 5s linear fadeout;
		}
	</style>
</head>

<body>
	<div id="div"/>
</body>
</rml>
)";

TEST_CASE("animation.multiple_overlapping")
{
	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	Context* context = TestsShell::GetContext();

	const float greater_than_zero = FLT_MIN;

	const double dt = 1.0 / 60.0;
	const double t0 = 0;
	const double t_delta = 0.1;
	const double t_fadein_final = 1;
	const double t_fadeout_start = 5;
	const double t_fadeout_final = 6;

	struct TestCase {
		double time;
		float opacity_min;
		bool is_visible;
	};
	// The later animation of the list wins for the properties it animates while it applies a value; without fill modes,
	// neither applies anything outside its active interval (the element's own opacity is 1 and it is visible).
	const std::vector<TestCase> tests = {
		{t0, 0.f, false},
		{t0 + t_delta, 0.1f, true},
		{t_fadein_final - t_delta, 0.9f, true},
		{t_fadein_final, 1.f, true},
		{t_fadein_final + t_delta, 1.f, true},
		{t_fadeout_start - t_delta, 1.f, true},
		{t_fadeout_start, 1.f, true},
		{t_fadeout_start + t_delta, 0.9f, true},
		{t_fadeout_final - t_delta, 0.1f, true},
		{t_fadeout_final, 1.f, true},
		{t_fadeout_final + t_delta, 1.f, true},
	};
	(void)greater_than_zero;

	// Several animations of the same property on one element (6.3 warned and kept the first, #608) composite in list order.
	{
		system_interface->SetManualTime(0.0);

		ElementDocument* document = context->LoadDocumentFromMemory(document_animation_multiple_values_rml, "assets/");
		Element* element = TestsShell::GetElementChild(document, 0);

		document->Show();

		TestsShell::RenderLoop();

		double t = 0.f;
		for (const auto& test : tests)
		{
			while (t < test.time)
			{
				t = Math::Min(t + dt, test.time);
				system_interface->SetManualTime(t);
				context->Update();
			}

			INFO("Time: ", test.time);
			CHECK(element->GetProperty<float>("opacity") == doctest::Approx(test.opacity_min).epsilon(0.02));
			CHECK(element->IsVisible() == test.is_visible);
		}

		document->Close();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("transition.display_and_visibility")
{
	// Display and visibility properties have special behavior that make them visible throughout the interpolation duration.
	// Phase 3 (CSS): 'display' transitions only with 'allow-discrete', and an element shown from 'display: none' has no
	// before-change style: it fades in from its @starting-style.
	const String document_rml_template = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			inset: 0;
		}
		div {
			background-color: #c66;
			width: 300dp;
			height: 300dp;
			margin: auto;
			transition: opacity 1s linear, display 1s linear allow-discrete, visibility 1s linear;
		}
		.hide {
			%s;
			opacity: 0;
		}
		@starting-style {
			div { opacity: 0; }
		}
	</style>
</head>

<body>
	<div class="hide" id="div"/>
</body>
</rml>
)";

	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	Context* context = TestsShell::GetContext();

	String document_rml;
	bool visible_when_shown = false;

	SUBCASE("display: none")
	{
		document_rml = CreateString(document_rml_template.c_str(), "display: none");
		// Shown at once ('display' has no before-change value to transition from).
		visible_when_shown = true;
	}
	SUBCASE("visibility")
	{
		document_rml = CreateString(document_rml_template.c_str(), "visibility: hidden");
	}

	const double dt = 1.0 / 60.0;
	const double t_delta = 0.1;
	const double t_fadein0 = 1;
	const double t_fadein1 = 2;
	const double t_fadeout0 = 4;
	const double t_fadeout1 = 5;

	enum class Action { None, Show, Hide };

	struct TestCase {
		double time;
		float opacity;
		bool is_visible;
		Action action = Action::None;
	};
	const std::vector<TestCase> tests = {
		{t_fadein0 - t_delta, 0.f, false},
		{t_fadein0, 0.f, visible_when_shown, Action::Show},
		{t_fadein0 + t_delta, 0.1f, true},
		{t_fadein1 - t_delta, 0.9f, true},
		{t_fadein1, 1.f, true},
		{t_fadein1 + t_delta, 1.f, true},
		{t_fadeout0 - t_delta, 1.f, true},
		{t_fadeout0, 1.f, true, Action::Hide},
		{t_fadeout0 + t_delta, 0.9f, true},
		{t_fadeout1 - t_delta, 0.1f, true},
		{t_fadeout1 + dt, 0.f, false}, // Due to floating-point precision, the animation may end slightly after the exact endtime.
		{t_fadeout1 + t_delta, 0.f, false},
	};

	{
		system_interface->SetManualTime(0.0);

		ElementDocument* document = context->LoadDocumentFromMemory(document_rml, "assets/");
		Element* element = TestsShell::GetElementChild(document, 0);

		document->Show();

		double t = 0.f;
		for (const auto& test : tests)
		{
			while (t < test.time)
			{
				t = Math::Min(t + dt, test.time);
				system_interface->SetManualTime(t);
				TestsShell::RenderLoop(false);
			}

			if (test.action == Action::Show)
				element->SetClass("hide", false);
			else if (test.action == Action::Hide)
				element->SetClass("hide", true);

			context->Update();

			INFO("Time: ", test.time);
			CHECK(element->GetProperty<float>("opacity") == doctest::Approx(test.opacity));
			CHECK(element->IsVisible() == test.is_visible);
		}

		document->Close();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("animation.transform_interpolation")
{
	static const String document_rml_template = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body { inset: 0; }
		@keyframes move {
			from { transform: %s; }
			to   { transform: %s; }
		}
		div {
			width: 64px;
			height: 64px;
			animation: move 0.1s linear;
		}
	</style>
</head>

<body>
	<div/>
</body>
</rml>
)";

	struct Test {
		String from;
		String to;
		String expected_midpoint;
	};

	const Vector<Test> tests{
		{
			"translate(0px, 50px)",
			"translate3d(-600px, -200px, -200px) rotate3d(1, 0, 0, 60deg) skewX(-10deg)",
			"translate3d(-300px, -75px, -100px) rotate3d(1, 0, 0, 30deg) skewX(-5deg)",
		},
		{
			"translate3d(-600px, -200px, -200px) rotate3d(1, 0, 0, 60deg) skewX(-10deg)",
			"translate(0px, 50px)",
			"translate3d(-300px, -75px, -100px) rotate3d(1, 0, 0, 30deg) skewX(-5deg)",
		},
		{
			"none",
			"rotate3d(0, 1, 0, 60deg)",
			"rotate3d(0, 1, 0, 30deg)",
		},
		{
			"scale(3)",
			"scale(1) rotate3d(3, 4, 0, 90deg)",
			"scale(2, 2) rotate3d(0.6, 0.8, 0, 45deg)",
		},
	};

	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	Context* context = TestsShell::GetContext();

	for (const Test& test : tests)
	{
		const double t_final = 0.1;

		system_interface->SetManualTime(0.0);
		const String document_rml = CreateString(document_rml_template.c_str(), test.from.c_str(), test.to.c_str());

		ElementDocument* document = context->LoadDocumentFromMemory(document_rml, "assets/");
		document->Show();

		system_interface->SetManualTime(0.5 * t_final);
		context->Update();

		Element* element = TestsShell::GetElementChild(document, 0);
		String transform_str = element->GetProperty<String>("transform");
		CAPTURE(test.from);
		CAPTURE(test.to);
		CAPTURE(transform_str);
		CHECK(transform_str.find("decomposedMatrix3d") == String::npos);
		CHECK(transform_str == test.expected_midpoint);

		document->Close();
	}

	TestsShell::ShutdownShell();
}

// Phase 3 (docs/html-ui/phase-3-fork-patches.md 3.13.2-3.13.3, FORK.md P3-A1): the transition and animation shorthands and
// longhands, and the computed lists built from them.

namespace {

// The longhands a declaration sets, as "name: value" lines; "<invalid>" if it does not parse.
String ParsedLonghands(const String& name, const String& value)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value))
		return "<invalid>";
	String result;
	Vector<PropertyId> ids;
	for (const auto& id_property : dictionary.GetProperties())
		ids.push_back(id_property.first);
	std::sort(ids.begin(), ids.end());
	for (PropertyId id : ids)
		result += StyleSheetSpecification::GetPropertyName(id) + ": " + dictionary.GetProperty(id)->ToString() + "\n";
	return result;
}

String ParsedLonghand(const String& name, const String& value, PropertyId id)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value))
		return "<invalid>";
	const Property* property = dictionary.GetProperty(id);
	return property ? property->ToString() : "<unset>";
}

} // namespace

TEST_CASE("transition.parse")
{
	TestsShell::GetContext();

	CHECK(ParsedLonghands("transition", "opacity 200ms ease-in 0.1s") ==
		"transition-property: opacity\ntransition-duration: 0.2s\ntransition-timing-function: ease-in\ntransition-delay: 0.1s\n"
		"transition-behavior: normal\n");
	CHECK(ParsedLonghands("transition", "1s") ==
		"transition-property: all\ntransition-duration: 1s\ntransition-timing-function: ease\ntransition-delay: 0s\ntransition-behavior: normal\n");
	CHECK(ParsedLonghand("transition", "OPACITY 1S EASE-OUT", PropertyId::TransitionTimingFunction) == "ease-out");
	CHECK(ParsedLonghand("transition", "OPACITY 1S EASE-OUT", PropertyId::TransitionProperty) == "opacity");
	CHECK(ParsedLonghand("transition", "opacity 0s", PropertyId::TransitionDuration) == "0s");
	CHECK(ParsedLonghand("transition", "opacity 1s -0.5s", PropertyId::TransitionDelay) == "-0.5s");
	CHECK(ParsedLonghand("transition", "opacity 1s allow-discrete", PropertyId::TransitionBehavior) == "allow-discrete");
	CHECK(ParsedLonghand("transition", "allow-discrete display 1s", PropertyId::TransitionProperty) == "display");
	CHECK(ParsedLonghand("transition", "all 1s, transform 0.5s steps(4)", PropertyId::TransitionProperty) == "all, transform");
	CHECK(ParsedLonghand("transition", "all 1s, transform 0.5s steps(4)", PropertyId::TransitionTimingFunction) == "ease, steps(4)");
	CHECK(ParsedLonghand("transition", "none", PropertyId::TransitionProperty) == "none");
	CHECK(ParsedLonghand("transition", "unknown-property 1s", PropertyId::TransitionProperty) == "unknown-property");
	CHECK(ParsedLonghand("transition", "opacity 1s cubic-bezier(0.1, 0.2, 0.3, 0.4)", PropertyId::TransitionTimingFunction) ==
		"cubic-bezier(0.1, 0.2, 0.3, 0.4)");
	// RmlUi's syntax: several properties in one entry, with RmlUi's tween names.
	CHECK(ParsedLonghand("transition", "opacity width 1s cubic-in-out", PropertyId::TransitionProperty) == "opacity, width");
	CHECK(ParsedLonghand("transition", "opacity width 1s cubic-in-out", PropertyId::TransitionTimingFunction) == "cubic-in-out, cubic-in-out");

	for (const char* invalid : {"opacity 0", "opacity -1s", "opacity 1s 2s 3s", "none, opacity 1s", "opacity 1s ease linear", "opacity 1s, ",
			 "opacity 1px", "opacity 1s 0.5", "opacity all 1s", "inherit 1s"})
	{
		INFO(invalid);
		CHECK(ParsedLonghands("transition", invalid) == "<invalid>");
	}

	CHECK(ParsedLonghands("transition-duration", "1s, 200ms") == "transition-duration: 1s, 0.2s\n");
	CHECK(ParsedLonghands("transition-duration", "-1s") == "<invalid>");
	CHECK(ParsedLonghands("transition-delay", "-1s, 0s") == "transition-delay: -1s, 0s\n");
	CHECK(ParsedLonghands("transition-timing-function", "ease, steps(2, start), cubic-bezier(0, 0, 1, 1), linear(0, 1)") ==
		"transition-timing-function: ease, steps(2, jump-start), cubic-bezier(0, 0, 1, 1), linear(0 0%, 1 100%)\n");
	CHECK(ParsedLonghands("transition-property", "opacity, none") == "<invalid>");
	CHECK(ParsedLonghands("transition-property", "Opacity, all") == "transition-property: opacity, all\n");
	CHECK(ParsedLonghands("transition-behavior", "allow-discrete, NORMAL") == "transition-behavior: allow-discrete, normal\n");
	CHECK(ParsedLonghands("transition-behavior", "discrete") == "<invalid>");

	TestsShell::ShutdownShell();
}

TEST_CASE("animation.parse")
{
	TestsShell::GetContext();

	CHECK(ParsedLonghands("animation", "slide 1s ease-in 0.5s 3 reverse both paused") ==
		"animation-name: slide\nanimation-duration: 1s\nanimation-timing-function: ease-in\nanimation-delay: 0.5s\n"
		"animation-iteration-count: 3\nanimation-direction: reverse\nanimation-fill-mode: both\nanimation-play-state: paused\n");
	CHECK(ParsedLonghands("animation", "none") ==
		"animation-name: none\nanimation-duration: 0s\nanimation-timing-function: ease\nanimation-delay: 0s\n"
		"animation-iteration-count: 1\nanimation-direction: normal\nanimation-fill-mode: none\nanimation-play-state: running\n");
	// The name is the component no other longhand takes.
	CHECK(ParsedLonghand("animation", "1s linear linear", PropertyId::AnimationName) == "linear");
	CHECK(ParsedLonghand("animation", "1s linear linear", PropertyId::AnimationTimingFunction) == "linear");
	CHECK(ParsedLonghand("animation", "reverse 1s reverse", PropertyId::AnimationName) == "reverse");
	CHECK(ParsedLonghand("animation", "2.5 fade 1s", PropertyId::AnimationIterationCount) == "2.5");
	CHECK(ParsedLonghand("animation", "fade 1s INFINITE alternate-reverse", PropertyId::AnimationIterationCount) == "infinite");
	CHECK(ParsedLonghand("animation", "fade 1s INFINITE alternate-reverse", PropertyId::AnimationDirection) == "alternate-reverse");
	CHECK(ParsedLonghand("animation", "fade 200ms, other 2s 1s", PropertyId::AnimationDuration) == "0.2s, 2s");
	CHECK(ParsedLonghand("animation", "fade 200ms, other 2s 1s", PropertyId::AnimationDelay) == "0s, 1s");
	CHECK(ParsedLonghand("animation", "fade 0s", PropertyId::AnimationDuration) == "0s");
	CHECK(ParsedLonghand("animation", "fade 1s 0", PropertyId::AnimationIterationCount) == "0");

	for (const char* invalid : {"fade -1s", "fade 1s 1s 1s", "fade other 1s", "fade 1s -1", "fade 1px", "fade 1s, ", "initial 1s"})
	{
		INFO(invalid);
		CHECK(ParsedLonghands("animation", invalid) == "<invalid>");
	}

	CHECK(ParsedLonghands("animation-iteration-count", "infinite, 0, 2.5") == "animation-iteration-count: infinite, 0, 2.5\n");
	CHECK(ParsedLonghands("animation-iteration-count", "-1") == "<invalid>");
	CHECK(ParsedLonghands("animation-name", "fade, none, Other") == "animation-name: fade, none, Other\n");
	// 'initial' is not an animation name: it is the CSS-wide keyword (p3-cascade), valid on every property.
	CHECK(ParsedLonghands("animation-name", "initial") == "animation-name: initial\n");
	CHECK(ParsedLonghands("animation-name", "fade, initial") == "<invalid>");
	CHECK(ParsedLonghands("animation-direction", "alternate, REVERSE") == "animation-direction: alternate, reverse\n");
	CHECK(ParsedLonghands("animation-fill-mode", "both, forwards, backwards, none") == "animation-fill-mode: both, forwards, backwards, none\n");
	CHECK(ParsedLonghands("animation-play-state", "paused, running") == "animation-play-state: paused, running\n");
	CHECK(ParsedLonghands("animation-composition", "replace, add, accumulate") == "animation-composition: replace, add, accumulate\n");
	CHECK(ParsedLonghands("animation-delay", "-200ms") == "animation-delay: -0.2s\n");
	CHECK(ParsedLonghands("animation-duration", "-1s") == "<invalid>");

	TestsShell::ShutdownShell();
}

TEST_CASE("animation.computed_lists")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory("<rml><body/></rml>");
	REQUIRE(document);
	document->Show();
	Element* element = nullptr;
	// (each case on a new element: 6.3's style attribute does not remove the inline properties it no longer sets)
	auto new_element = [&]() { element = document->AppendChild(document->CreateElement("div")); };

	auto transitions = [&](const String& style) {
		new_element();
		element->SetAttribute("style", style);
		context->Update();
		String result;
		if (const TransitionList* list = element->GetComputedValues().transition())
			for (const Transition& t : list->transitions)
				result += (t.id == PropertyId::Invalid ? String("all") : StyleSheetSpecification::GetPropertyName(t.id)) +
					CreateString(" %g %g %s%s; ", t.duration, t.delay, t.tween.to_string().c_str(), t.allow_discrete ? " discrete" : "");
		return result;
	};
	auto animations = [&](const String& style) {
		new_element();
		element->SetAttribute("style", style);
		context->Update();
		String result;
		if (const AnimationList* list = element->GetComputedValues().animation())
			for (const Animation& a : *list)
				result += a.name +
					CreateString(" %g %g %g %d %d %d %s; ", a.duration, a.delay, a.iteration_count, (int)a.direction, (int)a.fill_mode, (int)a.paused,
						a.tween.to_string().c_str());
		return result;
	};

	// One entry per transition-property item; the other lists repeat, or are cut.
	CHECK(transitions("transition-property: opacity, width, height; transition-duration: 1s, 2s") ==
		"opacity 1 0 ease; width 2 0 ease; height 1 0 ease; ");
	CHECK(transitions("transition-property: opacity; transition-duration: 1s, 2s") == "opacity 1 0 ease; ");
	// A shorthand transitions its longhands; an unknown name keeps its place in the lists.
	CHECK(transitions("transition: margin 1s linear") ==
		"margin-top 1 0 linear; margin-right 1 0 linear; margin-bottom 1 0 linear; margin-left 1 0 linear; ");
	CHECK(transitions("transition-property: unknown, opacity; transition-duration: 1s, 2s") == "opacity 2 0 ease; ");
	// A combined duration of zero does nothing; a delay alone makes a transition.
	CHECK(transitions("transition: opacity 0s") == "");
	CHECK(transitions("transition: opacity 0s 1s") == "opacity 0 1 ease; ");
	CHECK(transitions("transition: all 1s, opacity 2s allow-discrete") == "all 1 0 ease; opacity 2 0 ease discrete; ");
	CHECK(transitions("transition: none") == "");
	CHECK(transitions("") == "");

	// One entry per animation-name item; 'none' items are left out.
	CHECK(animations("animation-name: a, none, b; animation-duration: 1s, 2s") == "a 1 0 1 0 0 0 ease; b 1 0 1 0 0 0 ease; ");
	CHECK(animations("animation: spin 2s linear -1s infinite alternate forwards paused") == "spin 2 -1 inf 2 1 1 linear; ");
	CHECK(animations("animation: none") == "");

	const TransitionList* list = nullptr;
	new_element();
	element->SetAttribute("style", "transition: all 1s, opacity 2s");
	context->Update();
	list = element->GetComputedValues().transition();
	REQUIRE(list);
	CHECK(list->all);
	REQUIRE(list->Find(PropertyId::Opacity));
	CHECK(list->Find(PropertyId::Opacity)->duration == 2.f);
	REQUIRE(list->Find(PropertyId::Width));
	CHECK(list->Find(PropertyId::Width)->duration == 1.f);

	document->Close();
	TestsShell::ShutdownShell();
}

// Phase 3 (docs/html-ui/phase-3-fork-patches.md 3.13.5, FORK.md P3-A2): the animation layers and the timing model of CSS
// animations: fill modes, direction, fractional iterations, play state, per-keyframe easing, events, display.

namespace {

const String timing_rml_head = R"(
<rml>
<head>
	<style>
		body { inset: 0; }
		div { display: block; width: 10px; height: 10px; opacity: 1; }
		@keyframes fade { from { opacity: 0.2; } to { opacity: 0.8; } }
)";

const String timing_rml_tail = R"(
	</style>
</head>
<body>
	<div id="a"/><div id="b"/><div id="c"/><div id="d"/><div id="e"/>
</body>
</rml>
)";

// Steps the manual clock to a time (in 0.05 s steps) and updates the context.
struct Clock {
	Context* context;
	TestsSystemInterface* system;
	double t = 0.0;
	void AdvanceTo(double time)
	{
		while (t < time)
		{
			t = Math::Min(t + 0.05, time);
			system->SetManualTime(t);
			context->Update();
		}
	}
	void JumpTo(double time)
	{
		t = time;
		system->SetManualTime(t);
		context->Update();
	}
};

float Opacity(Element* element)
{
	return element->GetComputedValues().opacity();
}

class RecordingListener : public EventListener {
public:
	void ProcessEvent(Event& event) override
	{
		String text = event.GetType();
		const String name = event.GetParameter<String>("animationName", event.GetParameter<String>("propertyName", ""));
		text += " " + name + CreateString(" %g", event.GetParameter<float>("elapsedTime", -1.f));
		log.push_back(text);
	}
	StringList log;
};

ElementDocument* LoadTimingDocument(Context* context, const String& style)
{
	ElementDocument* document = context->LoadDocumentFromMemory(timing_rml_head + style + timing_rml_tail);
	REQUIRE(document);
	document->Show();
	return document;
}

} // namespace

TEST_CASE("animation.fill_modes")
{
	Context* context = TestsShell::GetContext();
	Clock clock{context, TestsShell::GetTestsSystemInterface()};
	clock.system->SetManualTime(0.0);
	ElementDocument* document = LoadTimingDocument(context, R"(
		#a { animation: fade 1s linear 1s; }
		#b { animation: fade 1s linear 1s forwards; }
		#c { animation: fade 1s linear 1s backwards; }
		#d { animation: fade 1s linear 1s both; }
	)");
	Element* none = document->GetElementById("a");
	Element* forwards = document->GetElementById("b");
	Element* backwards = document->GetElementById("c");
	Element* both = document->GetElementById("d");

	clock.AdvanceTo(0.5);
	CHECK(Opacity(none) == doctest::Approx(1.f));
	CHECK(Opacity(forwards) == doctest::Approx(1.f));
	CHECK(Opacity(backwards) == doctest::Approx(0.2f));
	CHECK(Opacity(both) == doctest::Approx(0.2f));

	clock.AdvanceTo(1.5);
	for (Element* element : {none, forwards, backwards, both})
		CHECK(Opacity(element) == doctest::Approx(0.5f));

	clock.AdvanceTo(3.0);
	CHECK(Opacity(none) == doctest::Approx(1.f));
	CHECK(Opacity(forwards) == doctest::Approx(0.8f));
	CHECK(Opacity(backwards) == doctest::Approx(1.f));
	CHECK(Opacity(both) == doctest::Approx(0.8f));

	// The values live in the animation layer: the inline style is never written.
	CHECK(forwards->GetLocalStyleProperties().empty());
	CHECK(forwards->GetLocalProperty(PropertyId::Opacity));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("animation.direction_and_iterations")
{
	Context* context = TestsShell::GetContext();
	Clock clock{context, TestsShell::GetTestsSystemInterface()};
	clock.system->SetManualTime(0.0);
	ElementDocument* document = LoadTimingDocument(context, R"(
		#a { animation: fade 1s linear reverse; }
		#b { animation: fade 1s linear 2 alternate; }
		#c { animation: fade 1s linear 2 alternate-reverse forwards; }
		#d { animation: fade 1s linear 1.5 forwards; }
		#e { animation: fade 1s linear 0 both; }
	)");
	Element* reverse = document->GetElementById("a");
	Element* alternate = document->GetElementById("b");
	Element* alternate_reverse = document->GetElementById("c");
	Element* fractional = document->GetElementById("d");
	Element* zero = document->GetElementById("e");

	CHECK(Opacity(reverse) == doctest::Approx(0.8f));
	CHECK(Opacity(alternate_reverse) == doctest::Approx(0.8f));
	CHECK(Opacity(zero) == doctest::Approx(0.2f));

	clock.AdvanceTo(0.25);
	CHECK(Opacity(reverse) == doctest::Approx(0.65f));
	CHECK(Opacity(alternate) == doctest::Approx(0.35f));
	CHECK(Opacity(alternate_reverse) == doctest::Approx(0.65f));

	clock.AdvanceTo(1.25);
	CHECK(Opacity(reverse) == doctest::Approx(1.f));
	CHECK(Opacity(alternate) == doctest::Approx(0.65f));
	CHECK(Opacity(alternate_reverse) == doctest::Approx(0.35f));
	CHECK(Opacity(fractional) == doctest::Approx(0.35f));

	clock.AdvanceTo(3.0);
	CHECK(Opacity(alternate) == doctest::Approx(1.f));
	CHECK(Opacity(alternate_reverse) == doctest::Approx(0.8f));
	// A fractional count ends at its fractional progress.
	CHECK(Opacity(fractional) == doctest::Approx(0.5f));
	CHECK(Opacity(zero) == doctest::Approx(0.2f));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("animation.play_state")
{
	Context* context = TestsShell::GetContext();
	Clock clock{context, TestsShell::GetTestsSystemInterface()};
	clock.system->SetManualTime(0.0);
	ElementDocument* document = LoadTimingDocument(context, R"(
		#a { animation: fade 1s linear forwards; }
		#b { animation: fade 1s linear forwards paused; }
		#a.paused { animation-play-state: paused; }
		#b.running { animation-play-state: running; }
	)");
	Element* element = document->GetElementById("a");
	Element* starts_paused = document->GetElementById("b");

	clock.AdvanceTo(0.25);
	CHECK(Opacity(element) == doctest::Approx(0.35f));
	CHECK(Opacity(starts_paused) == doctest::Approx(0.2f));

	// Pausing holds the animation's time from the style change on; changing only the play state does not restart it.
	element->SetClass("paused", true);
	context->Update();
	clock.AdvanceTo(2.0);
	CHECK(Opacity(element) == doctest::Approx(0.35f));
	element->SetClass("paused", false);
	context->Update();
	clock.AdvanceTo(2.25);
	CHECK(Opacity(element) == doctest::Approx(0.5f));

	// An animation paused from its start runs from its start when resumed.
	CHECK(Opacity(starts_paused) == doctest::Approx(0.2f));
	starts_paused->SetClass("running", true);
	context->Update();
	clock.AdvanceTo(2.75);
	CHECK(Opacity(starts_paused) == doctest::Approx(0.5f));
	clock.AdvanceTo(5.0);
	CHECK(Opacity(element) == doctest::Approx(0.8f));
	CHECK(Opacity(starts_paused) == doctest::Approx(0.8f));

	// A change of the duration updates the running animation in place.
	element->SetProperty("animation-duration", "2s");
	clock.AdvanceTo(5.1);
	CHECK(Opacity(element) == doctest::Approx(0.8f));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("animation.keyframes")
{
	Context* context = TestsShell::GetContext();
	Clock clock{context, TestsShell::GetTestsSystemInterface()};
	clock.system->SetManualTime(0.0);
	ElementDocument* document = LoadTimingDocument(context, R"(
		@keyframes stepped {
			0% { opacity: 0; animation-timing-function: steps(2); }
			50% { opacity: 0.5; animation-timing-function: linear; }
			100% { opacity: 1; }
		}
		@keyframes merged {
			0%, 100% { opacity: 0.5; }
			50% { opacity: 1; }
			50% { width: 30px; }
		}
		@keyframes partial {
			50% { opacity: 0; }
		}
		#a { animation: stepped 1s ease; }
		#b { animation: merged 1s linear; }
		#c { animation: partial 1s linear; opacity: 0.4; }
	)");
	Element* stepped = document->GetElementById("a");
	Element* merged = document->GetElementById("b");
	Element* partial = document->GetElementById("c");

	// A segment is eased by its start keyframe's timing function; keyframes without one take the animation's.
	clock.AdvanceTo(0.2);
	CHECK(Opacity(stepped) == doctest::Approx(0.f));
	clock.AdvanceTo(0.3);
	CHECK(Opacity(stepped) == doctest::Approx(0.25f));
	clock.AdvanceTo(0.75);
	CHECK(Opacity(stepped) == doctest::Approx(0.75f));

	// A keyframe with several selectors, and two blocks of one selector cascading.
	CHECK(Opacity(merged) == doctest::Approx(1.f - 0.25f));
	CHECK(merged->GetBox().GetSize().x == doctest::Approx(10.f + 20.f * 0.5f).epsilon(0.05));

	// Missing 0% and 100% keyframes take the underlying value.
	CHECK(Opacity(partial) == doctest::Approx(0.2f));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("animation.events")
{
	Context* context = TestsShell::GetContext();
	Clock clock{context, TestsShell::GetTestsSystemInterface()};
	clock.system->SetManualTime(0.0);
	ElementDocument* document = LoadTimingDocument(context, R"(
		#a { animation: fade 1s linear 0.5s 2; }
		#b.go { animation: fade 1s linear; }
		#c { animation: fade 1s linear; }
	)");
	Element* element = document->GetElementById("a");
	Element* jumping = document->GetElementById("b");
	Element* cancelled = document->GetElementById("c");
	RecordingListener listener;
	for (Element* e : {element, jumping, cancelled})
		for (const char* type : {"animationstart", "animationiteration", "animationend", "animationcancel"})
			e->AddEventListener(type, &listener);

	clock.AdvanceTo(0.25);
	CHECK(listener.log.empty());
	clock.AdvanceTo(0.75);
	REQUIRE(listener.log.size() == 1);
	CHECK(listener.log[0] == "animationstart fade 0");

	// Removing a running animation cancels it.
	cancelled->SetProperty("animation", "none");
	clock.AdvanceTo(0.8);
	REQUIRE(listener.log.size() == 2);
	CHECK(listener.log[1].substr(0, 20) == "animationcancel fade");

	clock.AdvanceTo(1.75);
	REQUIRE(listener.log.size() == 3);
	CHECK(listener.log[2] == "animationiteration fade 1");
	clock.AdvanceTo(3.0);
	REQUIRE(listener.log.size() == 4);
	CHECK(listener.log[3] == "animationend fade 2");

	// No time step clamp: one update past the whole animation runs it to its end, with both events.
	jumping->SetClass("go", true);
	clock.JumpTo(3.0);
	clock.JumpTo(10.0);
	REQUIRE(listener.log.size() == 6);
	CHECK(listener.log[4] == "animationstart fade 0");
	CHECK(listener.log[5] == "animationend fade 1");

	for (Element* e : {element, jumping, cancelled})
		for (const char* type : {"animationstart", "animationiteration", "animationend", "animationcancel"})
			e->RemoveEventListener(type, &listener);
	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("animation.display_none")
{
	Context* context = TestsShell::GetContext();
	Clock clock{context, TestsShell::GetTestsSystemInterface()};
	clock.system->SetManualTime(0.0);
	ElementDocument* document = context->LoadDocumentFromMemory(R"(
<rml>
<head>
	<style>
		@keyframes fade { from { opacity: 0.2; } to { opacity: 0.8; } }
		div { display: block; width: 10px; height: 10px; }
		#child { animation: fade 1s linear forwards; }
		.hidden { display: none; }
	</style>
</head>
<body><div id="parent"><div id="child"/></div></body>
</rml>)");
	REQUIRE(document);
	document->Show();
	Element* parent = document->GetElementById("parent");
	Element* child = document->GetElementById("child");
	RecordingListener listener;
	child->AddEventListener("animationcancel", &listener);

	clock.AdvanceTo(0.5);
	CHECK(Opacity(child) == doctest::Approx(0.5f));

	// An ancestor set to 'display: none' cancels the animation; shown again, the animation starts over.
	parent->SetClass("hidden", true);
	clock.AdvanceTo(0.6);
	REQUIRE(listener.log.size() == 1);
	CHECK(Opacity(child) == doctest::Approx(1.f));
	clock.AdvanceTo(2.0);
	parent->SetClass("hidden", false);
	clock.AdvanceTo(2.05);
	clock.AdvanceTo(2.55);
	CHECK(Opacity(child) == doctest::Approx(0.5f).epsilon(0.1));

	child->RemoveEventListener("animationcancel", &listener);
	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("animation.inline_style_kept")
{
	Context* context = TestsShell::GetContext();
	Clock clock{context, TestsShell::GetTestsSystemInterface()};
	clock.system->SetManualTime(0.0);
	ElementDocument* document = LoadTimingDocument(context, R"(
		#a { animation: fade 1s linear; }
	)");
	Element* element = document->GetElementById("a");
	element->SetProperty("opacity", "0.3");
	clock.AdvanceTo(0.5);
	CHECK(Opacity(element) == doctest::Approx(0.5f));
	REQUIRE(element->GetLocalStyleProperties().count(PropertyId::Opacity) == 1);
	CHECK(element->GetLocalStyleProperties().find(PropertyId::Opacity)->second.Get<float>() == 0.3f);
	clock.AdvanceTo(1.5);
	// The animation ended (no fill): the inline value the author wrote applies again.
	CHECK(Opacity(element) == doctest::Approx(0.3f));

	document->Close();
	TestsShell::ShutdownShell();
}

// Phase 3 (docs/html-ui/phase-3-fork-patches.md 3.13.6, FORK.md P3-A4): box shadows interpolate, colours in premultiplied
// sRGB.
TEST_CASE("animation.box_shadow_and_colours")
{
	Context* context = TestsShell::GetContext();
	Clock clock{context, TestsShell::GetTestsSystemInterface()};
	clock.system->SetManualTime(0.0);
	ElementDocument* document = LoadTimingDocument(context, R"(
		@keyframes shadow { from { box-shadow: 0px 0px 0px 0px #000; } to { box-shadow: 10px 10px 4px 2px #f00; } }
		@keyframes padded { from { box-shadow: none; } to { box-shadow: 10px 10px 0px 0px #f00, 4px 4px #00f inset; } }
		@keyframes inset { from { box-shadow: 10px 10px #f00; } to { box-shadow: 10px 10px #f00 inset; } }
		@keyframes colour { from { background-color: #ff000000; } to { background-color: #0000ff; } }
		#a { animation: shadow 1s linear; }
		#b { animation: padded 1s linear; }
		#c { animation: inset 1s linear; }
		#d { animation: colour 1s linear; }
	)");
	auto shadows = [&](const char* id) {
		const Property* property = document->GetElementById(id)->GetProperty(PropertyId::BoxShadow);
		REQUIRE(property);
		REQUIRE(property->unit == Unit::BOXSHADOWLIST);
		return property->value.GetReference<BoxShadowList>();
	};

	clock.AdvanceTo(0.5);
	{
		const BoxShadowList list = shadows("a");
		REQUIRE(list.size() == 1);
		CHECK(list[0].offset_x.number == doctest::Approx(5.f));
		CHECK(list[0].offset_y.number == doctest::Approx(5.f));
		CHECK(list[0].blur_radius.number == doctest::Approx(2.f));
		CHECK(list[0].spread_distance.number == doctest::Approx(1.f));
		CHECK(list[0].color == Colourb(128, 0, 0).ToPremultiplied());
	}
	{
		// The shorter list is padded with transparent zero shadows of the same inset.
		const BoxShadowList list = shadows("b");
		REQUIRE(list.size() == 2);
		CHECK(list[0].offset_x.number == doctest::Approx(5.f));
		CHECK(list[0].color.alpha == 128);
		CHECK(list[0].color.red == 128);
		CHECK(list[1].inset);
		CHECK(list[1].offset_x.number == doctest::Approx(2.f));
	}
	// Inset against outset: discrete, switching at half way.
	CHECK(shadows("c")[0].inset);
	// A transparent end keeps the other colour's hue.
	CHECK(document->GetElementById("d")->GetComputedValues().background_color() == Colourb(0, 0, 255, 128));

	document->Close();
	TestsShell::ShutdownShell();
}
