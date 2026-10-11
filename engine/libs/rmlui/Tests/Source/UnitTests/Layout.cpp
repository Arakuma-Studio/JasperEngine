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

static const String document_layout_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			width: 500px;
			height: 300px;
			top: 100px;
			left: 100px;
			border: 10px #fff;
			background-color: #ccc;
		}
		#relative {
			width: 100%;
			height: 25%;
			background-color: red;
			position: relative;
			top: 50%;
		}
	</style>
</head>

<body>
	<div id="relative"/>
</body>
</rml>
)";

static const String document_layout_rml_nested = R"(
<rml>
<head>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			width: 500px;
			height: 300px;
			top: 100px;
			left: 100px;
			border: 10px #fff;
			background-color: #ccc;
		}
		#parent {
			background-color: green;
			position: relative;
			top: 50%;
		}
		#relative {
			width: 100%;
			height: 25%;
			background-color: red;
			position: relative;
			top: 50%;
		}
	</style>
</head>

<body>
	<div id="parent">
		<div id="relative"/>
	</div>
</body>
</rml>
)";

TEST_CASE("Layout.Position.Relative")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	// Test that percentage positioning in 'position: relative' elements is correctly resolved during the first layout run, and
	// does not change during the next layout run. See issue: https://github.com/mikke89/RmlUi/issues/262

	for (auto&& rml_source : {document_layout_rml, document_layout_rml_nested})
	{
		ElementDocument* document = context->LoadDocumentFromMemory(rml_source);
		REQUIRE(document);
		document->Show();

		Element* element = document->GetElementById("relative");
		REQUIRE(element);

		TestsShell::RenderLoop();

		const float absolute_top = element->GetAbsoluteTop();
		CHECK(absolute_top >= 150.f);

		// This forces a new layout run but shouldn't make any difference to the rendered output.
		document->SetProperty("width", "500px");
		TestsShell::RenderLoop();

		CHECK(absolute_top == element->GetAbsoluteTop());

		document->SetProperty("width", "400px");
		TestsShell::RenderLoop();

		CHECK(absolute_top == element->GetAbsoluteTop());

		document->Close();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("Layout.ScrollableOverflow.MarginTriggersScrollbar")
{
	const String document_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/../Tests/Data/style.rcss"/>
	<style>
		scrollbarvertical { width: 15px; }
		scrollbarhorizontal { height: 15px; }
		.scroller { width: 100px; height: 100px; background: #3a3; }
		.item { background: #33a; }
		.vertical { overflow-x: hidden; overflow-y: auto; }
		.horizontal { overflow-x: auto; overflow-y: hidden; }
		.flex { display: flex; }
		.flex.vertical { flex-direction: column; }
		.flex.horizontal { flex-direction: row; }
		.flex .item { flex-shrink: 0; }
		.vertical .item { width: 20px; height: 95px; margin-bottom: 20px; }
		.horizontal .item { width: 95px; height: 20px; margin-right: 20px; }
		.vertical .item.fits { height: 80px; }
	</style>
</head>
<body>
	<div class="scroller vertical"><div class="item"/></div>
	<div class="scroller flex vertical"><div class="item"/></div>
	<div class="scroller flex horizontal"><div class="item"/></div>

	<div class="scroller flex vertical"><div class="item fits"/></div>
	<div class="scroller vertical"><div class="item fits"/></div>
</body>
</rml>
)";

	// A direct child whose margin box extends past the scroll container's padding area should enable an auto-scrollbar, even
	// when the child's border box alone fits.
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	CHECK(TestsShell::GetElementChild(document, 0)->GetScrollHeight() == doctest::Approx(115.f));
	CHECK(TestsShell::GetElementChild(document, 0)->GetClientWidth() == doctest::Approx(100.f - 15.f)); // Height - Scrollbar

	CHECK(TestsShell::GetElementChild(document, 1)->GetScrollHeight() == doctest::Approx(115.f));
	CHECK(TestsShell::GetElementChild(document, 1)->GetClientWidth() == doctest::Approx(100.f - 15.f)); // Height - Scrollbar

	CHECK(TestsShell::GetElementChild(document, 2)->GetScrollWidth() == doctest::Approx(115.f));
	CHECK(TestsShell::GetElementChild(document, 2)->GetClientHeight() == doctest::Approx(100.f - 15.f)); // Width - Scrollbar

	// The last two items fits just about, no scrollbars should appear.
	CHECK(TestsShell::GetElementChild(document, 3)->GetScrollHeight() == doctest::Approx(100.f));
	CHECK(TestsShell::GetElementChild(document, 3)->GetClientWidth() == doctest::Approx(100.f)); // No scrollbar.

	CHECK(TestsShell::GetElementChild(document, 4)->GetScrollHeight() == doctest::Approx(100.f));
	CHECK(TestsShell::GetElementChild(document, 4)->GetClientWidth() == doctest::Approx(100.f)); // No scrollbar.

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("Layout.ScrollableOverflow.AutoHeightHorizontalScrollbar")
{
	const String document_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/../Tests/Data/style.rcss"/>
	<style>
		scrollbarvertical { width: 15px; }
		scrollbarhorizontal { height: 15px; }
		.scroller { width: 120px; border: 2px #000; overflow: auto; background: #3a3; }
		.scroller div { background: #33a; }
		.flex { display: flex; flex-direction: row; }
		.item { flex-shrink: 0; width: 240px; height: 60px; }
		.tall { height: 200px; }
		.fixed { height: 80px; }
	</style>
</head>
<body>
	<div class="scroller flex"><div class="item"/></div>
	<div class="scroller"><div class="item"/></div>

	<div class="scroller flex fixed"><div class="item tall"/></div>
	<div class="scroller fixed"><div class="item tall"/></div>
</body>
</rml>
)";

	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	// Auto height, horizontal scrollbar only.
	for (int i = 0; i < 2; i++)
	{
		INFO("Scroller: " << i);
		Element* e = TestsShell::GetElementChild(document, i);
		CHECK(e->GetClientWidth() == doctest::Approx(120.f)); // No vertical scrollbar.
		CHECK(e->GetClientHeight() == doctest::Approx(60.f)); // Container height - scrollbar height.
		CHECK(e->GetScrollWidth() == doctest::Approx(240.f));
	}

	// Fixed height with overflow on both axes, both scrollbars needed.
	for (int i = 2; i < 4; i++)
	{
		INFO("Scroller: " << i);
		Element* e = TestsShell::GetElementChild(document, i);
		CHECK(e->GetClientWidth() == doctest::Approx(105.f)); // Container width - vertical scrollbar.
		CHECK(e->GetClientHeight() == doctest::Approx(65.f)); // Container height - horizontal scrollbar.
	}

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("Layout.ScrollableOverflow.ChildMarginTriggersScrollbar")
{
	const String document_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/../Tests/Data/style.rcss"/>
	<style>
		scrollbarvertical { width: 15px; }
		scrollbarhorizontal { height: 15px; }
		.scroller { width: 100px; height: 80px; padding-bottom: 20px; overflow: auto; background: #3a3; }
		.flex { display: flex; }
		/* Height + margin-bottom overflows onto the container padding. Both child margin and container padding should be reachable by scrolling. */
		.child { flex-shrink: 0; width: 20px; height: 70px; margin-bottom: 25px; background: #33a; }
	</style>
</head>
<body>
	<div id="block" class="scroller"><div class="child"/></div>
	<div id="flex" class="scroller flex"><div class="child"/></div>
</body>
</rml>
)";

	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	for (const char* id : {"block", "flex"})
	{
		INFO("Scroller: " << id);
		Element* e = document->GetElementById(id);
		REQUIRE(e);
		CHECK(e->GetScrollHeight() == doctest::Approx(95.f + 20.f)); // Child height (w/margin) + container padding
		CHECK(e->GetClientWidth() == doctest::Approx(100.f - 15.f)); // Container width - Scrollbar width
	}

	document->Close();
	TestsShell::ShutdownShell();
}

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.11.3): 'overflow: clip' clips like 'hidden' but is not a
// scroll container, and the overflow values are fixed up across the two axes (CSS Overflow 3).
static const String document_overflow_clip_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 300px; margin: 0; padding: 0; }
		div { display: block; }
		#scroller { overflow: auto; width: 200px; height: 100px; }
		#clip { overflow: clip; width: 100px; height: 50px; padding: 5px; }
		#clip-x { overflow-x: clip; width: 100px; height: 50px; }
		#clip-margin { overflow: clip; overflow-clip-margin: 8px; width: 100px; height: 50px; }
		.tall { width: 300px; height: 400px; }
		#fix1 { overflow-x: visible; overflow-y: hidden; }
		#fix2 { overflow-x: clip; overflow-y: scroll; }
		#fix3 { overflow-x: clip; overflow-y: visible; }
	</style>
</head>
<body>
	<div id="scroller"><div id="clip"><div id="clip-content" class="tall"/></div></div>
	<div id="clip-x"><div id="clip-x-content" class="tall"/></div>
	<div id="clip-margin"><div id="clip-margin-content" class="tall"/></div>
	<div id="fix1"/><div id="fix2"/><div id="fix3"/>
</body>
</rml>
)";

TEST_CASE("Layout.Overflow.Clip")
{
	using Style::Overflow;
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(document_overflow_clip_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();

	Element* scroller = document->GetElementById("scroller");
	Element* clip = document->GetElementById("clip");

	SUBCASE("Computed values and the fix-up")
	{
		CHECK(clip->GetComputedValues().overflow_x() == Overflow::Clip);
		CHECK(clip->GetComputedValues().overflow_y() == Overflow::Clip);
		// One axis scrolls: 'visible' computes to 'auto', 'clip' to 'hidden'.
		CHECK(document->GetElementById("fix1")->GetComputedValues().overflow_x() == Overflow::Auto);
		CHECK(document->GetElementById("fix1")->GetComputedValues().overflow_y() == Overflow::Hidden);
		CHECK(document->GetElementById("fix2")->GetComputedValues().overflow_x() == Overflow::Hidden);
		CHECK(document->GetElementById("fix2")->GetComputedValues().overflow_y() == Overflow::Scroll);
		// Neither axis scrolls: kept as is.
		CHECK(document->GetElementById("fix3")->GetComputedValues().overflow_x() == Overflow::Clip);
		CHECK(document->GetElementById("fix3")->GetComputedValues().overflow_y() == Overflow::Visible);
		CHECK(clip->GetProperty(PropertyId::OverflowX)->ToString() == "clip");
	}

	SUBCASE("Not scrollable, not even by script")
	{
		CHECK(clip->GetScrollHeight() > clip->GetClientHeight());
		clip->SetScrollTop(50.f);
		clip->SetScrollLeft(50.f);
		CHECK(clip->GetScrollTop() == 0.f);
		CHECK(clip->GetScrollLeft() == 0.f);
	}

	SUBCASE("Clipped content does not overflow the ancestor scroll container")
	{
		// #clip's border box is 110 x 60: its 300 x 400 child is clipped, so the scroller has nothing to scroll.
		CHECK(scroller->GetScrollHeight() == doctest::Approx(100.f));
		CHECK(scroller->GetScrollWidth() == doctest::Approx(200.f));
	}

	SUBCASE("Clipping region")
	{
		Rectanglei region;
		REQUIRE(ElementUtilities::GetClippingRegion(document->GetElementById("clip-content"), region));
		// The padding box of #clip (its default clip area), inside the scroller's own region.
		const Vector2f clip_offset = clip->GetAbsoluteOffset(BoxArea::Padding);
		CHECK(region.Left() == int(clip_offset.x));
		CHECK(region.Top() == int(clip_offset.y));
		CHECK(region.Width() == 110);
		CHECK(region.Height() == 60);

		// Only the x axis: unbounded vertically.
		REQUIRE(ElementUtilities::GetClippingRegion(document->GetElementById("clip-x-content"), region));
		CHECK(region.Width() == 100);
		CHECK(region.Height() > 10000);

		// Extended by the clip margin.
		Element* clip_margin = document->GetElementById("clip-margin");
		REQUIRE(ElementUtilities::GetClippingRegion(document->GetElementById("clip-margin-content"), region));
		CHECK(region.Left() == int(clip_margin->GetAbsoluteOffset(BoxArea::Padding).x) - 8);
		CHECK(region.Width() == 116);
		CHECK(region.Height() == 66);
	}

	document->Close();
	TestsShell::ShutdownShell();
}

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.11.3): 'scrollbar-gutter: stable' keeps the space of the
// vertical scrollbar of a scroll container, which is only shown while the content overflows.
TEST_CASE("Layout.ScrollbarGutter")
{
	const String document_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 600px; margin: 0; padding: 0; }
		div { display: block; }
		scrollbarvertical { width: 12px; }
		.scroller { overflow: auto; width: 100px; height: 50px; }
		.stable { scrollbar-gutter: stable; }
		.short { height: 20px; }
		.tall { height: 200px; }
	</style>
</head>
<body>
	<div id="auto" class="scroller"><div class="short"/></div>
	<div id="stable" class="scroller stable"><div id="stable-child" class="short"/></div>
	<div id="stable-overflow" class="scroller stable"><div class="tall"/></div>
	<div id="stable-hidden" class="scroller stable" style="overflow: hidden"><div class="tall"/></div>
	<div id="stable-visible" class="stable" style="width: 100px"><div class="short"/></div>
	<div id="both-edges" class="scroller" style="scrollbar-gutter: stable both-edges"><div class="short"/></div>
</body>
</rml>
)";

	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();

	auto client_width = [&](const char* id) { return document->GetElementById(id)->GetClientWidth(); };
	auto scrollbar_visible = [&](const char* id) {
		Element* scrollbar = document->GetElementById(id)->GetElementScroll()->GetScrollbar(ElementScroll::VERTICAL);
		return scrollbar && scrollbar->IsVisible();
	};

	CHECK(document->GetElementById("stable")->GetComputedValues().scrollbar_gutter() == Style::ScrollbarGutter::Stable);

	// Without a gutter the content takes the whole width until it overflows.
	CHECK(client_width("auto") == doctest::Approx(100.f));
	CHECK_FALSE(scrollbar_visible("auto"));

	// The gutter is there without overflow, and the scrollbar is not shown.
	CHECK(client_width("stable") == doctest::Approx(88.f));
	CHECK(document->GetElementById("stable-child")->GetBox().GetSize(BoxArea::Border).x == doctest::Approx(88.f));
	CHECK_FALSE(scrollbar_visible("stable"));

	// With overflow the scrollbar takes it.
	CHECK(client_width("stable-overflow") == doctest::Approx(88.f));
	CHECK(scrollbar_visible("stable-overflow"));

	// 'overflow: hidden' keeps the gutter, and never shows a scrollbar.
	CHECK(client_width("stable-hidden") == doctest::Approx(88.f));
	CHECK_FALSE(scrollbar_visible("stable-hidden"));

	// Not a scroll container: no gutter.
	CHECK(client_width("stable-visible") == doctest::Approx(100.f));

	// 'both-edges' is taken as 'stable'.
	CHECK(client_width("both-edges") == doctest::Approx(88.f));

	// Content that starts to overflow shows the scrollbar without changing the width of the content.
	document->GetElementById("stable-child")->SetProperty("height", "300px");
	context->Update();
	CHECK(client_width("stable") == doctest::Approx(88.f));
	CHECK(scrollbar_visible("stable"));

	document->Close();
	TestsShell::ShutdownShell();
}

// Phase 3 (p3-layout): the legacy 'text-align' values -webkit-left, -webkit-right and -webkit-center align the block-level
// children of the element (as 'justify-items: legacy' in browsers); 'justify-self' takes precedence.
TEST_CASE("Layout.TextAlignLegacy")
{
	const String document_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 300px; margin: 0; padding: 0; }
		div { display: block; height: 10px; }
		.parent { width: 200px; height: auto; }
		.child { width: 50px; }
	</style>
</head>
<body>
	<div class="parent" style="text-align: -webkit-center"><div id="center" class="child"/><div id="wide" style="max-width: 300px"/><div id="nested" style="height: auto"><div id="inherited" class="child"/></div></div>
	<div class="parent" style="text-align: -webkit-right"><div id="start" class="child" style="justify-self: start"/><div id="self-center" class="child" style="justify-self: center"/><div id="right" class="child"/></div>
	<div class="parent" style="text-align: center"><div id="plain" class="child"/></div>
</body>
</rml>
)";

	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto x = [&](const char* id) { return document->GetElementById(id)->GetAbsoluteOffset(BoxArea::Border).x; };
	CHECK(x("center") == doctest::Approx(75.f));
	CHECK(x("wide") == doctest::Approx(0.f));
	// Inherited with 'text-align'.
	CHECK(x("inherited") == doctest::Approx(75.f));
	CHECK(x("start") == doctest::Approx(0.f));
	CHECK(x("self-center") == doctest::Approx(75.f));
	CHECK(x("right") == doctest::Approx(150.f));
	// 'center' aligns the text only.
	CHECK(x("plain") == doctest::Approx(0.f));

	document->Close();
	TestsShell::ShutdownShell();
}
