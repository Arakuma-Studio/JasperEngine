#include "../../../Source/Core/ComputeProperty.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/FontMetrics.h>
#include <RmlUi/Core/Math.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/TextShapingContext.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 values (docs/html-ui/phase-3-fork-patches.md 3.3): units, font keywords and lists, the font shorthand,
// line-height: normal.

namespace {

const String values_rml = R"(
<rml>
<head>
	<style>
		body { font-family: LatoLatin; font-size: 20px; }
		div { display: block; }
		%s
	</style>
</head>
<body><div id="a"><div id="b"/></div></body>
</rml>
)";

ElementDocument* LoadValues(Context* context, const char* style)
{
	ElementDocument* document = context->LoadDocumentFromMemory(CreateString(values_rml.c_str(), style));
	REQUIRE(document);
	document->Show();
	context->Update();
	return document;
}

bool ParsesTo(const char* property, const char* value, Unit unit, float number)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, property, value))
		return false;
	const PropertyId id = StyleSheetSpecification::GetPropertyId(property);
	const Property* parsed = dictionary.GetProperty(id);
	return parsed && parsed->unit == unit && Math::Absolute(parsed->Get<float>() - number) < 1e-4f;
}

float ZeroAdvance(Element* element)
{
	static const String no_language;
	return float(GetFontEngineInterface()->GetStringWidth(element->GetFontFaceHandle(), "0", TextShapingContext{no_language}));
}

} // namespace

TEST_CASE("Values.units.parse")
{
	TestsShell::GetContext();

	CHECK(ParsesTo("width", "10vmin", Unit::VMIN, 10.f));
	CHECK(ParsesTo("width", "10vmax", Unit::VMAX, 10.f));
	CHECK(ParsesTo("width", "2ch", Unit::CH, 2.f));
	CHECK(ParsesTo("width", "1.5ex", Unit::EX, 1.5f));
	CHECK(ParsesTo("width", "5svw", Unit::VW, 5.f));
	CHECK(ParsesTo("width", "5lvh", Unit::VH, 5.f));
	CHECK(ParsesTo("width", "5dvh", Unit::VH, 5.f));
	CHECK(ParsesTo("width", "5DVW", Unit::VW, 5.f));
	CHECK(ParsesTo("width", "5svmin", Unit::VMIN, 5.f));
	CHECK(ParsesTo("width", "5dvmax", Unit::VMAX, 5.f));
	CHECK(ParsesTo("perspective", "3ch", Unit::CH, 3.f));

	// Angles only where angles are expected, and 'Q' is not a unit here.
	CHECK_FALSE(ParsesTo("width", "1turn", Unit::TURN, 1.f));
	CHECK_FALSE(ParsesTo("width", "4Q", Unit::PX, 4.f));

	PropertyDictionary dictionary;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "transform", "rotate(0.25turn)"));
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "transform", "rotate(100grad)"));
}

TEST_CASE("Values.units.angles")
{
	CHECK(ComputeAngle(NumericValue(0.25f, Unit::TURN)) == doctest::Approx(Math::RMLUI_PI * 0.5f));
	CHECK(ComputeAngle(NumericValue(100.f, Unit::GRAD)) == doctest::Approx(Math::RMLUI_PI * 0.5f));
	CHECK(ComputeAngle(NumericValue(-2.f, Unit::TURN)) == doctest::Approx(-4.f * Math::RMLUI_PI));
	CHECK(ComputeAngle(NumericValue(90.f, Unit::DEG)) == doctest::Approx(Math::RMLUI_PI * 0.5f));
}

TEST_CASE("Values.units.length_context")
{
	LengthContext context;
	context.font_size = 10.f;
	context.root_font_size = 16.f;
	context.vp_dimensions = Vector2f(400.f, 300.f);

	CHECK(ComputeLength(NumericValue(10.f, Unit::VMIN), context) == doctest::Approx(30.f));
	CHECK(ComputeLength(NumericValue(10.f, Unit::VMAX), context) == doctest::Approx(40.f));
	// Without a font face 'ch' and 'ex' are half the font size.
	CHECK(ComputeLength(NumericValue(2.f, Unit::CH), context) == doctest::Approx(10.f));
	CHECK(ComputeLength(NumericValue(2.f, Unit::EX), context) == doctest::Approx(10.f));
	CHECK(ComputeLength(NumericValue(2.f, Unit::REM), context) == doctest::Approx(32.f));
}

TEST_CASE("Values.units.computed")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const Vector2f viewport(context->GetDimensions());
	const float vmin = Math::Min(viewport.x, viewport.y) * 0.01f;
	const float vmax = Math::Max(viewport.x, viewport.y) * 0.01f;

	ElementDocument* document = LoadValues(context, "#a { width: 10vmin; height: 2vmax; } #b { width: 3ch; height: 2ex; }");
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	REQUIRE(a);
	REQUIRE(b);

	CHECK(a->GetBox().GetSize().x == doctest::Approx(10.f * vmin));
	CHECK(a->GetBox().GetSize().y == doctest::Approx(2.f * vmax));

	const FontMetrics& metrics = GetFontEngineInterface()->GetFontMetrics(b->GetFontFaceHandle());
	REQUIRE(metrics.x_height > 0.f);
	const float zero = ZeroAdvance(b);
	REQUIRE(zero > 0.f);
	CHECK(b->GetBox().GetSize().x == doctest::Approx(3.f * zero));
	CHECK(b->GetBox().GetSize().y == doctest::Approx(2.f * metrics.x_height));

	document->Close();
	context->Update();
}

TEST_CASE("Values.units.follow_the_font_and_the_viewport")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const Vector2i dimensions = context->GetDimensions();

	ElementDocument* document = LoadValues(context, "#a { width: 50vmin; } #b { width: 4ch; height: 4ex; }");
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	REQUIRE(a);
	REQUIRE(b);

	const float width_before = b->GetBox().GetSize().x;
	const float height_before = b->GetBox().GetSize().y;

	// Its own font face: computed again against it.
	b->SetProperty("font-size", "40px");
	context->Update();
	CHECK(b->GetBox().GetSize().x == doctest::Approx(4.f * ZeroAdvance(b)));
	CHECK(b->GetBox().GetSize().x > width_before * 1.5f);
	CHECK(b->GetBox().GetSize().y > height_before * 1.5f);

	// An inherited font face change.
	b->RemoveProperty("font-size");
	a->SetProperty("font-size", "10px");
	context->Update();
	CHECK(b->GetBox().GetSize().x == doctest::Approx(4.f * ZeroAdvance(b)));
	CHECK(b->GetBox().GetSize().x < width_before);

	context->SetDimensions(Vector2i(300, 200));
	context->Update();
	CHECK(a->GetBox().GetSize().x == doctest::Approx(100.f));
	context->SetDimensions(Vector2i(200, 600));
	context->Update();
	CHECK(a->GetBox().GetSize().x == doctest::Approx(100.f));

	context->SetDimensions(dimensions);
	document->Close();
	context->Update();
}

#ifdef RMLUI_MATH_EXPRESSIONS
TEST_CASE("Values.units.calc")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const Vector2f viewport(context->GetDimensions());
	const float vmin = Math::Min(viewport.x, viewport.y) * 0.01f;

	ElementDocument* document =
		LoadValues(context, "#a { width: calc(10vmin + 2ch); height: max(1vmax, 3ex); } #b { width: calc(50% - 1ch); margin-left: calc(1dvw); }");
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	REQUIRE(a);
	REQUIRE(b);

	const FontMetrics& metrics = GetFontEngineInterface()->GetFontMetrics(a->GetFontFaceHandle());
	const float zero = ZeroAdvance(a);
	CHECK(a->GetBox().GetSize().x == doctest::Approx(10.f * vmin + 2.f * zero));
	CHECK(a->GetBox().GetSize().y == doctest::Approx(Math::Max(Math::Max(viewport.x, viewport.y) * 0.01f, 3.f * metrics.x_height)));
	CHECK(b->GetBox().GetSize().x == doctest::Approx(0.5f * a->GetBox().GetSize().x - zero));
	CHECK(b->GetBox().GetEdge(BoxArea::Margin, BoxEdge::Left) == doctest::Approx(viewport.x * 0.01f));

	PropertyDictionary dictionary;
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "transform", "rotate(calc(0.25turn + 100grad))"));

	document->Close();
	context->Update();
}
#endif

namespace {

String ParsedString(const char* property, const char* value)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, property, value))
		return "<invalid>";
	const Property* parsed = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(property));
	return parsed ? parsed->ToString() : "<none>";
}

// The longhands a shorthand declaration sets, as "name: value; " in id order.
String ParsedShorthand(const char* shorthand, const char* value)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, shorthand, value))
		return "<invalid>";
	String result;
	for (const PropertyId id : {PropertyId::FontStyle, PropertyId::FontWeight, PropertyId::FontSize, PropertyId::LineHeight, PropertyId::FontFamily})
	{
		if (const Property* property = dictionary.GetProperty(id))
			result += StyleSheetSpecification::GetPropertyName(id) + ": " + property->ToString() + "; ";
	}
	return result;
}

} // namespace

TEST_CASE("Values.font_family")
{
	TestsShell::GetContext();

	CHECK(ParsedString("font-family", "\"Inter\",  'Segoe  UI' , Sans-Serif") == "inter,segoe ui,sans-serif");
	CHECK(ParsedString("font-family", "Open   Sans, serif") == "open sans,serif");
	CHECK(ParsedString("font-family", "'A, B', c") == "<invalid>");
	CHECK(ParsedString("font-family", "\"Font \\\"X\\\"\"") == "font \"x\"");
	CHECK(ParsedString("font-family", "LatoLatin") == "latolatin");
	CHECK(ParsedString("font-family", "a,,b") == "<invalid>");
	CHECK(ParsedString("font-family", "\"a\" b") == "<invalid>");
	CHECK(ParsedString("font-family", "a(b)") == "<invalid>");
	CHECK(ParsedString("font-family", "a,") == "<invalid>");

	// The font engine resolves the canonical list from left to right: an unknown family falls through.
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadValues(context,
		"#a { font-family: 'Unknown Family', LatoLatin; } #b { --family: \"Other\", 'LatoLatin'; font-family: var(--family); font: italic var(--size) var(--family); --size: 12px; }");
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	CHECK(a->GetComputedValues().font_family() == "unknown family,latolatin");
	// Through var(), also in the font shorthand.
	CHECK(b->GetComputedValues().font_family() == "other,latolatin");
	CHECK(b->GetComputedValues().font_style() == Style::FontStyle::Italic);
	CHECK(b->GetComputedValues().font_size() == 12.f);
	document->Close();
	context->Update();
}

TEST_CASE("Values.font_size_keywords")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadValues(context, "");
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");

	struct Case {
		const char* value;
		float expected;
	};
	const Case cases[] = {
		{"xx-small", 9.f},
		{"x-small", 10.f},
		{"small", 13.f},
		{"medium", 16.f},
		{"large", 18.f},
		{"x-large", 24.f},
		{"xx-large", 32.f},
		{"xxx-large", 48.f},
		{"smaller", 20.f / 1.2f},
		{"larger", 24.f},
		{"math", 20.f},
	};
	for (const Case& test : cases)
	{
		INFO(test.value);
		a->SetProperty("font-size", test.value);
		context->Update();
		CHECK(a->GetComputedValues().font_size() == doctest::Approx(test.expected));
		CHECK(b->GetComputedValues().font_size() == doctest::Approx(test.expected));
	}

	// The face is made at the rounded size, not the truncated one.
	a->SetProperty("font-size", "12.8px");
	context->Update();
	CHECK(GetFontEngineInterface()->GetFontMetrics(a->GetFontFaceHandle()).size == 13);

	document->Close();
	context->Update();
}

TEST_CASE("Values.font_weight_relative")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadValues(context, "");
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");

	struct Case {
		const char* parent;
		const char* child;
		int expected;
	};
	const Case cases[] = {
		{"400", "bolder", 700},
		{"400", "lighter", 100},
		{"300", "bolder", 400},
		{"700", "bolder", 900},
		{"700", "lighter", 400},
		{"900", "bolder", 900},
		{"900", "lighter", 700},
		{"50", "lighter", 50},
		{"600", "lighter", 400},
		{"bold", "bolder", 900},
	};
	for (const Case& test : cases)
	{
		INFO(test.parent, " ", test.child);
		a->SetProperty("font-weight", test.parent);
		b->SetProperty("font-weight", test.child);
		context->Update();
		CHECK(int(b->GetComputedValues().font_weight()) == test.expected);
	}

	// Computed again when the parent's weight changes.
	a->SetProperty("font-weight", "100");
	b->SetProperty("font-weight", "bolder");
	context->Update();
	CHECK(int(b->GetComputedValues().font_weight()) == 400);
	a->SetProperty("font-weight", "800");
	context->Update();
	CHECK(int(b->GetComputedValues().font_weight()) == 900);

	document->Close();
	context->Update();
}

TEST_CASE("Values.font_shorthand")
{
	TestsShell::GetContext();
	const String defaults_style = "font-style: normal; font-weight: normal; ";

	CHECK(ParsedShorthand("font", "italic bold 20px/1.5 Arial, sans-serif") ==
		"font-style: italic; font-weight: bold; font-size: 20px; line-height: 1.5; font-family: arial,sans-serif; ");
	CHECK(ParsedShorthand("font", "16px LatoLatin").find(defaults_style) == 0);
	CHECK(ParsedShorthand("font", "italic normal 20px \"LatoLatin\"") ==
		"font-style: italic; font-weight: normal; font-size: 20px; line-height: " +
			StyleSheetSpecification::GetProperty(PropertyId::LineHeight)->GetDefaultValue()->ToString() + "; font-family: latolatin; ");
	CHECK(ParsedShorthand("font", "700 1.5em / 2 'Open Sans'") ==
		"font-style: normal; font-weight: 700; font-size: 1.5em; line-height: 2; font-family: open sans; ");
	CHECK(ParsedShorthand("font", "normal normal 12px /20px a") ==
		"font-style: normal; font-weight: normal; font-size: 12px; line-height: 20px; font-family: a; ");
	CHECK(ParsedShorthand("font", "oblique 10deg large serif") ==
		"font-style: italic; font-weight: normal; font-size: large; line-height: " +
			StyleSheetSpecification::GetProperty(PropertyId::LineHeight)->GetDefaultValue()->ToString() + "; font-family: serif; ");
	CHECK(ParsedShorthand("font", "bolder larger a") ==
		"font-style: normal; font-weight: bolder; font-size: larger; line-height: " +
			StyleSheetSpecification::GetProperty(PropertyId::LineHeight)->GetDefaultValue()->ToString() + "; font-family: a; ");

	for (const char* invalid : {"bold", "20px", "caption", "menu", "small-caps 12px a", "condensed 12px a", "italic italic 12px a",
			 "normal normal normal normal normal 12px a", "12px/ a", "bold 12px 'a' b", "12px/1.5"})
	{
		INFO(invalid);
		CHECK(ParsedShorthand("font", invalid) == "<invalid>");
	}

	// The omitted longhands are reset.
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadValues(context, "#a { font-weight: bold; font-style: italic; line-height: 3; } #a { font: 10px LatoLatin; }");
	Element* a = document->GetElementById("a");
	CHECK(a->GetComputedValues().font_weight() == Style::FontWeight::Normal);
	CHECK(a->GetComputedValues().font_style() == Style::FontStyle::Normal);
	CHECK(a->GetComputedValues().font_size() == 10.f);
	CHECK(a->GetComputedValues().line_height().inherit_value != doctest::Approx(3.f));
	document->Close();
	context->Update();
}

namespace {

float NormalLineHeight(Element* element)
{
	const FontMetrics& metrics = GetFontEngineInterface()->GetFontMetrics(element->GetFontFaceHandle());
	return Math::Round(metrics.ascent) + Math::Round(metrics.descent) + Math::Round(Math::Max(metrics.line_spacing - metrics.ascent - metrics.descent, 0.f));
}

} // namespace

TEST_CASE("Values.line_height_normal")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	CHECK(ParsedString("line-height", "normal") == "normal");
	CHECK(ParsedString("line-height", "NORMAL") == "normal");
	CHECK(StyleSheetSpecification::GetProperty(PropertyId::LineHeight)->GetDefaultValue()->ToString() == "normal");
	CHECK(DefaultComputedValues().line_height().inherit_type == Style::LineHeight::Normal);

	ElementDocument* document = LoadValues(context, "#b { font-size: 40px; } #b { vertical-align: 50%; display: inline-block; }");
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	REQUIRE(a->GetFontFaceHandle());

	// The initial value: the face's ascent, descent and line gap, each rounded.
	const float normal_a = NormalLineHeight(a);
	const float normal_b = NormalLineHeight(b);
	CHECK(normal_a > 20.f);
	CHECK(normal_b > 1.5f * normal_a);
	CHECK(a->GetComputedValues().line_height().value == normal_a);
	CHECK(a->GetComputedValues().line_height().inherit_type == Style::LineHeight::Normal);
	// Inherited as the keyword: the child's own face.
	CHECK(b->GetComputedValues().line_height().value == normal_b);
	CHECK(b->GetComputedValues().vertical_align().value == doctest::Approx(0.5f * normal_b));

	// A number inherits as a number, and 'normal' again resolves per face.
	a->SetProperty("line-height", "2");
	context->Update();
	CHECK(b->GetComputedValues().line_height().value == doctest::Approx(80.f));
	CHECK(b->GetComputedValues().vertical_align().value == doctest::Approx(40.f));
	a->SetProperty("line-height", "normal");
	context->Update();
	CHECK(b->GetComputedValues().line_height().value == normal_b);
	CHECK(b->GetComputedValues().vertical_align().value == doctest::Approx(0.5f * normal_b));

	// A font change follows.
	b->SetProperty("font-size", "10px");
	context->Update();
	CHECK(b->GetComputedValues().line_height().value == NormalLineHeight(b));
	CHECK(b->GetComputedValues().line_height().value < normal_a);
	a->SetProperty("font-size", "30px");
	context->Update();
	CHECK(a->GetComputedValues().line_height().value == NormalLineHeight(a));
	CHECK(a->GetComputedValues().line_height().value > normal_a);

	// The font shorthand resets it.
	b->SetProperty("line-height", "3");
	b->SetProperty("font", "20px LatoLatin");
	context->Update();
	CHECK(b->GetComputedValues().line_height().inherit_type == Style::LineHeight::Normal);
	CHECK(b->GetComputedValues().line_height().value == normal_a);

	// Without a font face: 1.2 times the font size.
	CHECK(ComputeNormalLineHeight(0, 10.f) == doctest::Approx(12.f));

	document->Close();
	context->Update();
}

TEST_CASE("Values.logical_properties")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadValues(context,
		"#a { width: 300px; height: 300px; position: relative; } "
		"#b { position: absolute; margin-inline: 10px 20px; margin-block: 5px; padding-inline-start: 3px; padding-block-end: 4px; "
		"inset-inline-start: 7px; inset-block: 8px auto; inline-size: 50px; block-size: 40px; max-inline-size: 45px; min-block-size: 60px; "
		"border-inline-width: 2px 4px; border-block-start-width: 1px; border-inline-color: #ff0000; border-block: 3px #0000ff; }");
	Element* b = document->GetElementById("b");
	REQUIRE(b);
	const auto& computed = b->GetComputedValues();

	CHECK(ResolveValue(computed.margin_left(), 0.f) == 10.f);
	CHECK(ResolveValue(computed.margin_right(), 0.f) == 20.f);
	CHECK(ResolveValue(computed.margin_top(), 0.f) == 5.f);
	CHECK(ResolveValue(computed.margin_bottom(), 0.f) == 5.f);
	CHECK(ResolveValue(computed.padding_left(), 0.f) == 3.f);
	CHECK(ResolveValue(computed.padding_bottom(), 0.f) == 4.f);
	CHECK(ResolveValue(computed.left(), 0.f) == 7.f);
	CHECK(ResolveValue(computed.top(), 0.f) == 8.f);
	CHECK(computed.bottom().type == Style::LengthPercentageAuto::Auto);
	CHECK(ResolveValue(computed.width(), 0.f) == 50.f);
	CHECK(ResolveValue(computed.height(), 0.f) == 40.f);
	CHECK(ResolveValue(computed.max_width(), 0.f) == 45.f);
	CHECK(ResolveValue(computed.min_height(), 0.f) == 60.f);
	CHECK(computed.border_right_width() == 4);
	// border-block (3px) comes after border-block-start-width (1px).
	CHECK(computed.border_top_width() == 3);
	CHECK(computed.border_bottom_width() == 3);
	CHECK(computed.border_left_width() == 2);
	CHECK(computed.border_left_color() == Colourb(255, 0, 0));
	CHECK(computed.border_right_color() == Colourb(255, 0, 0));
	CHECK(computed.border_top_color() == Colourb(0, 0, 255));

	// Through var(), and the shorthands' validation.
	b->SetProperty("--m", "1px 2px");
	CHECK(b->SetProperty("margin-inline", "var(--m)"));
	context->Update();
	CHECK(ResolveValue(b->GetComputedValues().margin_left(), 0.f) == 1.f);
	CHECK(ResolveValue(b->GetComputedValues().margin_right(), 0.f) == 2.f);

	PropertyDictionary dictionary;
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "margin-inline", "1px 2px 3px"));
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "inline-size", "red"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "padding-block", "10%"));
	// The border styles are the paint package's longhands: the logical shorthands parse once those are registered.
	const bool has_border_styles = StyleSheetSpecification::GetProperty(PropertyId::BorderLeftStyle) != nullptr;
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "border-inline-style", "solid dashed") == has_border_styles);
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "border-block-end-style", "dotted") == has_border_styles);
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "border-block-end-style", "solid solid"));

	document->Close();
	context->Update();
}

TEST_CASE("Values.cursor")
{
	Context* context = TestsShell::GetContext();

	CHECK(ParsedString("cursor", "pointer") == "pointer");
	CHECK(ParsedString("cursor", "Not-Allowed") == "not-allowed");
	CHECK(ParsedString("cursor", "url(hand.png), pointer") == "pointer");
	CHECK(ParsedString("cursor", "url(\"a b.cur\") 4 12, url(x.svg), grab") == "grab");
	CHECK(ParsedString("cursor", "image-set(\"a.png\" 1x), move") == "move");
	// Application cursor names as written.
	CHECK(ParsedString("cursor", "arrow") == "arrow");
	CHECK(ParsedString("cursor", "My_Cursor-2") == "My_Cursor-2");
	CHECK(ParsedString("cursor", "x<y") == "<invalid>");
	CHECK(ParsedString("cursor", "a b") == "<invalid>");
	CHECK(ParsedString("cursor", "2d") == "<invalid>");
	CHECK(ParsedString("cursor", "url(a.png)") == "<invalid>");
	CHECK(ParsedString("cursor", "pointer, url(a.png)") == "<invalid>");
	// Fallback keywords for the system interface.
	CHECK(ParsedString("cursor", "url(x.png), Help, pointer") == "help, pointer");
	CHECK(ParsedString("cursor", "zoom-in, my-cursor") == "zoom-in, my-cursor");
	CHECK(ParsedString("cursor", "url(a.png) 4, pointer") == "<invalid>");

	// The keyword reaches the system interface.
	ElementDocument* document = LoadValues(context, "#a { cursor: url(x.png) 1 2, zoom-in; width: 50px; height: 50px; }");
	Element* a = document->GetElementById("a");
	CHECK(a->GetComputedValues().cursor() == "zoom-in");
	document->Close();
	context->Update();
}
