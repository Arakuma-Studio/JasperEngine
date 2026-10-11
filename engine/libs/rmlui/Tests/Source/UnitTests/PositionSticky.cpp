#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.11.2): 'position: sticky' is laid out in flow and shifted so
// that it stays in the scrollport of its scroll container, inset by its 'top', 'right', 'bottom', 'left', without leaving
// its containing block (CSS Positioned Layout 3 §3.4).

namespace {

const String position_sticky_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 600px; margin: 0; padding: 0; }
		div { display: block; }
		#scroller { overflow: auto; width: 200px; height: 100px; margin-top: 30px; }
		#section { height: 300px; }
		#head { position: sticky; top: 5px; height: 20px; }
		#foot { position: sticky; bottom: 0px; height: 10px; }
		#wide { overflow: auto; width: 100px; height: 60px; }
		#row { display: flex; width: 500px; height: 40px; }
		#left { position: sticky; left: 10px; width: 30px; margin-left: 50px; flex: none; }
	</style>
</head>
<body>
	<div id="scroller">
		<div style="height: 50px"/>
		<div id="section"><div id="head"><div id="head-child" style="height: 5px"/></div></div>
		<div style="height: 400px"/>
		<div id="foot"/>
		<div style="height: 100px"/>
	</div>
	<div id="wide"><div id="row"><div id="left"/></div></div>
</body>
</rml>
)";

} // namespace

TEST_CASE("PositionSticky")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(position_sticky_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* scroller = document->GetElementById("scroller");
	Element* head = document->GetElementById("head");
	Element* head_child = document->GetElementById("head-child");
	Element* foot = document->GetElementById("foot");
	const float scroller_top = scroller->GetAbsoluteOffset(BoxArea::Border).y;
	REQUIRE(scroller_top == 30.f);
	REQUIRE(scroller->GetScrollHeight() == doctest::Approx(860.f));

	// The top of an element's border box in the scrollport.
	auto top = [&](Element* element) { return element->GetAbsoluteOffset(BoxArea::Border).y - scroller_top; };
	auto scroll_to = [&](float scroll_top) {
		scroller->SetScrollTop(scroll_top);
		context->Update();
		REQUIRE(scroller->GetScrollTop() == scroll_top);
	};

	SUBCASE("In flow while its position is in the sticky view rectangle")
	{
		CHECK(top(head) == doctest::Approx(50.f));
		scroll_to(40.f);
		CHECK(top(head) == doctest::Approx(10.f));
	}

	SUBCASE("Stuck at its top inset once scrolled past it")
	{
		scroll_to(100.f);
		CHECK(top(head) == doctest::Approx(5.f));
		scroll_to(250.f);
		CHECK(top(head) == doctest::Approx(5.f));
		// Its content moves with it.
		CHECK(top(head_child) == doctest::Approx(5.f));
	}

	SUBCASE("It does not leave its containing block")
	{
		// The 300px section ends 10px below the top of the scrollport: the 20px header ends with it.
		scroll_to(340.f);
		CHECK(top(head) == doctest::Approx(-10.f));
		// And back.
		scroll_to(0.f);
		CHECK(top(head) == doctest::Approx(50.f));
	}

	SUBCASE("Stuck at its bottom inset before it is reached")
	{
		// The footer is 750px down the content of the scroll container, which is its containing block.
		CHECK(top(foot) == doctest::Approx(90.f));
		scroll_to(300.f);
		CHECK(top(foot) == doctest::Approx(90.f));
		scroll_to(700.f);
		CHECK(top(foot) == doctest::Approx(50.f));
	}

	SUBCASE("Laid out as in flow")
	{
		// The shift takes no space and does not change the scrollable area.
		scroll_to(100.f);
		CHECK(head->GetBox().GetSize(BoxArea::Border).y == doctest::Approx(20.f));
		CHECK(scroller->GetScrollHeight() == doctest::Approx(860.f));
		CHECK(document->GetElementById("section")->GetAbsoluteOffset(BoxArea::Border).y - scroller_top == doctest::Approx(-50.f));
	}

	SUBCASE("Horizontally, in a flex container")
	{
		Element* wide = document->GetElementById("wide");
		Element* left = document->GetElementById("left");
		const float wide_left = wide->GetAbsoluteOffset(BoxArea::Border).x;
		auto x = [&]() { return left->GetAbsoluteOffset(BoxArea::Border).x - wide_left; };
		CHECK(x() == doctest::Approx(50.f));
		wide->SetScrollLeft(100.f);
		context->Update();
		REQUIRE(wide->GetScrollLeft() == 100.f);
		CHECK(x() == doctest::Approx(10.f));
		// The row is 500px wide: scrolled to its end (400px), the box is still inside it.
		wide->SetScrollLeft(400.f);
		context->Update();
		CHECK(x() == doctest::Approx(10.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
