#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

static const String document_flex_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			width: 500px;
			height: 300px;
			background: #333;
		}
		#flex {
			background: #666;
			display: flex;
			width: 50%;
			height: 30%;
		}
		input.checkbox {
			background: #fff;
			border: 1px solid #000;
		}
	</style>
</head>

<body>
	<div id="flex">
		<input type="checkbox" id="checkbox"/>
	</div>
</body>
</rml>
)";

TEST_CASE("FlexFormatting")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(document_flex_rml);
	REQUIRE(document);
	document->Show();

	Element* checkbox = document->GetElementById("checkbox");
	Element* flex = document->GetElementById("flex");

	struct TestCase {
		String flex_direction;
		String align_items;
		Vector2f expected_size;
	};
	// The checkbox is a replaced element with a natural aspect ratio (1): stretched to a definite cross size, its main size
	// follows through the ratio (CSS Flexbox 9.2 step 3B, as images in browsers; phase 3, p3-layout's flex layout).
	TestCase test_cases[] = {
		{"column", "stretch", Vector2f(248, 248)},
		{"column", "center", Vector2f(16, 16)},
		{"row", "stretch", Vector2f(88, 88)},
		{"row", "center", Vector2f(16, 16)},
	};

	for (auto& test_case : test_cases)
	{
		flex->SetProperty("flex-direction", test_case.flex_direction);
		flex->SetProperty("align-items", test_case.align_items);

		TestsShell::RenderLoop();

		CAPTURE(test_case.align_items);
		CAPTURE(test_case.flex_direction);
		CHECK(checkbox->GetBox().GetSize().x == doctest::Approx(test_case.expected_size.x));
		CHECK(checkbox->GetBox().GetSize().y == doctest::Approx(test_case.expected_size.y));
	}

	document->Close();

	TestsShell::ShutdownShell();
}

static const String document_flex_dp_ratio_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			width: 100%;
			height: 100%;
		}
		.window {
			width: 1920dp;
			height: 1080dp;
			margin: 0 auto;
			display: flex;
			flex-direction: column;
		}
		.inner {
			flex-grow: 1;
			display: flex;
			flex-direction: row;
		}
		.left {
			width: 30%;
			margin: 32dp;
			overflow: auto;
			background: #522;
		}
		.right {
			flex: 1;
			margin: 32dp;
			overflow: auto;
			background: #252;
		}
	</style>
</head>

<body>
	<div class="window">
		<div class="inner">
			<div class="left"/>
			<div class="right"/>
		</div>
	</div>
</body>
</rml>
)";

TEST_CASE("FlexFormatting.dp_ratio")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(document_flex_dp_ratio_rml);
	REQUIRE(document);
	document->Show();

	Element* window = TestsShell::GetElementChild(document, 0);

	constexpr float native_width = 1920.f;
	constexpr float native_height = 1080.f;

	const float test_window_widths[] = {
		3440.f,
		2960.f,
		2880.f,
		2560.f,
		2400.f,
		2048.f,
		1921.f,
		1920.f,
		1919.f,
		1600.f,
		1366.f,
		1280.f,
		1024.f,
	};

	for (float window_width : test_window_widths)
	{
		const float dp_ratio = window_width / native_width;
		CAPTURE(window_width);
		CAPTURE(dp_ratio);

		context->SetDensityIndependentPixelRatio(dp_ratio);

		TestsShell::RenderLoop();

		CHECK(window->GetBox().GetSize().x == window_width);
		CHECK(window->GetBox().GetSize().y == doctest::Approx((window_width / native_width) * native_height));
	}

	document->Close();

	TestsShell::ShutdownShell();
}

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.12): 'order', the automatic minimum size, 'flex-basis:
// content', the alignment keywords of CSS Box Alignment 3, and inline-flex parity.
static const String document_flex_parity_rml = R"(
<rml>
<head>
	<style>
		body { width: 600px; height: 900px; margin: 0; padding: 0; }
		div { display: block; box-sizing: border-box; }
		.flex { display: flex; width: 200px; height: 50px; }
		.w20 { width: 20px; height: 10px; }
		.w30 { width: 30px; height: 10px; }
		.wide { width: 150px; height: 10px; }
	</style>
</head>
<body>
	<div id="order" class="flex"><div id="o1" class="w20" style="order: 2"/><div id="o2" class="w30"/><div id="o3" style="width: 40px; order: -1"/></div>
	<div id="min-auto" class="flex"><div id="m1" style="flex: 1 1 0px"><div class="wide"/></div><div id="m2" style="flex: 1 1 0px"/></div>
	<div id="min-zero" class="flex"><div id="z1" style="flex: 1 1 0px; min-width: 0"><div class="wide"/></div><div id="z2" style="flex: 1 1 0px"/></div>
	<div id="min-scroll" class="flex"><div id="s1" style="flex: 1 1 0px; overflow: hidden"><div class="wide"/></div><div id="s2" style="flex: 1 1 0px"/></div>
	<div id="min-column" class="flex" style="flex-direction: column"><div id="c1" style="flex: 1 1 0px"><div style="height: 40px"/></div><div id="c2" style="flex: 1 1 0px"/></div>
	<div id="basis" class="flex"><div id="b1" style="width: 120px; flex: 0 0 content"><div class="w30"/></div><div id="b2" style="width: 120px; flex: 0 0 auto"><div class="w30"/></div></div>
	<div id="justify-end" class="flex" style="justify-content: end"><div id="je" class="w20"/></div>
	<div id="justify-right" class="flex" style="justify-content: right"><div id="jr" class="w20"/></div>
	<div id="reverse-start" class="flex" style="flex-direction: row-reverse; justify-content: start"><div id="rs1" class="w20"/><div id="rs2" class="w30"/></div>
	<div id="reverse-flex-start" class="flex" style="flex-direction: row-reverse; justify-content: flex-start"><div id="rf1" class="w20"/><div id="rf2" class="w30"/></div>
	<div id="align-end" class="flex" style="align-items: end"><div id="ae" class="w20"/></div>
	<div id="align-self-start" class="flex" style="align-items: end"><div id="as" class="w20" style="align-self: start"/></div>
	<div id="center" class="flex" style="align-items: center"><div id="uc" style="width: 20px; height: 80px"/></div>
	<div id="safe-center" class="flex" style="align-items: safe center"><div id="sc" style="width: 20px; height: 80px"/></div>
	<div id="content-end" class="flex" style="flex-wrap: wrap; align-content: end"><div id="ce" class="w20"/></div>
	<div style="height: 60px"><div id="inline-flex" style="display: inline-flex"><div id="if1" style="width: 20px"><div style="margin-top: 20px; height: 5px"/></div><div id="if2" style="width: 30px; flex: none"/></div></div>
</body>
</rml>
)";

TEST_CASE("FlexFormatting.parity")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(document_flex_parity_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	// The position of an element's border box in its parent's border box.
	auto x = [&](const char* id) {
		Element* element = document->GetElementById(id);
		return element->GetAbsoluteOffset(BoxArea::Border).x - element->GetParentNode()->GetAbsoluteOffset(BoxArea::Border).x;
	};
	auto y = [&](const char* id) {
		Element* element = document->GetElementById(id);
		return element->GetAbsoluteOffset(BoxArea::Border).y - element->GetParentNode()->GetAbsoluteOffset(BoxArea::Border).y;
	};
	auto width = [&](const char* id) { return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border).x; };
	auto height = [&](const char* id) { return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border).y; };

	SUBCASE("order")
	{
		CHECK(x("o3") == doctest::Approx(0.f));
		CHECK(x("o2") == doctest::Approx(40.f));
		CHECK(x("o1") == doctest::Approx(70.f));

		// Changing it lays the items out again.
		document->GetElementById("o2")->SetProperty("order", "5");
		context->Update();
		CHECK(x("o1") == doctest::Approx(40.f));
		CHECK(x("o2") == doctest::Approx(60.f));
	}

	SUBCASE("The automatic minimum size is the content size")
	{
		// 'min-width: auto': the item does not shrink below its 150px content.
		CHECK(width("m1") == doctest::Approx(150.f));
		CHECK(width("m2") == doctest::Approx(50.f));
		// 'min-width: 0' and scroll containers have none.
		CHECK(width("z1") == doctest::Approx(100.f));
		CHECK(width("z2") == doctest::Approx(100.f));
		CHECK(width("s1") == doctest::Approx(100.f));
		// In a column, 'min-height: auto'.
		CHECK(height("c1") == doctest::Approx(40.f));
		CHECK(height("c2") == doctest::Approx(10.f));
	}

	SUBCASE("flex-basis: content")
	{
		// Sized by its content, whatever its 'width'; 'auto' takes the width.
		CHECK(width("b1") == doctest::Approx(30.f));
		CHECK(width("b2") == doctest::Approx(120.f));
	}

	SUBCASE("justify-content keywords")
	{
		CHECK(x("je") == doctest::Approx(180.f));
		CHECK(x("jr") == doctest::Approx(180.f));
		// 'start' follows the writing mode, not the reversed main axis: the items are packed to the left.
		CHECK(x("rs2") == doctest::Approx(0.f));
		CHECK(x("rs1") == doctest::Approx(30.f));
		// 'flex-start' follows the main axis: packed to the right.
		CHECK(x("rf1") == doctest::Approx(180.f));
		CHECK(x("rf2") == doctest::Approx(150.f));
	}

	SUBCASE("align keywords")
	{
		CHECK(y("ae") == doctest::Approx(40.f));
		CHECK(y("as") == doctest::Approx(0.f));
		CHECK(y("ce") == doctest::Approx(40.f));
		// An overflowing item is centered, unless 'safe': then it stays at the start.
		CHECK(y("uc") == doctest::Approx(-15.f));
		CHECK(y("sc") == doctest::Approx(0.f));
	}

	SUBCASE("inline-flex")
	{
		// Its children are flex items: side by side, each a formatting context of its own (the margin of the first one's
		// child stays inside it).
		CHECK(x("if1") == doctest::Approx(0.f));
		CHECK(x("if2") == doctest::Approx(20.f));
		CHECK(height("if1") == doctest::Approx(25.f));
		CHECK(height("if2") == doctest::Approx(25.f));
		CHECK(width("inline-flex") == doctest::Approx(50.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
