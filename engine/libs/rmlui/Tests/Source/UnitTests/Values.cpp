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
