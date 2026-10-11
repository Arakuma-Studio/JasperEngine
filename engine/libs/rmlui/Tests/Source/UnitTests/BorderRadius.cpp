#include "../../../Source/Core/GeometryBackgroundBorder.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/RenderBox.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-paint (docs/html-ui/phase-3-fork-patches.md 3.7.1, 3.7.2): a horizontal and a vertical radius per corner,
// percentages of the border box, the scaling of overlapping curves, elliptical geometry and hit testing.

namespace {

bool Parse(PropertyDictionary& dictionary, const String& name, const String& value)
{
	dictionary = PropertyDictionary();
	return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
}

// The eight radii as "tl-x tl-y, tr-x tr-y, br-x br-y, bl-x bl-y".
String Radii(const PropertyDictionary& dictionary)
{
	String out;
	for (const char* corner : {"top-left", "top-right", "bottom-right", "bottom-left"})
	{
		if (!out.empty())
			out += ", ";
		for (const char* axis : {"x", "y"})
		{
			const Property* property =
				dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(CreateString("-rmlui-border-%s-radius-%s", corner, axis)));
			out += (property ? property->ToString() : String("<none>"));
			if (*axis == 'x')
				out += ' ';
		}
	}
	return out;
}

String Radii(const RenderBox& box)
{
	String out;
	for (const Vector2f radius : box.GetBorderRadius())
		out += CreateString("%s%g %g", out.empty() ? "" : ", ", radius.x, radius.y);
	return out;
}

const String document_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; }
	div { display: block; position: absolute; left: 0; top: 0; width: 200px; height: 100px; }
</style>
</head>
<body><div id="a"/></body>
</rml>
)";

} // namespace

TEST_CASE("BorderRadius.shorthands")
{
	TestsShell::GetContext();
	PropertyDictionary dictionary;

	// One to four values, repeated as in a box shorthand; the vertical radii equal the horizontal ones.
	REQUIRE(Parse(dictionary, "border-radius", "10px"));
	CHECK(Radii(dictionary) == "10px 10px, 10px 10px, 10px 10px, 10px 10px");
	REQUIRE(Parse(dictionary, "border-radius", "10px 20%"));
	CHECK(Radii(dictionary) == "10px 10px, 20% 20%, 10px 10px, 20% 20%");
	REQUIRE(Parse(dictionary, "border-radius", "1px 2px 3px"));
	CHECK(Radii(dictionary) == "1px 1px, 2px 2px, 3px 3px, 2px 2px");
	REQUIRE(Parse(dictionary, "border-radius", "1px 2px 3px 4px"));
	CHECK(Radii(dictionary) == "1px 1px, 2px 2px, 3px 3px, 4px 4px");

	// After a slash the vertical radii, expanded on their own.
	REQUIRE(Parse(dictionary, "border-radius", "10px / 20px"));
	CHECK(Radii(dictionary) == "10px 20px, 10px 20px, 10px 20px, 10px 20px");
	REQUIRE(Parse(dictionary, "border-radius", "1px 2px 3px 4px / 5px 6px"));
	CHECK(Radii(dictionary) == "1px 5px, 2px 6px, 3px 5px, 4px 6px");
	REQUIRE(Parse(dictionary, "border-radius", "50%/10%"));
	CHECK(Radii(dictionary) == "50% 10%, 50% 10%, 50% 10%, 50% 10%");

	// A corner: the horizontal radius, then the vertical one.
	REQUIRE(Parse(dictionary, "border-top-right-radius", "10px 20px"));
	CHECK(dictionary.GetNumProperties() == 2);
	CHECK(dictionary.GetProperty(PropertyId::RmlUi_BorderTopRightRadiusX)->ToString() == "10px");
	CHECK(dictionary.GetProperty(PropertyId::RmlUi_BorderTopRightRadiusY)->ToString() == "20px");
	REQUIRE(Parse(dictionary, "border-bottom-left-radius", "25%"));
	CHECK(dictionary.GetProperty(PropertyId::RmlUi_BorderBottomLeftRadiusX)->ToString() == "25%");
	CHECK(dictionary.GetProperty(PropertyId::RmlUi_BorderBottomLeftRadiusY)->ToString() == "25%");

	// Invalid declarations leave nothing behind.
	for (const char* value : {"", "1px 2px 3px 4px 5px", "1px / 2px / 3px", "1px /", "/ 1px", "red", "1px / 1px 2px 3px 4px 5px"})
	{
		INFO(value);
		CHECK_FALSE(Parse(dictionary, "border-radius", value));
		CHECK(dictionary.GetNumProperties() == 0);
	}
	CHECK_FALSE(Parse(dictionary, "border-top-left-radius", "1px 2px 3px"));
	CHECK_FALSE(Parse(dictionary, "border-top-left-radius", "1px / 2px"));

	TestsShell::ShutdownShell();
}

TEST_CASE("BorderRadius.resolution")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	Element* element = document->GetElementById("a");
	auto radii = [&](const char* value, BoxArea area = BoxArea::Border) {
		REQUIRE(element->SetProperty("border-radius", value));
		TestsShell::RenderLoop();
		return Radii(element->GetRenderBox(area));
	};

	// Lengths are kept (also fractions of a pixel), percentages take the border box's width and height.
	CHECK(radii("10px") == "10 10, 10 10, 10 10, 10 10");
	CHECK(radii("2.5px 0.75px") == "2.5 2.5, 0.75 0.75, 2.5 2.5, 0.75 0.75");
	CHECK(radii("10% / 20%") == "20 20, 20 20, 20 20, 20 20");
	CHECK(radii("50%") == "100 50, 100 50, 100 50, 100 50");
	CHECK(radii("10px 20px 30px 40px / 1px 2px 3px 4px") == "10 1, 20 2, 30 3, 40 4");

	const ComputedValues& computed = element->GetComputedValues();
	CHECK(computed.has_border_radius());
	CHECK(computed.border_radius(3, 0).type == Style::LengthPercentage::Length);
	CHECK(computed.border_radius(3, 0).value == 40.f);
	CHECK(computed.border_radius(3, 1).value == 4.f);

	// Overlapping curves (CSS Backgrounds 3, 5.5): all radii shrink by the smallest ratio of a side to its two radii.
	CHECK(radii("100px") == "50 50, 50 50, 50 50, 50 50");
	CHECK(radii("100%") == "100 50, 100 50, 100 50, 100 50");
	CHECK(radii("150px 75px 0 0") == "100 100, 50 50, 0 0, 0 0"); // f = 100 / 150 at the left side
	CHECK(radii("0 0 300px 100px / 0 0 20px 10px") == "0 0, 0 0, 150 10, 50 5");

	// A corner with one radius of zero is square.
	CHECK(radii("20px / 0") == "0 0, 0 0, 0 0, 0 0");
	CHECK_FALSE(element->GetRenderBox(BoxArea::Border).HasBorderRadius());
	CHECK(radii("0") == "0 0, 0 0, 0 0, 0 0");
	CHECK_FALSE(element->GetComputedValues().has_border_radius());

	// calc() with a percentage resolves against the same lengths.
	CHECK(radii("calc(10% + 5px)") == "25 15, 25 15, 25 15, 25 15");
	CHECK(radii("calc(2px * 3)") == "6 6, 6 6, 6 6, 6 6");

	// The border belongs to the box whose width and height percentages take; the radii stay those of the border box
	// for every fill area (the inner curves follow from the border widths).
	REQUIRE(element->SetProperty("border", "10px solid black"));
	CHECK(radii("50%") == "110 60, 110 60, 110 60, 110 60");
	CHECK(radii("50%", BoxArea::Padding) == "110 60, 110 60, 110 60, 110 60");
	const RenderBox padding_box = element->GetRenderBox(BoxArea::Padding);
	const BorderMetrics metrics = GeometryBackgroundBorder::ComputeBorderMetrics(padding_box.GetBorderOffset(), padding_box.GetBorderWidths(),
		padding_box.GetFillSize(), padding_box.GetBorderRadius());
	CHECK(metrics.inner_radii[0].x == doctest::Approx(100.f));
	CHECK(metrics.inner_radii[0].y == doctest::Approx(50.f));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("BorderRadius.geometry")
{
	const ColourbPremultiplied red(255, 0, 0, 255);

	// An ellipse of 80 x 40 px: every vertex of the clip geometry lies on or inside of it, the curve is followed to a
	// tenth of a pixel, and the background has the same shape (its feathered rim half a pixel around it).
	const RenderBox box(Vector2f(80.f, 40.f), Vector2f(0.f), EdgeSizes{}, CornerSizes2{Vector2f(40.f, 20.f), Vector2f(40.f, 20.f), Vector2f(40.f, 20.f), Vector2f(40.f, 20.f)});
	auto ellipse = [](Vector2f p, float grow) {
		const Vector2f q = (p - Vector2f(40.f, 20.f)) / Vector2f(40.f + grow, 20.f + grow);
		return q.x * q.x + q.y * q.y;
	};

	Mesh clip;
	MeshUtilities::GenerateBackground(clip, box, red);
	REQUIRE(clip.vertices.size() > 32);
	int on_curve = 0;
	for (const Vertex& vertex : clip.vertices)
	{
		CHECK(vertex.colour == red);
		CHECK(ellipse(vertex.position, 0.01f) <= 1.f);
		on_curve += (ellipse(vertex.position, -0.01f) >= 1.f);
	}
	CHECK(on_curve > 32);

	// The chords stay within a tenth of a pixel of the curve: the midpoint of two neighbouring curve vertices.
	Vector<Vector2f> curve;
	for (const Vertex& vertex : clip.vertices)
	{
		if (ellipse(vertex.position, -0.01f) >= 1.f && vertex.position.x <= 40.f && vertex.position.y <= 20.f)
			curve.push_back(vertex.position);
	}
	std::sort(curve.begin(), curve.end(), [](Vector2f a, Vector2f b) { return a.x < b.x; });
	REQUIRE(curve.size() > 8);
	for (size_t i = 1; i < curve.size(); i++)
		CHECK(ellipse(0.5f * (curve[i - 1] + curve[i]), 0.f) > ellipse(Vector2f(40.f, 0.15f), 0.f));

	for (bool feathering : {false, true})
	{
		SetGeometryFeathering(feathering);
		Mesh background;
		const ColourbPremultiplied none(0, 0);
		const ColourbPremultiplied no_border[4] = {none, none, none, none};
		MeshUtilities::GenerateBackgroundBorder(background, box, red, no_border);
		bool rim = false;
		for (const Vertex& vertex : background.vertices)
		{
			CHECK(ellipse(vertex.position, 1.2f) <= 1.f);
			if (vertex.colour == red)
				CHECK(ellipse(vertex.position, 0.01f) <= 1.f);
			else
				rim = true;
		}
		CHECK(rim);
	}
	SetGeometryFeathering(true);

	// A small circle stays round: enough points for a tenth of a pixel.
	CHECK(GeometryBackgroundBorder::GetNumPoints(Vector2f(12.f)) >= 8);
	CHECK(GeometryBackgroundBorder::GetNumPoints(Vector2f(100.f, 2.f)) >= GeometryBackgroundBorder::GetNumPoints(Vector2f(50.f)));
}

TEST_CASE("BorderRadius.hit_test")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	Element* element = document->GetElementById("a");
	TestsShell::RenderLoop();

	// The square corner belongs to the element, the rounded one does not (the body behind it is hit).
	CHECK(context->GetElementAtPoint(Vector2f(3.f, 3.f)) == element);
	REQUIRE(element->SetProperty("border-radius", "50%"));
	TestsShell::RenderLoop();
	CHECK(context->GetElementAtPoint(Vector2f(3.f, 3.f)) != element);
	CHECK(context->GetElementAtPoint(Vector2f(197.f, 97.f)) != element);
	CHECK(context->GetElementAtPoint(Vector2f(28.f, 14.f)) != element); // ((100-28)/100)^2 + ((50-14)/50)^2 = 1.037
	CHECK(context->GetElementAtPoint(Vector2f(30.f, 15.f)) == element); // ((100-30)/100)^2 + ((50-15)/50)^2 = 0.98
	CHECK(context->GetElementAtPoint(Vector2f(100.f, 1.f)) == element);
	CHECK(context->GetElementAtPoint(Vector2f(100.f, 50.f)) == element);

	// One rounded corner only.
	REQUIRE(element->SetProperty("border-radius", "0 0 40px 0"));
	TestsShell::RenderLoop();
	CHECK(context->GetElementAtPoint(Vector2f(3.f, 3.f)) == element);
	CHECK(context->GetElementAtPoint(Vector2f(197.f, 97.f)) != element);
	CHECK(context->GetElementAtPoint(Vector2f(3.f, 97.f)) == element);

	document->Close();
	TestsShell::ShutdownShell();
}
