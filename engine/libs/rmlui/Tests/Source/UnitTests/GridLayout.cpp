#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementScroll.h>
#include <doctest.h>

using namespace Rml;

// CSS Grid layout (phase 3, docs/html-ui/phase-3-fork-patches.md 3.14, FORK.md P3-G2): placement (line numbers, spans,
// names, areas, negative lines, dense), track sizing (fixed, fr, minmax(), fit-content(), auto-fill, auto-fit, intrinsic
// contributions), alignment, the container's intrinsic size, absolutely positioned items in grid areas, and scrolling.
// The expected boxes are Chromium's for the same documents.

namespace {

const String document_header = R"(
<rml>
<head>
	<style>
		body { margin: 0; padding: 0; width: 600px; height: 400px; }
		div { display: block; }
		.grid { display: grid; }
	</style>
</head>
<body>
)";
const String document_footer = "</body></rml>";

struct Rect {
	float x, y, w, h;
};

// The border box of an element relative to the document.
Rect BoxOf(Element* element)
{
	REQUIRE(element);
	const Vector2f position = element->GetAbsoluteOffset(BoxArea::Border);
	const Vector2f size = element->GetBox().GetSize(BoxArea::Border);
	return Rect{position.x, position.y, size.x, size.y};
}

void CheckBox(ElementDocument* document, const char* id, Rect expected)
{
	const String name = String("#") + id;
	INFO(name);
	const Rect box = BoxOf(document->GetElementById(id));
	CHECK(box.x == doctest::Approx(expected.x));
	CHECK(box.y == doctest::Approx(expected.y));
	CHECK(box.w == doctest::Approx(expected.w));
	CHECK(box.h == doctest::Approx(expected.h));
}

ElementDocument* LoadGrid(Context* context, const String& body)
{
	ElementDocument* document = context->LoadDocumentFromMemory(document_header + body + document_footer);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();
	return document;
}

} // namespace

TEST_CASE("GridLayout.fixed_and_flexible_tracks")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadGrid(context, R"(
		<div id="g" class="grid" style="width: 300px; grid-template-columns: 100px 1fr; grid-auto-rows: 40px; gap: 10px;">
			<div id="a"/><div id="b"/><div id="c"/>
		</div>)");

	CheckBox(document, "g", {0, 0, 300, 90});
	CheckBox(document, "a", {0, 0, 100, 40});
	CheckBox(document, "b", {110, 0, 190, 40});
	CheckBox(document, "c", {0, 50, 100, 40});

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("GridLayout.placement")
{
	Context* context = TestsShell::GetContext();

	SUBCASE("lines_spans_negative")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 150px; grid-template-columns: repeat(3, 50px); grid-template-rows: repeat(2, 30px);">
				<div id="a" style="grid-column: 2 / -1;"/>
				<div id="b" style="grid-row: span 2;"/>
				<div id="c" style="grid-column: -2; grid-row: 2;"/>
			</div>)");
		CheckBox(document, "a", {50, 0, 100, 30});
		// 'b' spans the second row and an implicit (empty, auto) third row.
		CheckBox(document, "b", {0, 30, 50, 30});
		CheckBox(document, "c", {100, 30, 50, 30});
		document->Close();
	}

	SUBCASE("named_areas_and_lines")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 150px; grid-template-columns: [left] 50px [middle] 100px [right]; grid-template-rows: 20px 40px;
				grid-template-areas: 'head head' 'side main';">
				<div id="main" style="grid-area: main;"/>
				<div id="head" style="grid-area: head;"/>
				<div id="side" style="grid-column: left / middle; grid-row: side-start;"/>
				<div id="lines" style="grid-column: middle / right; grid-row: head-start / main-end;"/>
			</div>)");
		CheckBox(document, "main", {50, 20, 100, 40});
		CheckBox(document, "head", {0, 0, 150, 20});
		CheckBox(document, "side", {0, 20, 50, 40});
		CheckBox(document, "lines", {50, 0, 100, 60});
		document->Close();
	}

	SUBCASE("dense")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 30px; grid-template-columns: repeat(3, 10px); grid-auto-rows: 10px; grid-auto-flow: row dense;">
				<div id="a" style="grid-column: span 2;"/>
				<div id="b" style="grid-column: span 2;"/>
				<div id="c"/>
			</div>)");
		CheckBox(document, "a", {0, 0, 20, 10});
		CheckBox(document, "b", {0, 10, 20, 10});
		CheckBox(document, "c", {20, 0, 10, 10});
		document->Close();
	}

	SUBCASE("column_flow_and_implicit_tracks")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 100px; grid-template-rows: 10px 10px; grid-auto-columns: 25px; grid-auto-flow: column;">
				<div id="a"/><div id="b"/><div id="c"/>
				<div id="d" style="grid-column: -3;"/>
			</div>)");
		// Without explicit columns, line -3 is two lines before the first: 'd' is on an implicit column before the explicit grid,
		// where auto-placement starts.
		CheckBox(document, "d", {0, 0, 25, 10});
		CheckBox(document, "a", {0, 10, 25, 10});
		CheckBox(document, "b", {25, 0, 25, 10});
		CheckBox(document, "c", {25, 10, 25, 10});
		document->Close();
	}

	SUBCASE("order")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 20px; grid-template-columns: 10px 10px; grid-auto-rows: 10px;">
				<div id="a" style="order: 1;"/><div id="b"/>
			</div>)");
		CheckBox(document, "b", {0, 0, 10, 10});
		CheckBox(document, "a", {10, 0, 10, 10});
		document->Close();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("GridLayout.track_sizing")
{
	Context* context = TestsShell::GetContext();

	SUBCASE("auto_fill")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 130px; grid-template-columns: repeat(auto-fill, 40px); grid-auto-rows: 10px;">
				<div id="a"/><div id="b"/><div id="c"/><div id="d"/>
			</div>)");
		CheckBox(document, "c", {80, 0, 40, 10});
		CheckBox(document, "d", {0, 10, 40, 10});
		document->Close();
	}

	SUBCASE("auto_fit_collapse")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 200px; grid-template-columns: repeat(auto-fit, 50px); grid-auto-rows: 10px; justify-content: center;">
				<div id="a"/><div id="b"/>
			</div>)");
		CheckBox(document, "a", {50, 0, 50, 10});
		CheckBox(document, "b", {100, 0, 50, 10});
		document->Close();
	}

	SUBCASE("minmax_and_fr")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 300px; grid-template-columns: minmax(100px, 1fr) 2fr minmax(0, 50px); grid-auto-rows: 10px;">
				<div id="a"/><div id="b"/><div id="c"/>
			</div>)");
		// 250px for the fr tracks: 1fr = 83.3px is below the 100px minimum, which is then inflexible: 2fr = 150px.
		CheckBox(document, "a", {0, 0, 100, 10});
		CheckBox(document, "b", {100, 0, 150, 10});
		CheckBox(document, "c", {250, 0, 50, 10});
		document->Close();
	}

	SUBCASE("intrinsic_contributions")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 400px; grid-template-columns: auto min-content max-content fit-content(30px) 1fr;">
				<div id="a"><div style="width: 40px; height: 10px;"/></div>
				<div id="b"><div style="width: 20px; height: 15px;"/></div>
				<div id="c"><div style="width: 25px; height: 5px;"/></div>
				<div id="d"><div style="width: 50px; height: 5px;"/></div>
				<div id="e"/>
			</div>)");
		// The auto track stretches into the free space left after the fr track takes it all: none is left.
		CheckBox(document, "a", {0, 0, 40, 15});
		CheckBox(document, "b", {40, 0, 20, 15});
		CheckBox(document, "c", {60, 0, 25, 15});
		CheckBox(document, "d", {85, 0, 50, 15});
		CheckBox(document, "e", {135, 0, 265, 15});
		document->Close();
	}

	SUBCASE("percentages")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 200px; height: 100px; grid-template-columns: 25% calc(50% - 10px); grid-template-rows: 40%; column-gap: 5%;">
				<div id="a"/><div id="b"/>
			</div>)");
		CheckBox(document, "a", {0, 0, 50, 40});
		CheckBox(document, "b", {60, 0, 90, 40});
		document->Close();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("GridLayout.alignment")
{
	Context* context = TestsShell::GetContext();

	SUBCASE("self")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 100px; grid-template-columns: 100px; grid-template-rows: 50px 50px 50px; place-items: center;">
				<div id="a" style="width: 20px; height: 10px;"/>
				<div id="b" style="width: 20px; height: 10px; justify-self: end; align-self: start;"/>
				<div id="c" style="height: 10px; justify-self: stretch; align-self: end;"/>
			</div>)");
		CheckBox(document, "a", {40, 20, 20, 10});
		CheckBox(document, "b", {80, 50, 20, 10});
		CheckBox(document, "c", {0, 140, 100, 10});
		document->Close();
	}

	SUBCASE("content")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 100px; height: 100px; grid-template-columns: 20px 20px; grid-template-rows: 20px 20px;
				justify-content: space-between; align-content: end;">
				<div id="a"/><div id="b"/><div id="c"/>
			</div>)");
		CheckBox(document, "a", {0, 60, 20, 20});
		CheckBox(document, "b", {80, 60, 20, 20});
		CheckBox(document, "c", {0, 80, 20, 20});
		document->Close();
	}

	SUBCASE("auto_margins")
	{
		ElementDocument* document = LoadGrid(context, R"(
			<div class="grid" style="width: 100px; grid-template-columns: 100px; grid-template-rows: 100px;">
				<div id="a" style="width: 20px; height: 20px; margin: auto;"/>
			</div>)");
		CheckBox(document, "a", {40, 40, 20, 20});
		document->Close();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("GridLayout.container_size")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadGrid(context, R"(
		<div id="float" class="grid" style="float: left; grid-template-columns: auto auto; column-gap: 5px; padding: 1px;">
			<div style="width: 30px; height: 10px;"/><div style="width: 50px; height: 20px;"/>
		</div>
		<div id="inline" style="display: inline-grid; grid-template-columns: 1fr 1fr;">
			<div style="width: 30px; height: 10px;"/><div style="width: 10px; height: 10px;"/>
		</div>)");
	// A float shrinks to its max-content width: the columns' sum, with the gap and padding.
	CheckBox(document, "float", {0, 0, 87, 22});
	// The fr columns of an inline grid are as wide as the widest one needs: 30px each.
	CHECK(BoxOf(document->GetElementById("inline")).w == doctest::Approx(60.f));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("GridLayout.absolute_positioning")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadGrid(context, R"(
		<div id="g" class="grid" style="position: relative; width: 100px; height: 60px; padding: 5px; border: 2px #000;
			grid-template-columns: 50px 50px; grid-template-rows: 30px 30px;">
			<div id="area" style="position: absolute; grid-column: 2 / 3; grid-row: 1 / 3; left: 0; top: 0; right: 0; bottom: 0;"/>
			<div id="to-edge" style="position: absolute; grid-column: 2; grid-row: 1 / 3; left: 0; top: 0; right: 0; bottom: 0;"/>
			<div id="static" style="position: absolute; grid-column: 2; grid-row: 2; width: 10px; height: 10px;"/>
			<div id="centered" style="position: absolute; grid-area: 1 / 1 / 2 / 2; width: 10px; height: 10px; place-self: center;"/>
			<div id="padding" style="position: absolute; top: 0; left: 0; width: 10px; height: 10px;"/>
			<div id="item"><div id="hoisted" style="position: absolute; grid-column: 1; grid-row: 2; left: 1px; top: 1px; right: 1px; bottom: 1px;"/></div>
		</div>)");
	CheckBox(document, "area", {57, 7, 50, 60});
	// An 'auto' line of an absolutely positioned box is the padding edge (CSS Grid 2, 10.1).
	CheckBox(document, "to-edge", {57, 7, 55, 60});
	CheckBox(document, "static", {57, 37, 10, 10});
	CheckBox(document, "centered", {27, 17, 10, 10});
	CheckBox(document, "padding", {2, 2, 10, 10});
	// A descendant whose containing block is the grid is placed in its grid area too.
	CheckBox(document, "hoisted", {8, 38, 103, 33});

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("GridLayout.scrolling")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = LoadGrid(context, R"(
		<div id="g" class="grid" style="width: 100px; height: 50px; overflow: hidden; grid-template-rows: repeat(3, 30px); padding-bottom: 5px;">
			<div/><div/><div id="last"/>
		</div>)");
	Element* grid = document->GetElementById("g");
	CHECK(grid->GetScrollHeight() == doctest::Approx(95.f));
	CheckBox(document, "last", {0, 60, 100, 30});

	document->Close();
	TestsShell::ShutdownShell();
}
