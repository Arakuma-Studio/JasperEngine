#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementScroll.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <doctest.h>

using namespace Rml;

// The content of a scroll container is clipped to its scrollport, the padding box without the scrollbars (CSS Overflow 3
// §3), so a positioned descendant does not paint over them; a scrollbar's bar keeps its margins along the track (the
// thumb of Chromium's scrollbars is 3px off the track's ends), and an auto height of a horizontal bar fills the scrollbar
// inside the bar's vertical margins.

namespace {

const String scroll_container_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 400px; margin: 0; padding: 0; }
		div { display: block; }
		scrollbarvertical { width: 15px; }
		scrollbarhorizontal { height: 15px; }
		scrollbarvertical slidertrack { height: 100%; }
		scrollbarhorizontal slidertrack { width: 100%; height: 15px; }
		scrollbarvertical sliderarrowdec, scrollbarvertical sliderarrowinc,
		scrollbarhorizontal sliderarrowdec, scrollbarhorizontal sliderarrowinc { width: 15px; height: 15px; }
		scrollbarvertical sliderbar { margin: 3px; min-height: 17px; }
		scrollbarhorizontal sliderbar { margin: 3px; min-width: 17px; }
		scrollbarcorner { width: 15px; height: 15px; }
		#scroller { position: absolute; left: 10px; top: 20px; width: 200px; height: 150px; padding: 5px; overflow: auto; }
		#content { width: 400px; height: 300px; }
		#sticky { position: sticky; top: 0; left: 0; width: 500px; height: 20px; z-index: 1; }
		#plain { overflow: hidden; position: absolute; left: 250px; top: 20px; width: 100px; height: 100px; }
		#plain-child { position: absolute; width: 300px; height: 300px; }
	</style>
</head>
<body>
	<div id="scroller"><div id="sticky"/><div id="content"/></div>
	<div id="plain"><div id="plain-child"/></div>
</body>
</rml>
)";

// A part of a scrollbar (the widget's parts are not DOM children: QuerySelector does not see them).
Element* Part(Element* scrollbar, const String& tag)
{
	for (int i = 0; i < scrollbar->GetNumChildren(true); i++)
		if (scrollbar->GetChild(i)->GetTagName() == tag)
			return scrollbar->GetChild(i);
	return nullptr;
}

} // namespace

TEST_CASE("ScrollContainer")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(scroll_container_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();

	Element* scroller = document->GetElementById("scroller");
	ElementScroll* scroll = scroller->GetElementScroll();
	REQUIRE(scroll->GetScrollbarSize(ElementScroll::VERTICAL) == 15.f);
	REQUIRE(scroll->GetScrollbarSize(ElementScroll::HORIZONTAL) == 15.f);
	Element* vertical = scroll->GetScrollbar(ElementScroll::VERTICAL);
	Element* horizontal = scroll->GetScrollbar(ElementScroll::HORIZONTAL);
	REQUIRE(vertical);
	REQUIRE(horizontal);

	SUBCASE("content is clipped to the scrollport")
	{
		// padding box: (10, 20) 210 x 160; without the scrollbars: 195 x 145
		Rectanglei region;
		REQUIRE(ElementUtilities::GetClippingRegion(document->GetElementById("sticky"), region));
		CHECK(region.Position() == Vector2i(10, 20));
		CHECK(region.Size() == Vector2i(195, 145));
		REQUIRE(ElementUtilities::GetClippingRegion(document->GetElementById("content"), region));
		CHECK(region.Size() == Vector2i(195, 145));
	}

	SUBCASE("the scrollbars are not clipped by their scroll container")
	{
		Element* bar = Part(vertical, "sliderbar");
		REQUIRE(bar);
		Rectanglei region;
		if (ElementUtilities::GetClippingRegion(bar, region))
		{
			CHECK(region.Right() >= 220);
			CHECK(region.Bottom() >= 180);
		}
		if (ElementUtilities::GetClippingRegion(horizontal, region))
			CHECK(region.Bottom() >= 180);
	}

	SUBCASE("a container without scrollbars clips to its padding box")
	{
		// (also an absolutely positioned box that overflows it: 6.3 clipped only when the in-flow content overflowed)
		Rectanglei region;
		REQUIRE(ElementUtilities::GetClippingRegion(document->GetElementById("plain-child"), region));
		CHECK(region.Size() == Vector2i(100, 100));
	}

	SUBCASE("the bars keep their margins along the track")
	{
		Element* track = Part(vertical, "slidertrack");
		Element* bar = Part(vertical, "sliderbar");
		REQUIRE(track);
		REQUIRE(bar);
		const float track_top = track->GetAbsoluteOffset(BoxArea::Border).y;
		const float track_length = track->GetBox().GetSize().y;
		// 3px from the track's start, (track - 6) x 145 / 330 long (the sticky box and the content plus the padding)
		CHECK(bar->GetAbsoluteOffset(BoxArea::Border).y == doctest::Approx(track_top + 3.f));
		CHECK(bar->GetBox().GetSize(BoxArea::Border).y == doctest::Approx(Math::Round((track_length - 6.f) * 145.f / 330.f)));
		CHECK(bar->GetAbsoluteOffset(BoxArea::Border).x == doctest::Approx(vertical->GetAbsoluteOffset(BoxArea::Border).x + 3.f));
		CHECK(bar->GetBox().GetSize(BoxArea::Border).x == doctest::Approx(9.f));

		// scrolled to the end: 3px from the track's end
		scroller->SetScrollTop(scroller->GetScrollHeight());
		context->Update();
		const float bar_bottom = bar->GetAbsoluteOffset(BoxArea::Border).y + bar->GetBox().GetSize(BoxArea::Border).y;
		CHECK(bar_bottom == doctest::Approx(track_top + track_length - 3.f).epsilon(0.01));

		// the horizontal bar: 9px high (an auto height inside its 3px margins), 3px from the top and the track's start
		Element* htrack = Part(horizontal, "slidertrack");
		Element* hbar = Part(horizontal, "sliderbar");
		REQUIRE(htrack);
		REQUIRE(hbar);
		CHECK(hbar->GetBox().GetSize(BoxArea::Border).y == doctest::Approx(9.f));
		CHECK(hbar->GetAbsoluteOffset(BoxArea::Border).y == doctest::Approx(horizontal->GetAbsoluteOffset(BoxArea::Border).y + 3.f));
		CHECK(hbar->GetAbsoluteOffset(BoxArea::Border).x == doctest::Approx(htrack->GetAbsoluteOffset(BoxArea::Border).x + 3.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}

namespace {

const String absolute_overflow_rml = R"(
<rml>
<head>
	<style>
		body { width: 600px; height: 400px; margin: 0; padding: 0; }
		div { display: block; }
		scrollbarvertical { width: 15px; }
		scrollbarhorizontal { height: 15px; }
		scrollbarvertical slidertrack { height: 100%; }
		scrollbarhorizontal slidertrack { width: 100%; height: 15px; }
		scrollbarvertical sliderbar { margin: 3px; min-height: 17px; }
		scrollbarhorizontal sliderbar { margin: 3px; min-width: 17px; }
		scrollbarcorner { width: 15px; height: 15px; }
		.scroller { position: absolute; width: 100px; height: 100px; overflow: auto; }
		#a { left: 0; top: 0; }
		#a-child { position: absolute; left: 10px; top: 10px; width: 150px; height: 50px; }
		#b { left: 120px; top: 0; }
		#b-inner { position: relative; height: 20px; margin-top: 5px; }
		#b-child { position: absolute; left: 10px; top: 30px; width: 50px; height: 120px; }
		#c { left: 240px; top: 0; overflow: hidden; }
		#c-child { position: absolute; left: 30px; top: 40px; width: 150px; height: 150px; }
		#d { left: 360px; top: 0; position: absolute; }
		#d-scroller { position: static; width: 100px; height: 100px; overflow: auto; }
		#d-child { position: absolute; left: 0; top: 0; width: 200px; height: 200px; }
		#e { left: 0; top: 150px; }
		#e-child { position: absolute; left: 0; top: 0; width: 50px; height: 50px; }
		#e-grandchild { position: absolute; left: 40px; top: 60px; width: 100px; height: 100px; }
	</style>
</head>
<body>
	<div id="a" class="scroller"><div id="a-child"/></div>
	<div id="b" class="scroller"><div id="b-inner"><div id="b-child"/></div></div>
	<div id="c" class="scroller"><div id="c-child"/></div>
	<div id="d"><div id="d-scroller"><div id="d-child"/></div></div>
	<div id="e" class="scroller"><div id="e-child"><div id="e-grandchild"/></div></div>
</body>
</rml>
)";

} // namespace

// Absolutely positioned boxes are scrollable overflow of the nearest scroll container of their containing block (CSS Overflow 3
// §2.2): 6.3 laid them out after measuring the overflow, so they neither showed scrollbars nor could be scrolled to.
TEST_CASE("ScrollContainer.AbsoluteOverflow")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(absolute_overflow_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto scrollbars = [&](const char* id) {
		ElementScroll* scroll = document->GetElementById(id)->GetElementScroll();
		return Vector2f(scroll->GetScrollbarSize(ElementScroll::VERTICAL), scroll->GetScrollbarSize(ElementScroll::HORIZONTAL));
	};
	auto scroll_size = [&](const char* id) {
		Element* element = document->GetElementById(id);
		return Vector2f(element->GetScrollWidth(), element->GetScrollHeight());
	};

	// its containing block is the scroll container: 160 wide (10 + 150), a horizontal scrollbar only
	CHECK(scrollbars("a") == Vector2f(0.f, 15.f));
	CHECK(scroll_size("a") == Vector2f(160.f, 85.f));

	// its containing block is a box inside the scroll container: 5 + 30 + 120 = 155 high, a vertical scrollbar only
	CHECK(scrollbars("b") == Vector2f(15.f, 0.f));
	CHECK(scroll_size("b") == Vector2f(85.f, 155.f));

	// 'overflow: hidden': no scrollbars, but scrollable to it
	CHECK(scrollbars("c") == Vector2f(0.f, 0.f));
	CHECK(scroll_size("c") == Vector2f(180.f, 190.f));
	Element* c = document->GetElementById("c");
	c->SetScrollTop(1000.f);
	context->Update();
	CHECK(c->GetScrollTop() == 90.f);

	// its containing block is outside the scroll container: not its overflow
	CHECK(scrollbars("d-scroller") == Vector2f(0.f, 0.f));
	CHECK(scroll_size("d-scroller") == Vector2f(100.f, 100.f));

	// a box positioned in an absolutely positioned box: 40 + 100 and 60 + 100, both scrollbars
	CHECK(scrollbars("e") == Vector2f(15.f, 15.f));
	CHECK(scroll_size("e") == Vector2f(140.f, 160.f));

	document->Close();
	TestsShell::ShutdownShell();
}

// The scrollable overflow of a scroll container includes the block-end margin of its last in-flow content, also one
// collapsed through a child (Chromium: 150px + 20px margin is 170px; with 5px padding 175px).
TEST_CASE("ScrollContainer.EndMargins")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const String rml = R"(
<rml>
<head>
	<style>
		body { width: 600px; height: 400px; margin: 0; padding: 0; }
		div { display: block; }
		scrollbarvertical { width: 0px; }
		scrollbarhorizontal { height: 0px; }
		.s { overflow: auto; width: 100px; height: 100px; }
		.child { height: 150px; margin-bottom: 20px; }
	</style>
</head>
<body>
	<div class="s" id="a"><div class="child"/></div>
	<div class="s" id="b"><div><div class="child"/></div></div>
	<div class="s" id="c" style="padding-bottom: 5px"><div class="child"/></div>
	<div class="s" id="d" style="display: flex; flex-direction: column"><div class="child" style="flex: none"/></div>
</body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	CHECK(document->GetElementById("a")->GetScrollHeight() == 170.f);
	CHECK(document->GetElementById("b")->GetScrollHeight() == 170.f);
	CHECK(document->GetElementById("c")->GetScrollHeight() == 175.f);
	CHECK(document->GetElementById("d")->GetScrollHeight() == 170.f);
	document->Close();
	TestsShell::ShutdownShell();
}
