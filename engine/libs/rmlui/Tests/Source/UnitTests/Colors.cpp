#include "../Common/TestsShell.h"
#include <RmlUi/Core/Colour.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleSheetTypes.h>
#include <RmlUi/Core/TypeConverter.h>
#include "../../../Source/Core/TextShadow.h"
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

TEST_CASE("Colors.currentcolor")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	// Parsed to a keyword wherever a property takes a colour, serialized as written.
	PropertyDictionary dictionary;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "background-color", "CurrentColor"));
	CHECK(dictionary.GetProperty(PropertyId::BackgroundColor)->unit == Unit::KEYWORD);
	CHECK(dictionary.GetProperty(PropertyId::BackgroundColor)->ToString() == "currentcolor");
	CHECK(StyleSheetSpecification::GetProperty(PropertyId::BorderTopColor)->GetDefaultValue()->ToString() == "currentcolor");
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "box-shadow", "0 0 2px currentcolor, 1px 1px red"));
	CHECK(dictionary.GetProperty(PropertyId::BoxShadow)->ToString() == "currentcolor 0px 0px 2px, #ff0000 1px 1px");
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "decorator", "linear-gradient(currentColor, transparent)"));
	Colourb colour;
	CHECK_FALSE(TypeConverter<String, Colourb>::Convert("currentcolor", colour));

	const String rml = R"(
<rml>
<head>
	<style>
		body { color: #0000ff; }
		#a { color: #ff0000; border: 1px #00ff00; border-color: currentcolor; background-color: currentColor; image-color: currentcolor; }
		#b { color: currentcolor; background-color: currentcolor; }
		#c { border-width: 1px; border-left-color: currentcolor; border-right-color: #00ff00; }
	</style>
</head>
<body><div id="a"><div id="b"/></div><div id="c"/></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	Element* c = document->GetElementById("c");
	CHECK(a->GetComputedValues().border_top_color() == Colourb(255, 0, 0));
	CHECK(a->GetComputedValues().border_left_color() == Colourb(255, 0, 0));
	CHECK(a->GetComputedValues().background_color() == Colourb(255, 0, 0));
	CHECK(a->GetComputedValues().image_color() == Colourb(255, 0, 0));
	// In 'color' itself it is the parent's color.
	CHECK(b->GetComputedValues().color() == Colourb(255, 0, 0));
	CHECK(b->GetComputedValues().background_color() == Colourb(255, 0, 0));
	CHECK(c->GetComputedValues().border_left_color() == Colourb(0, 0, 255));
	CHECK(c->GetComputedValues().border_right_color() == Colourb(0, 255, 0));
	CHECK(c->GetComputedValues().border_top_color() == Colourb(0, 0, 255));

	// The properties follow a change of the color, also an inherited one.
	a->SetProperty("color", "#808080");
	context->Update();
	CHECK(a->GetComputedValues().border_top_color() == Colourb(128, 128, 128));
	CHECK(a->GetComputedValues().background_color() == Colourb(128, 128, 128));
	CHECK(b->GetComputedValues().color() == Colourb(128, 128, 128));
	CHECK(b->GetComputedValues().background_color() == Colourb(128, 128, 128));
	document->SetProperty("color", "#ffff00");
	context->Update();
	CHECK(c->GetComputedValues().border_left_color() == Colourb(255, 255, 0));
	CHECK(a->GetComputedValues().border_top_color() == Colourb(128, 128, 128));

	// The root's 'currentcolor' in 'color' is the initial color.
	document->SetProperty("color", "currentcolor");
	context->Update();
	CHECK(document->GetComputedValues().color() == Colourb(0, 0, 0));

	document->Close();
	context->Update();
}

TEST_CASE("Colors.color_mix_currentcolor")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	// A color-mix() with a currentcolor operand parses, as written, and resolves with the element's colour (CSS Color 5).
	PropertyDictionary dictionary;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "background-color", "color-mix(in srgb, currentColor 25%, blue)"));
	CHECK(dictionary.GetProperty(PropertyId::BackgroundColor)->unit == Unit::STYLE_VALUE);
	CHECK(dictionary.GetProperty(PropertyId::BackgroundColor)->ToString() == "color-mix(in srgb, currentcolor 25%, blue)");
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "box-shadow", "0 0 2px color-mix(in srgb, currentcolor, red)"));
	CHECK(dictionary.GetProperty(PropertyId::BoxShadow)->ToString() == "color-mix(in srgb, currentcolor, red) 0px 0px 2px");
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "background-color", "color-mix(in srgb, currentcolor)"));
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "background-color", "color-mix(in nowhere, currentcolor, red)"));
	Colourb colour;
	CHECK_FALSE(TypeConverter<String, Colourb>::Convert("color-mix(in srgb, currentcolor, red)", colour));

	const String rml = R"(
<rml>
<head>
	<style>
		body { color: #0000ff; }
		#a { color: #ff0000; border-width: 1px; border-color: color-mix(in srgb, currentcolor 25%, #0000ff);
			background-color: color-mix(in srgb, color-mix(in srgb, currentcolor, #ffffff), #000000);
			box-shadow: 0 0 0 1px color-mix(in srgb, currentcolor, #00ff00);
			decorator: linear-gradient(color-mix(in srgb, currentcolor, transparent), #000000);
			text-shadow: 1px 1px color-mix(in oklab, currentcolor 50%, #ffffff); }
		#b { color: color-mix(in srgb, currentcolor, #000000); }
	</style>
</head>
<body><div id="a"><div id="b"/></div></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	CHECK(a->GetComputedValues().border_top_color() == Colourb(64, 0, 191));
	CHECK(a->GetComputedValues().background_color() == Colourb(128, 64, 64));
	// In 'color' itself currentcolor is the parent's color.
	CHECK(b->GetComputedValues().color() == Colourb(128, 0, 0));
	const Vector<ResolvedTextShadow> shadows = ResolveTextShadows(*a);
	REQUIRE(shadows.size() == 1);
	const Colourb text_shadow = shadows[0].colour;

	// The properties follow a change of the color.
	a->SetProperty("color", "#00ff00");
	context->Update();
	CHECK(a->GetComputedValues().border_top_color() == Colourb(0, 64, 191));
	CHECK(a->GetComputedValues().background_color() == Colourb(64, 128, 64));
	CHECK(b->GetComputedValues().color() == Colourb(0, 128, 0));
	const Vector<ResolvedTextShadow> shadows_after = ResolveTextShadows(*a);
	REQUIRE(shadows_after.size() == 1);
	CHECK(shadows_after[0].colour != text_shadow);
	CHECK(shadows_after[0].colour.green > shadows_after[0].colour.red);

	// The shadow and the gradient stop keep it for their element.
	const Property* shadow = a->GetProperty("box-shadow");
	REQUIRE(shadow);
	const BoxShadowList& list = shadow->value.GetReference<BoxShadowList>();
	REQUIRE(list.size() == 1);
	CHECK(list[0].current_color);
	CHECK(list[0].color_mix == "color-mix(in srgb, currentcolor, #00ff00)");
	const Property* decorator = a->GetProperty("decorator");
	REQUIRE(decorator);
	CHECK(decorator->ToString().find("color-mix(in srgb, currentcolor, transparent)") != String::npos);

	document->Close();
	context->Update();
}
