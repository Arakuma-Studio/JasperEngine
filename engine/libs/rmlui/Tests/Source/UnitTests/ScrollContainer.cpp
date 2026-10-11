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

