#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-paint (docs/html-ui/phase-3-fork-patches.md 3.8.1): opacity is not inherited; an element with an opacity
// below 1 is drawn as a group (a layer composited with the 'opacity' filter), or as a leaf with its opacity multiplied
// into its geometry when it draws one thing only; a transparent element draws nothing but can still be hit.

namespace {

const String document_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; font-family: LatoLatin; }
	div { display: block; position: absolute; left: 0; top: 0; width: 100px; height: 100px; }
	#group > div { width: 60px; height: 60px; background-color: #f00; }
	#group > div + div { left: 30px; top: 30px; background-color: #00f; }
</style>
</head>
<body>
	<div id="group"><div id="a"/><div id="b"/></div>
	<div id="leaf" style="left: 200px; background-color: #0f0;"/>
</body>
</rml>
)";

struct Layers {
	size_t push, composite, pop;
};

Layers RenderAndCount(Context* context, TestsRenderInterface* render_interface)
{
	context->Update();
	render_interface->Reset();
	context->Render();
	const TestsRenderInterface::Counters& counters = render_interface->GetCounters();
	return {counters.push_layer, counters.composite_layers, counters.pop_layer};
}

} // namespace

TEST_CASE("GroupOpacity")
{
	Context* context = TestsShell::GetContext();
	TestsRenderInterface* render_interface = TestsShell::GetTestsRenderInterface();
	REQUIRE(render_interface);
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	Element* group = document->GetElementById("group");
	Element* a = document->GetElementById("a");
	Element* leaf = document->GetElementById("leaf");
	TestsShell::RenderLoop();

	// Nothing below 1: no layer.
	Layers layers = RenderAndCount(context, render_interface);
	CHECK(layers.push == 0);
	CHECK(layers.composite == 0);

	SUBCASE("not inherited")
	{
		group->SetProperty("opacity", "0.5");
		context->Update();
		CHECK(group->GetComputedValues().opacity() == 0.5f);
		CHECK(a->GetComputedValues().opacity() == 1.f);
	}

	SUBCASE("group")
	{
		// The overlapping children are drawn opaque into one layer, composited at 50%.
		group->SetProperty("opacity", "0.5");
		layers = RenderAndCount(context, render_interface);
		CHECK(layers.push == 1);
		CHECK(layers.composite == 1);
		CHECK(layers.pop == 1);
		CHECK(group->GetDrawOpacity() == 1.f);
		CHECK(a->GetDrawOpacity() == 1.f);

		// A new opacity compiles a new filter: the geometry stays.
		RenderAndCount(context, render_interface);
		group->SetProperty("opacity", "0.25");
		context->Update();
		render_interface->Reset();
		context->Render();
		CHECK(render_interface->GetCompiledOpacities() == Vector<float>{0.25f});
		CHECK(render_interface->GetCounters().compile_geometry == 0);

		// Nested groups: one layer each.
		a->SetProperty("opacity", "0.5");
		a->SetInnerRML("<div style='position: static; width: 10px; height: 10px; background-color: #000;'/>");
		layers = RenderAndCount(context, render_interface);
		CHECK(layers.push == 2);
		CHECK(layers.composite == 2);
	}

	SUBCASE("leaf")
	{
		// One thing drawn (the background): the opacity goes into its vertices, no layer.
		leaf->SetProperty("opacity", "0.5");
		layers = RenderAndCount(context, render_interface);
		CHECK(layers.push == 0);
		CHECK(leaf->GetDrawOpacity() == 0.5f);

		// A background and a border overlap: a group.
		leaf->SetProperty("border", "2px solid #000");
		layers = RenderAndCount(context, render_interface);
		CHECK(layers.push == 1);
		CHECK(leaf->GetDrawOpacity() == 1.f);

		// Text alone: a leaf again, the text draws with the element's opacity.
		leaf->RemoveProperty("border");
		leaf->SetProperty("background-color", "transparent");
		leaf->SetInnerRML("Text");
		layers = RenderAndCount(context, render_interface);
		CHECK(layers.push == 0);
		CHECK(leaf->GetDrawOpacity() == 0.5f);
		CHECK(leaf->GetChild(0)->GetDrawOpacity() == 1.f);
	}

	SUBCASE("filter and opacity")
	{
		// One layer for both: the opacity is one more filter of the composite.
		group->SetProperty("filter", "brightness(0.5)");
		group->SetProperty("opacity", "0.5");
		layers = RenderAndCount(context, render_interface);
		CHECK(layers.push == 1);
		CHECK(layers.composite == 1);

		leaf->SetProperty("filter", "brightness(0.5)");
		leaf->SetProperty("opacity", "0.5");
		layers = RenderAndCount(context, render_interface);
		CHECK(layers.push == 2);
		CHECK(leaf->GetDrawOpacity() == 1.f);
	}

	SUBCASE("transparent")
	{
		// Nothing of the subtree is drawn, the element is still hit.
		group->SetProperty("opacity", "0");
		context->Update();
		render_interface->Reset();
		context->Render();
		const size_t draws_without = render_interface->GetCounters().render_geometry;
		group->SetProperty("opacity", "1");
		context->Update();
		render_interface->Reset();
		context->Render();
		CHECK(render_interface->GetCounters().render_geometry >= draws_without + 2);

		group->SetProperty("opacity", "0");
		context->Update();
		CHECK(context->GetElementAtPoint(Vector2f(10.f, 10.f)) == a);
	}

	document->Close();
	TestsShell::ShutdownShell();
}
