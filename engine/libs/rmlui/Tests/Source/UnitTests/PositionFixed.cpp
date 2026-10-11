#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.11.1): 'position: fixed' is positioned and sized against the
// viewport, does not move when an ancestor or the document scrolls, and is not clipped by the ancestors it leaves. An
// ancestor with a transform is its containing block instead.

namespace {

const String position_fixed_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 300px; margin: 0; padding: 0; overflow: auto; }
		div { display: block; }
		#scroller { overflow: auto; width: 100px; height: 100px; margin-left: 40px; }
		#fixed { position: fixed; top: 10px; right: 20px; width: 50px; height: 20px; }
		#percent { position: fixed; left: 50%; top: 25%; width: 10%; height: 10%; }
		#transformed { transform: translateX(0px); width: 200px; height: 100px; padding: 5px; margin-left: 30px; }
		#inside { position: fixed; left: 10px; top: 20px; width: 50%; height: 10px; }
	</style>
</head>
<body>
	<div id="scroller">
		<div style="height: 60px"/>
		<div id="fixed"/>
		<div id="percent"/>
		<div style="height: 500px"/>
	</div>
	<div id="transformed"><div id="inside"/></div>
	<div style="height: 1000px"/>
</body>
</rml>
)";

} // namespace

TEST_CASE("PositionFixed")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(position_fixed_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	const Vector2f viewport = Vector2f(context->GetDimensions());
	Element* scroller = document->GetElementById("scroller");
	Element* fixed = document->GetElementById("fixed");
	Element* percent = document->GetElementById("percent");
	Element* transformed = document->GetElementById("transformed");
	Element* inside = document->GetElementById("inside");

	auto offset = [](Element* element) { return element->GetAbsoluteOffset(BoxArea::Border); };
	auto size = [](Element* element) { return element->GetBox().GetSize(BoxArea::Border); };

	SUBCASE("The viewport is the containing block")
	{
		CHECK(offset(fixed).x == doctest::Approx(viewport.x - 20.f - 50.f));
		CHECK(offset(fixed).y == doctest::Approx(10.f));
		CHECK(offset(percent).x == doctest::Approx(0.5f * viewport.x));
		CHECK(offset(percent).y == doctest::Approx(0.25f * viewport.y));
		CHECK(size(percent).x == doctest::Approx(0.1f * viewport.x));
		CHECK(size(percent).y == doctest::Approx(0.1f * viewport.y));
		CHECK(fixed->GetOffsetParent() == document);
	}

	SUBCASE("No scroll moves it")
	{
		const Vector2f before = offset(fixed);
		scroller->SetScrollTop(40.f);
		document->SetScrollTop(70.f);
		context->Update();
		REQUIRE(scroller->GetScrollTop() == 40.f);
		REQUIRE(document->GetScrollTop() == 70.f);
		CHECK(offset(fixed) == before);
		CHECK(offset(percent).y == doctest::Approx(0.25f * viewport.y));
	}

	SUBCASE("The scroll container it is in does not clip it, nor take it into its scrollable area")
	{
		// The fixed box is far outside the 100px scroller.
		Rectanglei clip_region;
		if (ElementUtilities::GetClippingRegion(fixed, clip_region))
		{
			CHECK(float(clip_region.Left()) <= offset(fixed).x);
			CHECK(float(clip_region.Right()) >= Math::Min(offset(fixed).x + 50.f, 400.f));
			CHECK(clip_region.Width() > 100);
		}
		CHECK(scroller->GetScrollWidth() == doctest::Approx(scroller->GetClientWidth()));
		CHECK(scroller->GetScrollHeight() == doctest::Approx(560.f));
	}

	SUBCASE("Hit testing follows it")
	{
		// Inside the document's box, so that the document does not clip it.
		fixed->SetProperty("right", "auto");
		fixed->SetProperty("left", "300px");
		scroller->SetScrollTop(40.f);
		context->Update();
		CHECK(offset(fixed) == Vector2f(300.f, 10.f));
		CHECK(context->GetElementAtPoint(Vector2f(310.f, 15.f)) == fixed);
	}

	SUBCASE("A transformed ancestor is the containing block")
	{
		// Its padding box: at the ancestor's border box origin (no border), 200px + 2 * 5px wide.
		CHECK(inside->GetOffsetParent() == transformed);
		CHECK(offset(inside) == offset(transformed) + Vector2f(10.f, 20.f));
		CHECK(size(inside).x == doctest::Approx(0.5f * 210.f));

		// It scrolls with that ancestor.
		const Vector2f before = offset(inside);
		document->SetScrollTop(30.f);
		context->Update();
		CHECK(offset(inside) == before - Vector2f(0.f, 30.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
