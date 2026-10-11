#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.12.6): vertical margin collapsing (CSS 2.1 8.3.1) between a
// block and its first and last in-flow children, through empty blocks, and between siblings; what prevents it.

namespace {

const String margin_collapse_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 600px; margin: 0; padding: 0; }
		div { display: block; }
		.root { position: absolute; width: 100px; }
	</style>
</head>
<body>
	<div class="root" style="left: 0"><div id="parent" style="margin-top: 10px"><div id="first" style="margin-top: 20px; height: 5px"/></div></div>
	<div class="root" style="left: 100px"><div id="bordered" style="margin-top: 10px; border-top: 1px solid #000"><div id="inside" style="margin-top: 20px; height: 5px"/></div></div>
	<div class="root" style="left: 200px"><div id="last-parent" style="margin-bottom: 10px"><div style="margin-bottom: 30px; height: 5px"/></div><div id="after" style="height: 5px"/></div>
	<div class="root" style="left: 300px"><div style="height: 5px; margin-bottom: 10px"/><div id="empty" style="margin-top: 15px; margin-bottom: 25px"/><div id="next" style="height: 5px; margin-top: 5px"/></div>
	<div class="root" style="left: 0; top: 200px"><div style="height: 5px; margin-bottom: 20px"/><div id="negative" style="height: 5px; margin-top: -5px"/></div>
	<div class="root" style="left: 100px; top: 200px"><div id="bfc" style="margin-top: 10px; overflow: hidden"><div id="bfc-child" style="margin-top: 20px; height: 5px"/></div></div>
	<div class="root" style="left: 200px; top: 200px"><div id="min-height" style="min-height: 50px"><div style="height: 5px; margin-bottom: 30px"/></div><div id="min-height-next" style="height: 5px"/></div>
	<div class="root" style="left: 300px; top: 200px"><div id="grandparent" style="margin-top: 5px"><div style="margin-top: 15px"><div id="grandchild" style="margin-top: 25px; height: 5px"/></div></div></div>
</body>
</rml>
)";

} // namespace

TEST_CASE("MarginCollapse")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(margin_collapse_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto top = [&](const char* id) { return document->GetElementById(id)->GetAbsoluteOffset(BoxArea::Border).y; };
	auto height = [&](const char* id) { return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border).y; };

	SUBCASE("Parent and first child")
	{
		// The larger margin (20px) is above the parent's border box; the child starts at its top.
		CHECK(top("parent") == doctest::Approx(20.f));
		CHECK(top("first") == doctest::Approx(20.f));
		CHECK(height("parent") == doctest::Approx(5.f));
	}

	SUBCASE("A border separates them")
	{
		CHECK(top("bordered") == doctest::Approx(10.f));
		CHECK(top("inside") == doctest::Approx(10.f + 1.f + 20.f));
	}

	SUBCASE("Parent and last child")
	{
		// The child's bottom margin collapses through the auto-height parent with the parent's: 30px.
		CHECK(height("last-parent") == doctest::Approx(5.f));
		CHECK(top("after") == doctest::Approx(5.f + 30.f));
	}

	SUBCASE("Through an empty block")
	{
		// 10px, the empty block's 15px and 25px and the next block's 5px collapse into 25px.
		CHECK(height("empty") == doctest::Approx(0.f));
		CHECK(top("next") == doctest::Approx(5.f + 25.f));
	}

	SUBCASE("A negative margin")
	{
		CHECK(top("negative") == doctest::Approx(200.f + 5.f + 20.f - 5.f));
	}

	SUBCASE("A block formatting context root keeps its child's margin")
	{
		CHECK(top("bfc") == doctest::Approx(210.f));
		CHECK(top("bfc-child") == doctest::Approx(230.f));
	}

	SUBCASE("A min-height that decides the height keeps the last child's margin inside")
	{
		CHECK(height("min-height") == doctest::Approx(50.f));
		CHECK(top("min-height-next") == doctest::Approx(250.f));
	}

	SUBCASE("A chain of first children")
	{
		CHECK(top("grandparent") == doctest::Approx(225.f));
		CHECK(top("grandchild") == doctest::Approx(225.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
