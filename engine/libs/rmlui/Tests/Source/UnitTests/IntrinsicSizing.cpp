#include "../../../Source/Core/Layout/LayoutDetails.h"
#include "../../../Source/Core/Layout/LayoutEngine.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.8, FORK.md P3-8): min-content and max-content widths of
// block, inline and flex content (longest word, nowrap, floats, inline-blocks), the height of an element formatted at a
// given width, and the per-layout measurement cache.

static const String intrinsic_rml = R"(
<rml>
<head>
	<style>
		body { font-family: LatoLatin; font-size: 20px; width: 800px; }
		div { display: block; }
		.measure { display: inline-block; white-space: nowrap; }
		.flex { display: flex; }
		.column { display: flex; flex-direction: column; }
		.wrap { display: flex; flex-wrap: wrap; }
	</style>
</head>
<body>
	<span class="measure" id="w-aa">aa</span><span class="measure" id="w-long">longestword</span><span class="measure" id="w-all">aa longestword b</span>
	<div id="text">aa longestword b</div>
	<div id="nowrap" style="white-space: nowrap">aa longestword b</div>
	<div id="padded" style="padding: 0 3px; border: 2px #000">aa longestword b</div>
	<div id="fixed" style="width: 33px; padding-left: 2px">aa longestword b</div>
	<div id="floats"><div style="float: left; width: 50px; height: 5px"/><div style="float: left; width: 70px; height: 5px"/></div>
	<div id="nested"><div><p>aa longestword b</p></div></div>
	<div id="inline-block"><span style="display: inline-block">aa longestword b</span></div>
	<div id="flex-row" class="flex"><div>aa</div><div>longestword</div></div>
	<div id="flex-row-gap" class="flex" style="column-gap: 10px"><div>aa</div><div>longestword</div></div>
	<div id="flex-column" class="column"><div>aa</div><div>longestword</div></div>
	<div id="flex-wrap" class="wrap"><div>aa</div><div>longestword</div></div>
</body>
</rml>
)";

TEST_CASE("IntrinsicSizing.min_and_max_content_widths")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(intrinsic_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto width_of = [&](const char* id) { return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border).x; };
	const float aa = width_of("w-aa");
	const float longest = width_of("w-long");
	const float all = width_of("w-all");
	REQUIRE(aa > 0.f);
	REQUIRE(longest > aa);
	REQUIRE(all > longest);

	const Vector2f containing_block(800.f, -1.f);
	auto min_content = [&](const char* id) { return LayoutDetails::GetMinContentWidth(document->GetElementById(id), containing_block); };
	auto max_content = [&](const char* id) { return LayoutDetails::GetMaxContentWidth(document->GetElementById(id), containing_block); };

	SUBCASE("text: the longest word, the whole line")
	{
		CHECK(min_content("text") == doctest::Approx(longest));
		CHECK(max_content("text") == doctest::Approx(all));
		CHECK(min_content("nested") == doctest::Approx(longest));
		CHECK(max_content("nested") == doctest::Approx(all));
	}

	SUBCASE("nowrap: no soft wrap opportunity")
	{
		CHECK(min_content("nowrap") == doctest::Approx(all));
		CHECK(max_content("nowrap") == doctest::Approx(all));
	}

	SUBCASE("border-box widths; a definite width is its own")
	{
		CHECK(min_content("padded") == doctest::Approx(longest + 10.f));
		CHECK(max_content("padded") == doctest::Approx(all + 10.f));
		CHECK(min_content("fixed") == doctest::Approx(35.f));
		CHECK(max_content("fixed") == doctest::Approx(35.f));
	}

	SUBCASE("floats: the widest one, all side by side")
	{
		CHECK(min_content("floats") == doctest::Approx(70.f));
		CHECK(max_content("floats") == doctest::Approx(120.f));
	}

	SUBCASE("an inline-block takes its own min-content width")
	{
		CHECK(min_content("inline-block") == doctest::Approx(longest));
		CHECK(max_content("inline-block") == doctest::Approx(all));
	}

	SUBCASE("flex containers")
	{
		CHECK(min_content("flex-row") == doctest::Approx(aa + longest));
		CHECK(max_content("flex-row") == doctest::Approx(aa + longest));
		CHECK(min_content("flex-row-gap") == doctest::Approx(aa + longest + 10.f));
		CHECK(min_content("flex-column") == doctest::Approx(longest));
		CHECK(min_content("flex-wrap") == doctest::Approx(longest));
	}

	// A measurement formats the element (as layout does when it measures a flex item); the layout that follows places
	// it again.
	document->GetElementById("text")->SetProperty("width", "auto"); // (dirties the layout)
	context->Update();
	CHECK(width_of("text") == doctest::Approx(800.f));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("IntrinsicSizing.height_for_width")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(intrinsic_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* text = document->GetElementById("text");
	Element* padded = document->GetElementById("padded");
	const Vector2f containing_block(800.f, -1.f);
	const float all = document->GetElementById("w-all")->GetBox().GetSize(BoxArea::Border).x;
	const float one_line = LayoutDetails::GetContentHeightForWidth(text, 800.f, containing_block);
	REQUIRE(one_line > 0.f);
	CHECK(LayoutDetails::GetContentHeightForWidth(text, all + 1.f, containing_block) == doctest::Approx(one_line));
	// narrower than the line, wider than the longest word: two lines
	const float two_lines = LayoutDetails::GetContentHeightForWidth(text, all - 1.f, containing_block);
	CHECK(two_lines == doctest::Approx(2.f * one_line));
	// the border box: borders and padding included
	CHECK(LayoutDetails::GetContentHeightForWidth(padded, 800.f, containing_block) == doctest::Approx(one_line + 4.f));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("IntrinsicSizing.measure_cache")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(intrinsic_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	CHECK(LayoutMeasureCache::Get() == nullptr); // only during LayoutEngine::FormatElement
	Element* text = document->GetElementById("text");
	const Vector2f containing_block(800.f, -1.f);
	{
		LayoutMeasureCache::Scope scope;
		REQUIRE(LayoutMeasureCache::Get() == &scope.GetCache());
		const float first = LayoutDetails::GetMinContentWidth(text, containing_block);
		const size_t entries = scope.GetCache().Size();
		CHECK(entries >= 1);
		CHECK(LayoutDetails::GetMinContentWidth(text, containing_block) == first);
		CHECK(scope.GetCache().Size() == entries); // answered from the cache
		LayoutDetails::GetMinContentWidth(text, Vector2f(400.f, -1.f));
		LayoutDetails::GetMaxContentWidth(text, containing_block);
		LayoutDetails::GetContentHeightForWidth(text, 100.f, containing_block);
		CHECK(scope.GetCache().Size() == entries + 3);
		{
			LayoutMeasureCache::Scope nested;
			CHECK(LayoutMeasureCache::Get() == &nested.GetCache());
		}
		CHECK(LayoutMeasureCache::Get() == &scope.GetCache());
	}
	CHECK(LayoutMeasureCache::Get() == nullptr);

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("IntrinsicSizing.percentage_max_width_while_shrinking")
{
	// Upstream PR #983 (FORK.md P3-1): a percentage max-width is indefinite while the shrink-to-fit width of its
	// container is measured (6.3 resolved it against 0 and the container collapsed); it then applies to the final width.
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(R"(
<rml>
<head><style>body { font-family: LatoLatin; font-size: 20px; width: 800px; } div { display: block; }</style></head>
<body>
	<span id="measure" style="display: inline-block; white-space: nowrap">aa longestword b</span>
	<div id="shrink" style="display: inline-block"><div id="limited" style="max-width: 50%; white-space: nowrap">aa longestword b</div></div>
</body>
</rml>)");
	REQUIRE(document);
	document->Show();
	context->Update();

	const float all = document->GetElementById("measure")->GetBox().GetSize(BoxArea::Border).x;
	REQUIRE(all > 0.f);
	CHECK(document->GetElementById("shrink")->GetBox().GetSize(BoxArea::Border).x == doctest::Approx(all));
	CHECK(document->GetElementById("limited")->GetBox().GetSize(BoxArea::Border).x == doctest::Approx(all * 0.5f));

	document->Close();
	TestsShell::ShutdownShell();
}
