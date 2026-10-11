#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-paint (docs/html-ui/phase-3-fork-patches.md 3.7.1, 3.7.4): the outline properties and their shorthand,
// computed values, and that an outline is drawn without changing the layout.

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
	body { display: block; width: 400px; height: 300px; color: #00f; }
	div { display: block; width: 50px; height: 20px; background-color: #ccc; }
</style>
</head>
<body><div id="a"/><div id="b"/></body>
</rml>
)";

} // namespace

TEST_CASE("Outline.properties")
{
	TestsShell::GetContext();
	PropertyDictionary dictionary;

	for (const char* value : {"2px dashed red", "dashed red 2px", "red 2px dashed"})
	{
		INFO(value);
		REQUIRE(Parse(dictionary, "outline", value));
		CHECK(Parsed(dictionary, "outline-width") == "2px");
		CHECK(Parsed(dictionary, "outline-style") == "dashed");
		CHECK(Parsed(dictionary, "outline-color") == "#ff0000");
		CHECK(dictionary.GetNumProperties() == 3); // 'outline-offset' is not reset by the shorthand
	}

	// Omitted parts take their initial values.
	REQUIRE(Parse(dictionary, "outline", "auto"));
	CHECK(Parsed(dictionary, "outline-width") == "3px"); // medium: a length, it interpolates
	CHECK(Parsed(dictionary, "outline-style") == "auto");
	CHECK(Parsed(dictionary, "outline-color") == "currentcolor");
	REQUIRE(Parse(dictionary, "outline", "thick solid"));
	CHECK(Parsed(dictionary, "outline-width") == "5px");
	CHECK(Parsed(dictionary, "outline-color") == "currentcolor");
	REQUIRE(Parse(dictionary, "outline", "none"));
	CHECK(Parsed(dictionary, "outline-style") == "none");
	REQUIRE(Parse(dictionary, "outline", "invert dotted"));
	CHECK(Parsed(dictionary, "outline-color") == "invert");

	for (const char* style : {"none", "auto", "dotted", "dashed", "solid", "double", "groove", "ridge", "inset", "outset"})
	{
		REQUIRE(Parse(dictionary, "outline-style", style));
		CHECK(Parsed(dictionary, "outline-style") == style);
	}
	CHECK_FALSE(Parse(dictionary, "outline-style", "hidden"));
	CHECK_FALSE(Parse(dictionary, "outline", "2px 3px solid"));
	CHECK_FALSE(Parse(dictionary, "outline", "solid dashed"));
	REQUIRE(Parse(dictionary, "outline-offset", "-3px"));
	CHECK(Parsed(dictionary, "outline-offset") == "-3px");
	CHECK_FALSE(Parse(dictionary, "outline-offset", "10%"));

	TestsShell::ShutdownShell();
}

TEST_CASE("Outline.computed_and_drawn")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	TestsShell::RenderLoop();

	using Style::BorderStyle;
	const ComputedValues& computed = a->GetComputedValues();
	CHECK(computed.outline_style() == BorderStyle::None);
	CHECK(computed.outline_width() == 0.f); // medium, but no style
	CHECK(computed.outline_offset() == 0.f);
	CHECK(computed.outline_color() == Colourb(0, 0, 255)); // currentcolor

	const Vector2f b_offset = b->GetAbsoluteOffset(BoxArea::Border);
	const Vector2f a_size = a->GetBox().GetSize(BoxArea::Border);

	TestsRenderInterface* render_interface = TestsShell::GetTestsRenderInterface();
	if (!render_interface)
	{
		document->Close();
		TestsShell::ShutdownShell();
		return;
	}
	auto count_geometry = [&] {
		render_interface->ResetCounters();
		context->Render();
		return render_interface->GetCounters().render_geometry;
	};
	TestsShell::RenderLoop();
	const size_t draws_without = count_geometry();

	REQUIRE(a->SetProperty("outline", "4px solid"));
	REQUIRE(a->SetProperty("outline-offset", "2.9px"));
	TestsShell::RenderLoop();
	CHECK(computed.outline_style() == BorderStyle::Solid);
	CHECK(computed.outline_width() == 4.f);
	CHECK(computed.outline_offset() == doctest::Approx(2.9f));
	CHECK(computed.outline_color() == Colourb(0, 0, 255));

	// One more draw, and the layout is as it was.
	CHECK(count_geometry() == draws_without + 1);
	CHECK(b->GetAbsoluteOffset(BoxArea::Border) == b_offset);
	CHECK(a->GetBox().GetSize(BoxArea::Border) == a_size);

	// A colour of its own, widths snapped like border widths, the keywords.
	REQUIRE(a->SetProperty("outline", "0.4px dotted #f00"));
	TestsShell::RenderLoop();
	CHECK(computed.outline_width() == 1.f);
	CHECK(computed.outline_color() == Colourb(255, 0, 0));
	REQUIRE(a->SetProperty("outline", "thin auto"));
	TestsShell::RenderLoop();
	CHECK(computed.outline_style() == BorderStyle::Auto);
	CHECK(computed.outline_width() == 1.f);
	CHECK(computed.outline_color() == Colourb(0, 0, 255));
	CHECK(count_geometry() == draws_without + 1);

	// currentcolor follows the colour.
	REQUIRE(a->SetProperty("color", "#0f0"));
	TestsShell::RenderLoop();
	CHECK(computed.outline_color() == Colourb(0, 255, 0));

	// None draws nothing.
	REQUIRE(a->SetProperty("outline-style", "none"));
	TestsShell::RenderLoop();
	CHECK(computed.outline_width() == 0.f);
	CHECK(count_geometry() == draws_without);

	document->Close();
	TestsShell::ShutdownShell();
}
