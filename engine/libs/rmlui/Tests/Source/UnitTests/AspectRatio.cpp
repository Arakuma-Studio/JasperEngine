#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.12.5): 'aspect-ratio' (CSS Sizing 4 §5) in block, flex and
// absolute layout.

namespace {

const String aspect_ratio_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 900px; margin: 0; padding: 0; }
		div { display: block; }
		.flex { display: flex; width: 200px; height: 100px; align-items: flex-start; }
		.relative { position: relative; width: 200px; height: 100px; }
	</style>
</head>
<body>
	<div id="from-width" style="width: 100px; aspect-ratio: 2"/>
	<div id="from-height" style="height: 60px; aspect-ratio: 1.5"/>
	<div id="fraction" style="width: 160px; aspect-ratio: 16 / 9"/>
	<div id="auto-ratio" style="width: 100px; aspect-ratio: auto 4 / 1"/>
	<div id="both" style="width: 100px; height: 30px; aspect-ratio: 1"/>
	<div id="min-height" style="width: 100px; aspect-ratio: 2; min-height: 80px"/>
	<div id="max-height" style="width: 100px; aspect-ratio: 2; max-height: 20px"/>
	<div id="content" style="width: 100px; aspect-ratio: 2"><div style="height: 70px"/></div>
	<div id="content-box" style="width: 100px; padding: 10px; aspect-ratio: 2"/>
	<div id="border-box" style="width: 100px; padding: 10px; aspect-ratio: 2; box-sizing: border-box"/>
	<div class="flex"><div id="item" style="width: 50px; aspect-ratio: 1"/><div id="item-height" style="height: 40px; aspect-ratio: 2"/></div>
	<div class="flex" style="flex-direction: column; align-items: stretch"><div id="column-item" style="aspect-ratio: 4"/></div>
	<div class="relative"><div id="absolute" style="position: absolute; left: 0; right: 0; aspect-ratio: 4"/></div>
	<div class="relative"><div id="absolute-height" style="position: absolute; top: 10px; bottom: 30px; aspect-ratio: 0.5"/></div>
</body>
</rml>
)";

} // namespace

TEST_CASE("AspectRatio")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(aspect_ratio_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto size = [&](const char* id) { return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border); };

	SUBCASE("Computed value")
	{
		CHECK(document->GetElementById("from-width")->GetComputedValues().aspect_ratio() == doctest::Approx(2.f));
		CHECK(document->GetElementById("fraction")->GetComputedValues().aspect_ratio() == doctest::Approx(16.f / 9.f));
		CHECK(document->GetElementById("auto-ratio")->GetComputedValues().aspect_ratio() == doctest::Approx(4.f));
		CHECK(document->GetElementById("content")->GetFirstChild()->GetComputedValues().aspect_ratio() == 0.f);
	}

	SUBCASE("Block boxes")
	{
		CHECK(size("from-width") == Vector2f(100.f, 50.f));
		// A definite height gives the automatic width.
		CHECK(size("from-height") == Vector2f(90.f, 60.f));
		CHECK(size("fraction") == Vector2f(160.f, 90.f));
		CHECK(size("auto-ratio") == Vector2f(100.f, 25.f));
		// Both sizes specified: the ratio does not apply.
		CHECK(size("both") == Vector2f(100.f, 30.f));
	}

	SUBCASE("Minimum and maximum sizes")
	{
		CHECK(size("min-height") == Vector2f(100.f, 80.f));
		CHECK(size("max-height") == Vector2f(100.f, 20.f));
		// The automatic minimum height is the height of the content.
		CHECK(size("content") == Vector2f(100.f, 70.f));
	}

	SUBCASE("The ratio is that of the box-sizing box")
	{
		CHECK(size("content-box") == Vector2f(120.f, 70.f));
		CHECK(size("border-box") == Vector2f(100.f, 50.f));
	}

	SUBCASE("Flex items")
	{
		CHECK(size("item") == Vector2f(50.f, 50.f));
		CHECK(size("item-height") == Vector2f(80.f, 40.f));
		CHECK(size("column-item") == Vector2f(200.f, 50.f));
	}

	SUBCASE("Absolutely positioned boxes")
	{
		CHECK(size("absolute") == Vector2f(200.f, 50.f));
		// The height between 'top' and 'bottom' gives the width.
		CHECK(size("absolute-height") == Vector2f(30.f, 60.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
