#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.12.4): the sizing keywords of 'width', 'height', 'min-*' and
// 'max-*' (CSS Sizing 3 and 4): min-content, max-content, fit-content, fit-content(<length-percentage>), stretch.

namespace {

// Each .box holds two 60px inline blocks: its max-content width is 120px, its min-content width 60px.
const String sizing_keywords_rml = R"RML(
<rml>
<head>
	<style>
		body { width: 400px; height: 900px; margin: 0; padding: 0; }
		div { display: block; }
		span { display: inline-block; width: 60px; height: 10px; }
		.container { width: 100px; }
		.wide { width: 300px; }
	</style>
</head>
<body>
	<div class="container">
		<div id="auto"><span/><span/></div>
		<div id="max" style="width: max-content"><span/><span/></div>
		<div id="min" style="width: min-content"><span/><span/></div>
		<div id="fit" style="width: fit-content"><span/><span/></div>
		<div id="fit-length" style="width: fit-content(80px)"><span/><span/></div>
		<div id="fit-percent" style="width: fit-content(70%)"><span/><span/></div>
		<div id="min-max" style="width: 10px; min-width: max-content"><span/><span/></div>
		<div id="max-min" style="width: 300px; max-width: min-content"><span/><span/></div>
		<div id="float-stretch" style="float: left; width: stretch; margin: 0 10px"><span/></div>
		<div id="float-fill" style="float: left; clear: left; width: -webkit-fill-available; margin: 0 5px"><span/></div>
		<div id="height-max" style="clear: left; height: max-content"><div style="height: 30px"/></div>
		<div id="min-auto" style="width: 20px; min-width: auto"><span/></div>
	</div>
	<div class="wide">
		<div id="fit-wide" style="width: fit-content"><span/><span/></div>
		<div id="fit-padding" style="width: fit-content; padding: 0 5px; border: 2px solid #000"><span/></div>
	</div>
	<div class="wide" style="position: relative; height: 50px">
		<div id="absolute-fit" style="position: absolute; left: 0; right: 0; width: fit-content"><span/></div>
		<div id="absolute-stretch" style="position: absolute; top: 20px; width: stretch; margin: 0 20px"><span/></div>
	</div>
</body>
</rml>
)RML";

} // namespace

TEST_CASE("SizingKeywords")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(sizing_keywords_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto width = [&](const char* id) { return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border).x; };
	auto height = [&](const char* id) { return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border).y; };

	SUBCASE("Computed values")
	{
		using Style::SizingKeyword;
		const ComputedValues& max = document->GetElementById("max")->GetComputedValues();
		CHECK(max.width_keyword() == SizingKeyword::MaxContent);
		CHECK(max.width().type == Style::Width::Auto);
		CHECK(document->GetElementById("fit-length")->GetComputedValues().width_keyword() == SizingKeyword::FitContentLength);
		CHECK(document->GetElementById("min-max")->GetComputedValues().min_width_keyword() == SizingKeyword::MaxContent);
		CHECK(document->GetElementById("max-min")->GetComputedValues().max_width_keyword() == SizingKeyword::MinContent);
		CHECK(document->GetElementById("float-fill")->GetComputedValues().width_keyword() == SizingKeyword::Stretch);
		CHECK(document->GetElementById("auto")->GetComputedValues().width_keyword() == SizingKeyword::None);
	}

	SUBCASE("Widths")
	{
		CHECK(width("auto") == doctest::Approx(100.f));
		CHECK(width("max") == doctest::Approx(120.f));
		CHECK(width("min") == doctest::Approx(60.f));
		// fit-content: min(max-content, max(min-content, available)).
		CHECK(width("fit") == doctest::Approx(100.f));
		CHECK(width("fit-wide") == doctest::Approx(120.f));
		CHECK(width("fit-length") == doctest::Approx(80.f));
		CHECK(width("fit-percent") == doctest::Approx(70.f));
		// The padding and border are around the content size.
		CHECK(width("fit-padding") == doctest::Approx(60.f + 10.f + 4.f));
	}

	SUBCASE("Minimum and maximum widths")
	{
		CHECK(width("min-max") == doctest::Approx(120.f));
		CHECK(width("max-min") == doctest::Approx(60.f));
		// 'min-width: auto' is 0 outside flex and grid containers.
		CHECK(width("min-auto") == doctest::Approx(20.f));
	}

	SUBCASE("stretch")
	{
		// The margin box fills the containing block, also where the width would shrink to fit.
		CHECK(width("float-stretch") == doctest::Approx(80.f));
		CHECK(width("float-fill") == doctest::Approx(90.f));
		CHECK(width("absolute-stretch") == doctest::Approx(260.f));
	}

	SUBCASE("Heights")
	{
		// The content-based keywords of a block's height are its automatic height.
		CHECK(height("height-max") == doctest::Approx(30.f));
	}

	SUBCASE("Absolutely positioned boxes")
	{
		// 'fit-content' instead of filling the space between 'left' and 'right'.
		CHECK(width("absolute-fit") == doctest::Approx(60.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
