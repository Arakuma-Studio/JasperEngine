#include "../../../Source/Core/GeometryBorderStyles.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>
#include <float.h>

using namespace Rml;

// Phase 3, p3-paint (docs/html-ui/phase-3-fork-patches.md 3.7.1, 3.7.3): border styles, the CSS border shorthands in
// any order, line-width keywords, the computed width of none/hidden sides, and the styled border geometry.

namespace {

String Parsed(const PropertyDictionary& dictionary, const String& name)
{
	const Property* property = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(name));
	return property ? property->ToString() : String("<none>");
}

bool Parse(PropertyDictionary& dictionary, const String& name, const String& value)
{
	dictionary = PropertyDictionary();
	return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
}

const String document_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; }
	div { display: block; width: 50px; height: 20px; }
</style>
</head>
<body><div id="a"/></body>
</rml>
)";

int CountColour(const Mesh& mesh, ColourbPremultiplied colour)
{
	int count = 0;
	for (const Vertex& vertex : mesh.vertices)
		count += (vertex.colour == colour);
	return count;
}

} // namespace

TEST_CASE("BorderStyle.shorthands")
{
	TestsShell::GetContext();
	PropertyDictionary dictionary;

	// Width, style and color in any order, on all four sides.
	for (const char* value : {"1px solid red", "solid 1px red", "red solid 1px", "solid red 1px", "1px red solid", "red 1px solid"})
	{
		INFO(value);
		REQUIRE(Parse(dictionary, "border", value));
		for (const char* side : {"top", "right", "bottom", "left"})
		{
			CHECK(Parsed(dictionary, CreateString("border-%s-width", side)) == "1px");
			CHECK(Parsed(dictionary, CreateString("border-%s-style", side)) == "solid");
			CHECK(Parsed(dictionary, CreateString("border-%s-color", side)) == "#ff0000");
		}
		CHECK(Parsed(dictionary, "border-image") == "none");
	}

	// Omitted parts take their initial values: medium, none, the initial colour.
	REQUIRE(Parse(dictionary, "border", "dashed"));
	CHECK(Parsed(dictionary, "border-left-width") == "3px"); // medium (a length, it interpolates)
	CHECK(Parsed(dictionary, "border-left-style") == "dashed");
	CHECK(Parsed(dictionary, "border-left-color") == "currentcolor");
	REQUIRE(Parse(dictionary, "border", "2px"));
	CHECK(Parsed(dictionary, "border-top-style") == "none");

	// Each part at most once, nothing else.
	CHECK_FALSE(Parse(dictionary, "border", "1px solid red blue"));
	CHECK_FALSE(Parse(dictionary, "border", "1px 2px"));
	CHECK_FALSE(Parse(dictionary, "border", "solid dashed"));
	CHECK_FALSE(Parse(dictionary, "border", "1px solid red 3px"));
	CHECK_FALSE(Parse(dictionary, "border", ""));
	CHECK_FALSE(Parse(dictionary, "border", "wavy"));

	// The sides: AnyOrder, omitted parts reset.
	REQUIRE(Parse(dictionary, "border-top", "dotted thick"));
	CHECK(Parsed(dictionary, "border-top-width") == "5px");
	CHECK(Parsed(dictionary, "border-top-style") == "dotted");
	CHECK(Parsed(dictionary, "border-top-color") == "currentcolor");
	CHECK(Parsed(dictionary, "border-right-style") == "<none>");
	REQUIRE(Parse(dictionary, "border-left", "#00ff00 groove"));
	CHECK(Parsed(dictionary, "border-left-width") == "3px"); // medium (a length, it interpolates)
	CHECK_FALSE(Parse(dictionary, "border-bottom", "1px 2px"));

	// border-style is a box shorthand of every style keyword.
	REQUIRE(Parse(dictionary, "border-style", "none hidden dotted"));
	CHECK(Parsed(dictionary, "border-top-style") == "none");
	CHECK(Parsed(dictionary, "border-right-style") == "hidden");
	CHECK(Parsed(dictionary, "border-bottom-style") == "dotted");
	CHECK(Parsed(dictionary, "border-left-style") == "hidden");
	for (const char* style : {"none", "hidden", "dotted", "dashed", "solid", "double", "groove", "ridge", "inset", "outset"})
	{
		INFO(style);
		CHECK(Parse(dictionary, "border-top-style", style));
	}
	CHECK_FALSE(Parse(dictionary, "border-top-style", "auto"));

	// border-image takes only 'none' (compat emulates the others).
	CHECK(Parse(dictionary, "border-image", "none"));
	CHECK_FALSE(Parse(dictionary, "border-image", "url(a.png) 30"));

	TestsShell::ShutdownShell();
}

TEST_CASE("BorderStyle.computed_width")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	Element* a = document->GetElementById("a");
	REQUIRE(a);

	auto width_of = [&](const String& border) {
		a->SetProperty("border", border);
		context->Update();
		return a->GetComputedValues().border_top_width();
	};

	// A border without a style is not painted and takes no space.
	CHECK(width_of("5px") == 0.f);
	CHECK(a->GetBox().GetEdge(BoxArea::Border, BoxEdge::Top) == 0.f);
	CHECK(width_of("5px none") == 0.f);
	CHECK(width_of("5px hidden") == 0.f);
	CHECK(width_of("5px solid") == 5.f);
	CHECK(a->GetBox().GetEdge(BoxArea::Border, BoxEdge::Left) == 5.f);
	CHECK(width_of("5px dashed") == 5.f);

	// The keywords, the initial 'medium', and Chromium's snapping of widths to whole pixels.
	CHECK(width_of("thin solid") == 1.f);
	CHECK(width_of("medium solid") == 3.f);
	CHECK(width_of("thick solid") == 5.f);
	CHECK(width_of("solid") == 3.f);
	CHECK(width_of("0.5px solid") == 1.f);
	CHECK(width_of("0.1px solid") == 1.f);
	CHECK(width_of("1.5px solid") == 1.f);
	CHECK(width_of("2.5px solid") == 2.f);
	CHECK(width_of("2.99px solid") == 2.f);
	CHECK(width_of("0px solid") == 0.f);
	CHECK(width_of("0.1875em solid") == 3.f);

	// Only the side whose style is none loses its width.
	a->SetProperty("border", "4px solid");
	a->SetProperty("border-right-style", "none");
	context->Update();
	CHECK(a->GetComputedValues().border_top_width() == 4.f);
	CHECK(a->GetComputedValues().border_right_width() == 0.f);
	CHECK(a->GetComputedValues().border_right_style() == Style::BorderStyle::None);
	CHECK(a->GetComputedValues().border_bottom_style() == Style::BorderStyle::Solid);
	CHECK(a->GetBox().GetEdge(BoxArea::Border, BoxEdge::Right) == 0.f);

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("BorderStyle.geometry")
{
	// The exact geometry, without the feathering of the edges (checked below).
	SetGeometryFeathering(false);
	const ColourbPremultiplied red(255, 0, 0, 255), blue(0, 0, 255, 255), none(0, 0);
	auto render_box = [](float width, Style::BorderStyle style) {
		RenderBox box(Vector2f(60.f - 2.f * width, 40.f - 2.f * width), Vector2f(0.f), EdgeSizes{width, width, width, width}, CornerSizes{});
		box.SetBorderStyles({style, style, style, style});
		return box;
	};
	const ColourbPremultiplied colors[4] = {red, red, red, red};

	SUBCASE("solid")
	{
		Mesh mesh;
		MeshUtilities::GenerateBackgroundBorder(mesh, render_box(8.f, Style::BorderStyle::Solid), none, colors);
		CHECK(mesh.vertices.size() == 8);
	}

	SUBCASE("dashed")
	{
		// 60px sides, 8px wide: dashes of 16px and gaps of 8px stretched: three dashes a side (Chromium), two a side of 40px.
		Mesh mesh;
		MeshUtilities::GenerateBackgroundBorder(mesh, render_box(8.f, Style::BorderStyle::Dashed), none, colors);
		int num_strips = 0;
		for (size_t i = 0; i + 1 < mesh.indices.size(); i += 6)
			num_strips++;
		CHECK(mesh.indices.size() > 0);
		// Every vertex is inside the border area.
		for (const Vertex& vertex : mesh.vertices)
		{
			const bool inside = vertex.position.x >= -0.01f && vertex.position.y >= -0.01f && vertex.position.x <= 60.01f && vertex.position.y <= 40.01f;
			const bool in_border = vertex.position.x <= 8.01f || vertex.position.y <= 8.01f || vertex.position.x >= 51.99f || vertex.position.y >= 31.99f;
			CHECK(inside);
			CHECK(in_border);
		}
		// The first dash of the top side covers the corner and 16px of the edge, then a gap of 6px.
		bool dash_ends_at_16 = false, dash_starts_at_22 = false;
		for (const Vertex& vertex : mesh.vertices)
		{
			dash_ends_at_16 |= (vertex.position.y == 0.f && Math::Absolute(vertex.position.x - 16.f) < 0.01f);
			dash_starts_at_22 |= (vertex.position.y == 0.f && Math::Absolute(vertex.position.x - 22.f) < 0.01f);
		}
		CHECK(dash_ends_at_16);
		CHECK(dash_starts_at_22);
		CHECK(num_strips > 8);
	}

	SUBCASE("dotted")
	{
		Mesh mesh;
		MeshUtilities::GenerateBackgroundBorder(mesh, render_box(8.f, Style::BorderStyle::Dotted), none, colors);
		// Round dots, one centred on each corner (4, 4).
		bool corner_dot = false;
		for (const Vertex& vertex : mesh.vertices)
			corner_dot |= (vertex.position == Vector2f(4.f, 4.f));
		CHECK(corner_dot);
		CHECK(mesh.vertices.size() > 40);
	}

	SUBCASE("double")
	{
		Mesh mesh;
		MeshUtilities::GenerateBackgroundBorder(mesh, render_box(6.f, Style::BorderStyle::Double), none, colors);
		// Two bands of 2px: nothing between 2px and 4px from the outer edge.
		for (const Vertex& vertex : mesh.vertices)
		{
			const float distance = Math::Min(Math::Min(vertex.position.x, vertex.position.y), Math::Min(60.f - vertex.position.x, 40.f - vertex.position.y));
			CHECK((distance <= 2.01f || distance >= 3.99f));
		}
		// Thinner than 3px, a double border is solid.
		Mesh thin;
		MeshUtilities::GenerateBackgroundBorder(thin, render_box(2.f, Style::BorderStyle::Double), none, colors);
		Mesh solid;
		MeshUtilities::GenerateBackgroundBorder(solid, render_box(2.f, Style::BorderStyle::Solid), none, colors);
		CHECK(thin.vertices.size() >= solid.vertices.size());
	}

	SUBCASE("3d")
	{
		// Chromium's dark colour: rgb(200, 100, 50) scaled by (v - 0.33) / v.
		const ColourbPremultiplied base(200, 100, 50, 255);
		const ColourbPremultiplied dark = GeometryBorderStyles::DarkColor(base);
		CHECK(dark == ColourbPremultiplied(116, 58, 29, 255));
		const ColourbPremultiplied light = GeometryBorderStyles::LightColor(base);
		CHECK(light == ColourbPremultiplied(255, 127, 63, 255));
		// Colours darker than #202020 are lightened for both parts (Light, then Light again), black to fixed greys; colours
		// lighter than #ebebeb are their own light part.
		CHECK(GeometryBorderStyles::DarkColor(ColourbPremultiplied(0, 0, 0, 255)) == ColourbPremultiplied(84, 84, 84, 255));
		CHECK(GeometryBorderStyles::LightColor(ColourbPremultiplied(0, 0, 0, 255)) == ColourbPremultiplied(168, 168, 168, 255));
		CHECK(GeometryBorderStyles::DarkColor(ColourbPremultiplied(32, 32, 32, 255)) == ColourbPremultiplied(116, 116, 116, 255));
		CHECK(GeometryBorderStyles::LightColor(ColourbPremultiplied(32, 32, 32, 255)) == ColourbPremultiplied(200, 200, 200, 255));
		CHECK(GeometryBorderStyles::DarkColor(ColourbPremultiplied(34, 34, 34, 255)) == ColourbPremultiplied(0, 0, 0, 255));
		CHECK(GeometryBorderStyles::LightColor(ColourbPremultiplied(34, 34, 34, 255)) == ColourbPremultiplied(118, 118, 118, 255));
		CHECK(GeometryBorderStyles::DarkColor(ColourbPremultiplied(240, 240, 240, 255)) == ColourbPremultiplied(156, 156, 156, 255));
		CHECK(GeometryBorderStyles::LightColor(ColourbPremultiplied(240, 240, 240, 255)) == ColourbPremultiplied(240, 240, 240, 255));
		CHECK(GeometryBorderStyles::LightColor(ColourbPremultiplied(235, 235, 235, 255)) == ColourbPremultiplied(255, 255, 255, 255));
		CHECK(GeometryBorderStyles::DarkColor(ColourbPremultiplied(255, 255, 255, 255)) == ColourbPremultiplied(171, 171, 171, 255));
		const ColourbPremultiplied base_colors[4] = {base, base, base, base};

		Mesh inset;
		MeshUtilities::GenerateBackgroundBorder(inset, render_box(6.f, Style::BorderStyle::Inset), none, base_colors);
		CHECK(CountColour(inset, dark) > 0);
		CHECK(CountColour(inset, light) > 0);

		// Groove: top and left dark outside, light inside; the bottom and right the other way round.
		Mesh groove;
		MeshUtilities::GenerateBackgroundBorder(groove, render_box(6.f, Style::BorderStyle::Groove), none, base_colors);
		bool top_outer_dark = false, bottom_outer_light = false;
		for (const Vertex& vertex : groove.vertices)
		{
			if (vertex.position.y == 0.f)
				top_outer_dark |= (vertex.colour == dark);
			if (vertex.position.y == 40.f)
				bottom_outer_light |= (vertex.colour == light);
		}
		CHECK(top_outer_dark);
		CHECK(bottom_outer_light);
	}

	SUBCASE("corner split")
	{
		// Different colours meeting at a rounded corner are split, not blended.
		RenderBox box(Vector2f(40.f), Vector2f(0.f), EdgeSizes{4.f, 4.f, 4.f, 4.f}, CornerSizes{10.f, 10.f, 10.f, 10.f});
		const ColourbPremultiplied mixed[4] = {red, blue, red, blue};
		REQUIRE(GeometryBorderStyles::IsNeeded(
			GeometryBackgroundBorder::ComputeBorderMetrics(box.GetBorderOffset(), box.GetBorderWidths(), box.GetFillSize(), box.GetBorderRadius()),
			box.GetBorderWidths(), mixed, box.GetBorderStyles()));
		Mesh mesh;
		MeshUtilities::GenerateBackgroundBorder(mesh, box, none, mixed);
		// No vertex mixes the colours (the curves' feathered rims only fade them).
		for (const Vertex& vertex : mesh.vertices)
			CHECK((vertex.colour.red == 0 || vertex.colour.blue == 0));
		CHECK(CountColour(mesh, red) > 0);
		CHECK(CountColour(mesh, blue) > 0);
	}

	SetGeometryFeathering(true);
}

TEST_CASE("BorderStyle.feathering")
{
	// Feathered edges (anti-aliasing): the opaque part moves half a pixel inwards, a ring fades to transparent half a pixel
	// outside; pixel-aligned edges stay where they are for pixel centres.
	REQUIRE(GetGeometryFeathering());
	const ColourbPremultiplied red(255, 0, 0, 255), none(0, 0);
	const ColourbPremultiplied colors[4] = {red, red, red, red};

	RenderBox box(Vector2f(20.f), Vector2f(0.f), EdgeSizes{4.f, 4.f, 4.f, 4.f}, CornerSizes{});
	Mesh mesh;
	MeshUtilities::GenerateBackgroundBorder(mesh, box, none, colors);
	float min_opaque_x = FLT_MAX, max_opaque_x = -FLT_MAX, min_x = FLT_MAX, max_x = -FLT_MAX;
	for (const Vertex& vertex : mesh.vertices)
	{
		min_x = Math::Min(min_x, vertex.position.x);
		max_x = Math::Max(max_x, vertex.position.x);
		if (vertex.colour == red)
		{
			min_opaque_x = Math::Min(min_opaque_x, vertex.position.x);
			max_opaque_x = Math::Max(max_opaque_x, vertex.position.x);
		}
		else
		{
			CHECK(vertex.colour == none);
		}
	}
	CHECK(min_x == doctest::Approx(-1.f));
	CHECK(max_x == doctest::Approx(29.f));
	CHECK(min_opaque_x == doctest::Approx(0.5f));
	CHECK(max_opaque_x == doctest::Approx(27.5f));

	// A background alone is feathered at its edge; under a border its edge stays sharp (no seam).
	Mesh background;
	MeshUtilities::GenerateBackgroundBorder(background, RenderBox(Vector2f(20.f), Vector2f(0.f), EdgeSizes{}, CornerSizes{10.f, 10.f, 10.f, 10.f}),
		red, colors);
	bool transparent_rim = false;
	for (const Vertex& vertex : background.vertices)
		transparent_rim |= (vertex.colour == none);
	CHECK(transparent_rim);

	// The clip geometry is never feathered.
	Mesh clip;
	MeshUtilities::GenerateBackground(clip, RenderBox(Vector2f(20.f), Vector2f(0.f), EdgeSizes{}, CornerSizes{10.f, 10.f, 10.f, 10.f}), red);
	for (const Vertex& vertex : clip.vertices)
		CHECK(vertex.colour == red);
}
