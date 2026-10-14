#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/Types.h>
#include <doctest.h>
#include <float.h>

using namespace Rml;

static String GenerateRowsRml(int num_rows, String row_rml)
{
	String rml;
	rml.reserve(num_rows * row_rml.size());
	for (int i = 0; i < num_rows; i++)
		rml += row_rml;
	return rml;
}

static const String document_basic_rml = R"(
<rml>
<head>
<title>Demo</title>
<link type="text/rcss" href="/assets/rml.rcss" />
<link type="text/rcss" href="/../Tests/Data/style.rcss" />
<style>
	body {
		width: 800px;
		height: 800px;
	}
	#wrapper {
		height: 300.3px;
		overflow-y: scroll;
		background-color: #333;
	}
	#wrapper > div {
		height: 100.25px;
		background-color: #c33;
		margin: 5.333px 0;
		position: relative;
	}
</style>
</head>
<body>
	<div id="wrapper">
	</div>
</body>
</rml>
)";

TEST_CASE("ElementBackgroundBorder.render_stats")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(document_basic_rml);
	REQUIRE(document);
	document->Show();

	constexpr int num_rows = 10;
	const String row_rml = "<div/>";
	const String inner_rml = GenerateRowsRml(num_rows, row_rml);

	Element* wrapper = document->GetElementById("wrapper");
	REQUIRE(wrapper);
	wrapper->SetInnerRML(inner_rml);

	context->Update();
	context->Render();

	TestsShell::RenderLoop();

	TestsRenderInterface* render_interface = TestsShell::GetTestsRenderInterface();
	if (!render_interface)
		return;

	MESSAGE(TestsShell::GetRenderStats());
	render_interface->Reset();

	for (int i = 1; i < 50; i++)
	{
		wrapper->SetScrollTop(1.3333f * float(i));
		context->Update();
		context->Render();
	}

	wrapper->SetScrollTop(FLT_MAX);
	context->Update();
	context->Render();

	// Ensure that we have no unnecessary compile geometry commands. The geometry for the background-border should not
	// change in size as long as scrolling occurs in integer increments.
	CHECK(render_interface->GetCounters().compile_geometry == 0);

	document->Close();
	TestsShell::ShutdownShell();
}

static const String document_relative_offset_rml = R"(
<rml>
<head>
<title>Demo</title>
<link type="text/rcss" href="/assets/rml.rcss" />
<link type="text/rcss" href="/../Tests/Data/style.rcss" />
<style>
	body {
		width: 800px;
		height: 800px;
	}
	#wrapper {
		height: 600px;
		background-color: #333;
	}
	#wrapper > div {
		height: 10.333px;
		background-color: #c33;
		position: relative;
	}
</style>
</head>
<body>
	<div id="wrapper">
	</div>
</body>
</rml>
)";

TEST_CASE("ElementBackgroundBorder.background_edges_line_up_with_relative_offset")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(document_relative_offset_rml);
	REQUIRE(document);
	document->Show();

	constexpr int num_children = 10;
	const String row_rml = "<div/>";
	const String inner_rml = GenerateRowsRml(num_children, row_rml);

	Element* wrapper = document->GetElementById("wrapper");
	REQUIRE(wrapper);
	wrapper->SetInnerRML(inner_rml);

	context->Update();
	context->Render();

	TestsShell::RenderLoop();

	TestsRenderInterface* render_interface = TestsShell::GetTestsRenderInterface();
	if (!render_interface)
		return;

	MESSAGE(TestsShell::GetRenderStats());
	render_interface->Reset();

	for (int i = 1; i < 100; i++)
	{
		for (int child_index = 0; child_index < num_children; child_index++)
		{
			Element* child = wrapper->GetChild(child_index);
			child->SetProperty(PropertyId::Top, Property(1.3333f * float(i), Unit::PX));
		}

		context->Update();
		context->Render();

		for (int child_index = 0; child_index < num_children - 1; child_index++)
		{
			Element* current_child = wrapper->GetChild(child_index);
			Element* next_child = wrapper->GetChild(child_index + 1);
			Vector2f current_bottom_right = current_child->GetAbsoluteOffset(BoxArea::Border).Round() + current_child->GetRenderBox().GetFillSize();
			Vector2f next_top_left = next_child->GetAbsoluteOffset(BoxArea::Border).Round();
			CHECK(current_bottom_right.y == next_top_left.y);
		}
	}

	// When changing the position using fractional increments we expect the size of the backgrounds to change, resulting
	// in new geometry. This is done to ensure that the top and bottom of each background lines up with the one for the
	// next element, thereby avoiding any gaps.
	CHECK(render_interface->GetCounters().compile_geometry > 0);
	MESSAGE("Compile geometry after movement: ", render_interface->GetCounters().compile_geometry);

	document->Close();
	TestsShell::ShutdownShell();
}

// Fork (P4-G7): the box-shadow cache shares a texture (the shadow with the element's background and border drawn in it) only
// between elements whose background and border are anti-aliased alike. Its key had no pixel size: after a change of the
// geometry pixel size (a document drawn at another scale: the editor's Game view on play) an element kept the texture made at
// the old size, or got a new one, depending on whether another element still held it, so the same frame rendered with other
// curve edges from one session to the next.
TEST_CASE("ElementBackgroundBorder.box_shadow_texture_follows_the_pixel_size")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	TestsRenderInterface* render = TestsShell::GetTestsRenderInterface();
	REQUIRE(render);

	const String rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 300px; }
		div { display: block; width: 100px; height: 60px; margin: 20px; border-radius: 12px; background-color: #3366cc; box-shadow: 0 4px 16px #000a; }
	</style>
</head>
<body><div id="a"/><div id="b"/></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	// The layers pushed in a frame: a shadow texture is rendered in layers when it is made.
	const auto layers_in_frame = [&]() {
		render->ResetCounters();
		context->Update();
		context->Render();
		render->ResetCounters();
		return render->GetCountersFromPreviousReset().push_layer;
	};

	CHECK(layers_in_frame() > 0); // made (once for both elements: one shadow)
	CHECK(layers_in_frame() == 0);

	// Drawn at twice the size: the edges are feathered over half a document pixel, in a new texture.
	SetGeometryPixelSize(0.5f);
	CHECK(layers_in_frame() > 0);
	CHECK(layers_in_frame() == 0);

	// And back.
	SetGeometryPixelSize(1.f);
	CHECK(layers_in_frame() > 0);
	CHECK(layers_in_frame() == 0);

	document->Close();
	TestsShell::ShutdownShell();
}

// Fork (P4-G7): the rounded clips of background image layers are feathered over the element's device pixel and made again
// when it changes, as the background colour is (they kept the pixel size they were first made at).
TEST_CASE("ElementBackgroundBorder.image_layer_clips_follow_the_pixel_size")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	TestsRenderInterface* render = TestsShell::GetTestsRenderInterface();
	REQUIRE(render);

	const String rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 300px; }
		div { display: block; width: 100px; height: 60px; border-radius: 12px; background-color: #336699; background-image: linear-gradient(#ff0000, #0000ff); }
	</style>
</head>
<body><div id="a"/></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();

	const auto geometry_in_frame = [&]() {
		render->ResetCounters();
		context->Update();
		context->Render();
		render->ResetCounters();
		return render->GetCountersFromPreviousReset().compile_geometry;
	};
	geometry_in_frame();
	CHECK(geometry_in_frame() == 0);

	// Everything of the background made again (a property change): the colour, the layer and its rounded clip.
	Element* div = document->GetElementById("a");
	div->SetProperty("background-color", "#336698");
	const size_t all = geometry_in_frame();
	CHECK(all >= 3);

	// Another pixel size makes as much again.
	SetGeometryPixelSize(0.5f);
	CHECK(geometry_in_frame() == all);
	SetGeometryPixelSize(1.f);
	CHECK(geometry_in_frame() == all);

	document->Close();
	TestsShell::ShutdownShell();
}
