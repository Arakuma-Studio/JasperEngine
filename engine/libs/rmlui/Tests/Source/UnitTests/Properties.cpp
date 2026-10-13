#include "../Common/TestsInterface.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleSheetTypes.h>
#include <doctest.h>

using namespace Rml;

// A colour stop with a fixed colour (currentcolor and its color-mix() at their defaults).
static ColorStop MakeColorStop(ColourbPremultiplied color, NumericValue position)
{
	ColorStop stop;
	stop.color = color;
	stop.position = position;
	return stop;
}

TEST_CASE("Properties")
{
	const Vector2i window_size(1024, 768);

	TestsSystemInterface system_interface;
	TestsRenderInterface render_interface;

	SetRenderInterface(&render_interface);
	SetSystemInterface(&system_interface);

	Rml::Initialise();

	Context* context = Rml::CreateContext("main", window_size);
	ElementDocument* document = context->CreateDocument();

	SUBCASE("inset")
	{
		struct InsetTestCase {
			String inset_value;

			struct ExpectedValues {
				String top;
				String right;
				String bottom;
				String left;
			} expected;
		};

		InsetTestCase tests[] = {
			{"auto", {"auto", "auto", "auto", "auto"}},
			{"0", {"0px", "0px", "0px", "0px"}},
			{"1px", {"1px", "1px", "1px", "1px"}},
			{"1dp", {"1dp", "1dp", "1dp", "1dp"}},
			{"1%", {"1%", "1%", "1%", "1%"}},
			{"10px 20px", {"10px", "20px", "10px", "20px"}},
			{"10px 20px 30px", {"10px", "20px", "30px", "20px"}},
			{"10px 20px 30px 40px", {"10px", "20px", "30px", "40px"}},
		};

		for (const InsetTestCase& test : tests)
		{
			CHECK(document->SetProperty("inset", test.inset_value));

			CHECK(document->GetProperty("top")->ToString() == test.expected.top);
			CHECK(document->GetProperty("right")->ToString() == test.expected.right);
			CHECK(document->GetProperty("bottom")->ToString() == test.expected.bottom);
			CHECK(document->GetProperty("left")->ToString() == test.expected.left);
		}
	}

	SUBCASE("flex")
	{
		struct FlexTestCase {
			String flex_value;

			struct ExpectedValues {
				float flex_grow;
				float flex_shrink;
				String flex_basis;
			} expected;
		};

		FlexTestCase tests[] = {
			{"", {0.f, 1.f, "auto"}},
			{"none", {0.f, 0.f, "auto"}},
			{"auto", {1.f, 1.f, "auto"}},
			{"1", {1.f, 1.f, "0px"}},
			{"2", {2.f, 1.f, "0px"}},
			{"2 0", {2.f, 0.f, "0px"}},
			{"2 3", {2.f, 3.f, "0px"}},
			{"2 auto", {2.f, 1.f, "auto"}},
			{"2 0 auto", {2.f, 0.f, "auto"}},
			{"0 0 auto", {0.f, 0.f, "auto"}},
			{"0 0 50px", {0.f, 0.f, "50px"}},
			{"0 0 50px", {0.f, 0.f, "50px"}},
			{"0 0 0", {0.f, 0.f, "0px"}},
		};

		for (const FlexTestCase& test : tests)
		{
			if (!test.flex_value.empty())
			{
				CHECK(document->SetProperty("flex", test.flex_value));
			}

			CHECK(document->GetProperty<float>("flex-grow") == test.expected.flex_grow);
			CHECK(document->GetProperty<float>("flex-shrink") == test.expected.flex_shrink);
			CHECK(document->GetProperty("flex-basis")->ToString() == test.expected.flex_basis);
		}
	}

	SUBCASE("gradient")
	{
		auto ParseGradient = [&](const String& value) -> Property {
			document->SetProperty("decorator", "linear-gradient(" + value + ")");
			auto decorators = document->GetProperty<DecoratorsPtr>("decorator");
			if (!decorators || decorators->list.size() != 1)
				return {};
			for (auto& id_property : decorators->list.front().properties.GetProperties())
			{
				if (id_property.second.unit == Unit::COLORSTOPLIST)
					return id_property.second;
			}
			return {};
		};

		struct GradientTestCase {
			String value;
			String expected_parsed_string;
			ColorStopList expected_color_stops;
		};

		GradientTestCase test_cases[] = {
			{
				"red, blue",
				"#ff0000, #0000ff",
				{
					MakeColorStop(ColourbPremultiplied(255, 0, 0), NumericValue{}),
					MakeColorStop(ColourbPremultiplied(0, 0, 255), NumericValue{}),
				},
			},
			{
				"red 5px, blue 50%",
				"#ff0000 5px, #0000ff 50%",
				{
					MakeColorStop(ColourbPremultiplied(255, 0, 0), NumericValue{5.f, Unit::PX}),
					MakeColorStop(ColourbPremultiplied(0, 0, 255), NumericValue{50.f, Unit::PERCENT}),
				},
			},
			{
				// CSS alpha (phase 3, values): 0 to 1, larger values are clamped (6.3 read 0-255).
				"red, #00f 50%, rgba(0, 255,0, 150) 10dp",
				"#ff0000, #0000ff 50%, #00ff00 10dp",
				{
					MakeColorStop(ColourbPremultiplied(255, 0, 0), NumericValue{}),
					MakeColorStop(ColourbPremultiplied(0, 0, 255), NumericValue{50.f, Unit::PERCENT}),
					MakeColorStop(ColourbPremultiplied(0, 255, 0, 255), NumericValue{10.f, Unit::DP}),
				},
			},
			{
				"hsl(10000, 0%, 50%), hsl(240, 100%, 50%) 50%, hsla(-240, 100%, 50%, 0.5) 10dp",
				// Channels are rounded (phase 3, values; 6.3 truncated).
				"#808080, #0000ff 50%, #00ff0080 10dp",
				{
					MakeColorStop(ColourbPremultiplied(128, 128, 128), NumericValue{}),
					MakeColorStop(ColourbPremultiplied(0, 0, 255), NumericValue{50.f, Unit::PERCENT}),
					MakeColorStop(ColourbPremultiplied(0, 128, 0, 128), NumericValue{10.f, Unit::DP}),
				},
			},
			{
				"lab(55% none none), lab(30% 67% -110) 50%, lab(90% -90 80 / 0.5) 10dp",
				// CSS Color 4 Lab (D50, phase 3, values; 6.3 converted with a D65 white), as Chromium.
				"#848484, #5800fd 50%, #00ff1780 10dp",
				{
					MakeColorStop(ColourbPremultiplied(132, 132, 132), NumericValue{}),
					MakeColorStop(ColourbPremultiplied(88, 0, 253), NumericValue{50.f, Unit::PERCENT}),
					MakeColorStop(ColourbPremultiplied(0, 128, 12, 128), NumericValue{10.f, Unit::DP}),
				},
			},
			{
				"lch(55% none 300.0), lch(30% 85% 180.0) 50%, lch(90% 90 100.0 / 50%) 10dp",
				"#848484, #006144 50%, #e5e70080 10dp",
				{
					MakeColorStop(ColourbPremultiplied(132, 132, 132), NumericValue{}),
					MakeColorStop(ColourbPremultiplied(0, 97, 68), NumericValue{50.f, Unit::PERCENT}),
					MakeColorStop(ColourbPremultiplied(115, 116, 0, 128), NumericValue{10.f, Unit::DP}),
				},
			},
			{
				"oklab(85% -50% 70%), oklab(0.2 0.4 -0.4) 50%, oklab(100% none 0.4 / 0.25) 10dp",
				"#98ed00, #6700c1 50%, #ffdf0040 10dp",
				{
					MakeColorStop(ColourbPremultiplied(152, 237, 0), NumericValue{}),
					MakeColorStop(ColourbPremultiplied(103, 0, 193), NumericValue{50.f, Unit::PERCENT}),
					MakeColorStop(ColourbPremultiplied(64, 56, 0, 64), NumericValue{10.f, Unit::DP}),
				},
			},
			{
				"oklch(75% 100% 30.0), oklch(0.5 0.2 270) 50%, oklch(1.0 0.1 none / 0.5) 10dp",
				"#ff0000, #3b51d3 50%, #ffe1fb80 10dp",
				{
					MakeColorStop(ColourbPremultiplied(255, 0, 0), NumericValue{}),
					MakeColorStop(ColourbPremultiplied(59, 81, 211), NumericValue{50.f, Unit::PERCENT}),
					MakeColorStop(ColourbPremultiplied(128, 113, 126, 128), NumericValue{10.f, Unit::DP}),
				},
			},
			{
				"red 50px 20%, blue 10in",
				"#ff0000 50px, #ff0000 20%, #0000ff 10in",
				{
					MakeColorStop(ColourbPremultiplied(255, 0, 0), NumericValue{50.f, Unit::PX}),
					MakeColorStop(ColourbPremultiplied(255, 0, 0), NumericValue{20.f, Unit::PERCENT}),
					MakeColorStop(ColourbPremultiplied(0, 0, 255), NumericValue{10.f, Unit::INCH}),
				},
			},
		};

		for (const GradientTestCase& test_case : test_cases)
		{
			const Property result = ParseGradient(test_case.value);
			CHECK(result.ToString() == test_case.expected_parsed_string);
			CHECK(result.Get<ColorStopList>() == test_case.expected_color_stops);
		}
	}

	Rml::Shutdown();
}

TEST_CASE("Property.ToString")
{
	TestsSystemInterface system_interface;
	TestsRenderInterface render_interface;
	SetRenderInterface(&render_interface);
	SetSystemInterface(&system_interface);

	Rml::Initialise();

	CHECK(Property(5.2f, Unit::CM).ToString() == "5.2cm");
	CHECK(Property(150, Unit::PERCENT).ToString() == "150%");
	CHECK(Property(Colourb{170, 187, 204, 255}, Unit::COLOUR).ToString() == "#aabbcc");

	auto ParsedValue = [](const String& name, const String& value) -> String {
		PropertyDictionary properties;
		StyleSheetSpecification::ParsePropertyDeclaration(properties, name, value);
		REQUIRE(properties.GetNumProperties() == 1);
		return properties.GetProperties().begin()->second.ToString();
	};

	CHECK(ParsedValue("width", "10px") == "10px");
	CHECK(ParsedValue("width", "10.00em") == "10em");
	CHECK(ParsedValue("width", "auto") == "auto");

	CHECK(ParsedValue("background-color", "#abc") == "#aabbcc");
	CHECK(ParsedValue("background-color", "red") == "#ff0000");

	CHECK(ParsedValue("transform", "translateX(10px)") == "translateX(10px)");
	CHECK(ParsedValue("transform", "translate(20in, 50em)") == "translate(20in, 50em)");

	// A shadow without a color is 'currentcolor' (CSS; phase 3, values).
	CHECK(ParsedValue("box-shadow", "2px 2px 0px, rgba(0, 0, 255, 255) 4px 4px 2em") == "currentcolor 2px 2px 0px, #0000ff 4px 4px 2em");
	CHECK(ParsedValue("box-shadow", "2px 2px 0px, #00ff 4px 4px 2em") == "currentcolor 2px 2px 0px, #0000ff 4px 4px 2em");

	// Due to conversion to and from premultiplied alpha, some color information is lost.
	CHECK(ParsedValue("box-shadow", "#fff0 2px 2px 0px") == "#00000000 2px 2px 0px");

	CHECK(ParsedValue("decorator", "linear-gradient(110deg, #fff3, #fff 10%, #c33 250dp, #3c3, #33c, #000 90%, #0003) border-box") ==
		"linear-gradient(110deg, #fff3, #fff 10%, #c33 250dp, #3c3, #33c, #000 90%, #0003) border-box");

	CHECK(ParsedValue("filter", "drop-shadow(#000 30px 20px 5px) opacity(0.2) sepia(0.2)") ==
		"drop-shadow(#000 30px 20px 5px) opacity(0.2) sepia(0.2)");

	Rml::Shutdown();
}
