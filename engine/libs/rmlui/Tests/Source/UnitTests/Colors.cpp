#include "../Common/TestsShell.h"
#include <RmlUi/Core/Colour.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/TypeConverter.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 values (docs/html-ui/phase-3-fork-patches.md 3.3.2): CSS Color 4 and color-mix() of CSS Color 5.

namespace {

bool Parse(const char* value, Colourb& colour)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "color", value))
		return false;
	const Property* property = dictionary.GetProperty(PropertyId::Color);
	if (!property || property->unit != Unit::COLOUR)
		return false;
	colour = property->Get<Colourb>();
	return true;
}

// Within 'tolerance' per channel (conversions through other spaces round).
void CheckColour(const char* value, Colourb expected, int tolerance = 0)
{
	INFO(String(value));
	Colourb colour;
	REQUIRE(Parse(value, colour));
	for (int i = 0; i < 4; i++)
	{
		INFO("channel " << i << ": " << int(colour[i]) << " expected " << int(expected[i]));
		CHECK(Math::Absolute(int(colour[i]) - int(expected[i])) <= tolerance);
	}
}

void CheckInvalid(const char* value)
{
	INFO(String(value));
	Colourb colour;
	CHECK_FALSE(Parse(value, colour));
}

} // namespace

TEST_CASE("Colors.hex_and_names")
{
	TestsShell::GetContext();
	TestsShell::SetNumExpectedWarnings(0);

	CheckColour("#f00", Colourb(255, 0, 0));
	CheckColour("#f008", Colourb(255, 0, 0, 136));
	CheckColour("#00ff00", Colourb(0, 255, 0));
	CheckColour("#0000ff80", Colourb(0, 0, 255, 128));
	CheckInvalid("#ff");
	CheckInvalid("#gg0000");

	CheckColour("red", Colourb(255, 0, 0));
	CheckColour("RED", Colourb(255, 0, 0));
	CheckColour("RebeccaPurple", Colourb(102, 51, 153));
	CheckColour("aliceblue", Colourb(240, 248, 255));
	CheckColour("cornflowerblue", Colourb(100, 149, 237));
	CheckColour("darkslategrey", Colourb(47, 79, 79));
	CheckColour("yellowgreen", Colourb(154, 205, 50));
	CheckColour("transparent", Colourb(0, 0, 0, 0));
	CheckInvalid("notacolor");
}

TEST_CASE("Colors.rgb")
{
	TestsShell::GetContext();

	// Comma syntax: numbers or percentages, the alpha 0..1 or a percentage.
	CheckColour("rgb(255, 128, 0)", Colourb(255, 128, 0));
	CheckColour("rgba(255, 128, 0, 0.5)", Colourb(255, 128, 0, 128));
	CheckColour("rgba(255, 128, 0, .5)", Colourb(255, 128, 0, 128));
	CheckColour("rgba(255, 128, 0, 50%)", Colourb(255, 128, 0, 128));
	CheckColour("rgb(255, 128, 0, 0.25)", Colourb(255, 128, 0, 64));
	CheckColour("rgb(100%, 50%, 0%)", Colourb(255, 128, 0));
	CheckColour("RGB(255,0,0)", Colourb(255, 0, 0));
	CheckInvalid("rgb(100%, 128, 0)");
	CheckInvalid("rgb(255, 0)");
	CheckInvalid("rgb(none, 0, 0)");

	// Space syntax.
	CheckColour("rgb(255 128 0)", Colourb(255, 128, 0));
	CheckColour("rgb(255 128 0 / 0.5)", Colourb(255, 128, 0, 128));
	CheckColour("rgb(255 128 0/50%)", Colourb(255, 128, 0, 128));
	CheckColour("rgba(0 0 255 / 25%)", Colourb(0, 0, 255, 64));
	CheckColour("rgb(none 255 none)", Colourb(0, 255, 0));
	CheckColour("rgb(255 0 0 / none)", Colourb(255, 0, 0, 0));
	CheckColour("rgb(100% 50% 0)", Colourb(255, 128, 0));
	CheckInvalid("rgb(255 0 0 0)");
	CheckInvalid("rgb(255 0 / 0)");

	// Out of range components are clamped.
	CheckColour("rgb(300 -20 127.5)", Colourb(255, 0, 128));
	CheckColour("rgba(0, 0, 0, 2)", Colourb(0, 0, 0, 255));
	CheckColour("rgba(0, 0, 0, -1)", Colourb(0, 0, 0, 0));
}

TEST_CASE("Colors.hsl_hwb")
{
	TestsShell::GetContext();

	CheckColour("hsl(0, 100%, 50%)", Colourb(255, 0, 0));
	CheckColour("hsla(120, 100%, 50%, 0.5)", Colourb(0, 255, 0, 128));
	CheckColour("hsl(240deg 100% 50%)", Colourb(0, 0, 255));
	CheckColour("hsl(0.5turn 100% 50%)", Colourb(0, 255, 255));
	CheckColour("hsl(200grad 100% 50%)", Colourb(0, 255, 255));
	CheckColour("hsl(3.14159rad 100% 50%)", Colourb(0, 255, 255));
	CheckColour("hsl(120 100 25 / 50%)", Colourb(0, 128, 0, 128));
	CheckColour("hsl(none 0% 50%)", Colourb(128, 128, 128));
	CheckColour("hsl(-120, 100%, 50%)", Colourb(0, 0, 255));
	CheckInvalid("hsl(0, 100, 50)");

	CheckColour("hwb(0 0% 0%)", Colourb(255, 0, 0));
	CheckColour("hwb(120 20% 20%)", Colourb(51, 204, 51));
	CheckColour("hwb(0 60% 60%)", Colourb(128, 128, 128));
	CheckColour("hwb(240 0% 0% / 0.5)", Colourb(0, 0, 255, 128));
	CheckInvalid("hwb(0, 0%, 0%)");
}

TEST_CASE("Colors.lab_lch_oklab_oklch")
{
	TestsShell::GetContext();

	CheckColour("lab(100 0 0)", Colourb(255, 255, 255), 1);
	CheckColour("lab(0% 0 0)", Colourb(0, 0, 0), 1);
	// sRGB red in the other spaces (CSS Color 4 examples).
	CheckColour("lab(54.29% 80.82 69.91)", Colourb(255, 0, 0), 2);
	CheckColour("lch(54.29% 106.84 40.85)", Colourb(255, 0, 0), 2);
	CheckColour("oklab(0.628 0.2249 0.1258)", Colourb(255, 0, 0), 2);
	CheckColour("oklch(62.8% 0.2577 29.23)", Colourb(255, 0, 0), 2);
	CheckColour("oklch(62.8% 0.2577 29.23 / 0.5)", Colourb(255, 0, 0, 128), 2);
	CheckColour("oklch(1 0 none)", Colourb(255, 255, 255), 1);
	CheckColour("lab(50% 0 0 / none)", Colourb(119, 119, 119, 0), 1);

	// Chromium 153's sRGB results (canvas fill); out-of-gamut colors are clipped per channel as Chromium does.
	CheckColour("lab(30% 67% -110)", Colourb(88, 0, 253), 1);
	CheckColour("lab(90% -90 80)", Colourb(0, 255, 25), 1);
	CheckColour("lch(30% 85% 180.0)", Colourb(0, 97, 68), 1);
	CheckColour("lch(90% 90 100.0)", Colourb(231, 233, 0), 1);
	CheckColour("lch(50 150 250)", Colourb(0, 152, 255), 1);
	CheckColour("oklab(85% -50% 70%)", Colourb(152, 237, 0), 1);
	CheckColour("oklab(0.2 0.4 -0.4)", Colourb(103, 0, 193), 1);
	CheckColour("oklab(100% none 0.4)", Colourb(255, 225, 0), 1);
	CheckColour("oklab(100% none 0.4 / 0.25)", Colourb(255, 225, 0, 64), 1);
	CheckColour("oklch(0.5 0.2 270)", Colourb(59, 81, 211), 1);
	CheckColour("oklch(1.0 0.1 none)", Colourb(255, 227, 252), 1);
	CheckColour("oklch(0.7 0.4 140)", Colourb(0, 205, 0), 1);
	CheckColour("oklch(0.9 0.3 300)", Colourb(255, 160, 255), 1);

	CheckColour("color(srgb 1 0.5 0)", Colourb(255, 128, 0));
	CheckColour("color(srgb 1.2 -0.1 0.5)", Colourb(255, 0, 128));
	CheckColour("color(srgb-linear 1.5 0.2 -0.3)", Colourb(255, 124, 0), 1);
	CheckColour("color(srgb-linear 1 0 0)", Colourb(255, 0, 0));
	CheckColour("color(xyz 0.9505 1 1.089)", Colourb(255, 255, 255), 1);
	CheckInvalid("color(display-p3 1 0 0)");
	CheckInvalid("lab(50, 0, 0)");
}

TEST_CASE("Colors.color_mix")
{
	TestsShell::GetContext();

	// Vectors from WPT css/css-color/parsing/color-mix-computed.html (sRGB results, 8 bits).
	CheckColour("color-mix(in srgb, red, blue)", Colourb(128, 0, 128));
	CheckColour("color-mix(in srgb, red 25%, blue)", Colourb(64, 0, 191));
	CheckColour("color-mix(in srgb, 25% red, blue 75%)", Colourb(64, 0, 191));
	CheckColour("color-mix(in srgb, red 30%, blue 30%)", Colourb(128, 0, 128, 153));
	CheckColour("color-mix(in srgb, rgb(255 0 0 / 0.5), blue)", Colourb(85, 0, 170, 191));
	CheckColour("color-mix(in srgb, transparent, blue)", Colourb(0, 0, 255, 128));
	CheckColour("color-mix(in srgb-linear, red, blue)", Colourb(188, 0, 188), 1);
	CheckColour("color-mix(in hsl, red, blue)", Colourb(255, 0, 255));
	CheckColour("color-mix(in hsl longer hue, red, blue)", Colourb(0, 255, 0));
	CheckColour("color-mix(in hsl increasing hue, blue, red)", Colourb(255, 0, 255));
	CheckColour("color-mix(in hsl decreasing hue, red, blue)", Colourb(255, 0, 255));
	CheckColour("color-mix(in hsl decreasing hue, blue, red)", Colourb(0, 255, 0));
	CheckColour("color-mix(in hwb, white, black)", Colourb(128, 128, 128));
	CheckColour("color-mix(in oklab, white, black)", Colourb(99, 99, 99), 1);
	CheckColour("color-mix(in oklch, white 100%, black 0%)", Colourb(255, 255, 255), 1);
	CheckColour("color-mix(in lab, red, red)", Colourb(255, 0, 0), 1);
	// Chromium 153's results.
	CheckColour("color-mix(in oklch, red, blue)", Colourb(186, 0, 194), 1);
	CheckColour("color-mix(in lch, red, blue)", Colourb(245, 0, 134), 1);
	CheckColour("color-mix(in lab, red, blue)", Colourb(193, 0, 136), 1);
	CheckColour("color-mix(in oklab, red, blue)", Colourb(140, 83, 162), 1);
	CheckColour("color-mix(in xyz, red, blue)", Colourb(188, 0, 188), 1);
	CheckColour("color-mix(in srgb, color-mix(in srgb, red, blue), white)", Colourb(192, 128, 192), 1);
	CheckColour("COLOR-MIX(IN SRGB, RED, BLUE)", Colourb(128, 0, 128));

	CheckInvalid("color-mix(red, blue)");
	CheckInvalid("color-mix(in srgb, red)");
	CheckInvalid("color-mix(in srgb, red 0%, blue 0%)");
	CheckInvalid("color-mix(in srgb, red 120%, blue)");
	CheckInvalid("color-mix(in srgb longer hue, red, blue)");
	CheckInvalid("color-mix(in display-p3, red, blue)");
}

TEST_CASE("Colors.other_properties")
{
	TestsShell::GetContext();

	PropertyDictionary dictionary;
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "background-color", "rgb(0 0 0 / 50%)"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "border-color", "red rgb(0 0 255) hsl(120 100% 50%) #fff"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "box-shadow", "0 0 4px rgba(0, 0, 0, 0.5)"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "decorator", "linear-gradient(rgb(255 0 0 / 0.5), color-mix(in srgb, red, blue))"));

	Colourb colour;
	REQUIRE(TypeConverter<String, Colourb>::Convert("rgba(0, 0, 0, 0.5)", colour));
	CHECK(colour == Colourb(0, 0, 0, 128));
}
