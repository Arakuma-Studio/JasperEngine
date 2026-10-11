#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-paint (docs/html-ui/phase-3-fork-patches.md 3.8.2): z-index applies to positioned elements and to flex and
// grid items only; opacity, position fixed and sticky, isolation, will-change create stacking contexts.

namespace {

const String document_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; }
	div { display: block; }
	.box { width: 100px; height: 100px; }
	#second { margin-top: -50px; }
	#flex { display: flex; }
	#flex > div { width: 100px; height: 100px; }
	#f2 { margin-left: -50px; }
</style>
</head>
<body>
	<div id="first" class="box"/>
	<div id="second" class="box"/>
	<div id="flex"><div id="f1"/><div id="f2"/></div>
</body>
</rml>
)";

} // namespace

TEST_CASE("StackingContext.z_index")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	Element* first = document->GetElementById("first");
	Element* second = document->GetElementById("second");
	Element* f1 = document->GetElementById("f1");
	Element* f2 = document->GetElementById("f2");
	TestsShell::RenderLoop();

	const Vector2f overlap(20.f, 70.f);
	const Vector2f flex_overlap(70.f, 160.f);
	CHECK(context->GetElementAtPoint(overlap) == second);
	CHECK(context->GetElementAtPoint(flex_overlap) == f2);

	// On a static element that is not an item z-index is ignored.
	first->SetProperty("z-index", "10");
	TestsShell::RenderLoop();
	CHECK(first->GetZIndex() == 0.f);
	CHECK(context->GetElementAtPoint(overlap) == second);

	// Positioned, it applies.
	first->SetProperty("position", "relative");
	TestsShell::RenderLoop();
	CHECK(first->GetZIndex() == 10.f);
	CHECK(context->GetElementAtPoint(overlap) == first);

	// A flex item: z-index applies though it is static.
	f1->SetProperty("z-index", "1");
	TestsShell::RenderLoop();
	CHECK(f1->GetZIndex() == 1.f);
	CHECK(context->GetElementAtPoint(flex_overlap) == f1);

	// Not an item any more: ignored.
	document->GetElementById("flex")->SetProperty("display", "block");
	TestsShell::RenderLoop();
	CHECK(f1->GetZIndex() == 0.f);

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("StackingContext.creators")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	Element* first = document->GetElementById("first");
	Element* second = document->GetElementById("second");
	TestsShell::RenderLoop();

	// A child with a negative z-index is drawn below the content of its stacking context, and above the stacking
	// context's own background: with a stacking context on 'first', the child stays inside of it (above 'first') while
	// without one it goes below the body's content (below 'first').
	first->SetInnerRML("<div id='child' style='position: absolute; left: 0; top: 0; width: 20px; height: 20px; z-index: -1;'/>");
	Element* child = document->GetElementById("child");
	TestsShell::RenderLoop();
	const Vector2f at(10.f, 10.f);
	CHECK(context->GetElementAtPoint(at) == first);

	const std::pair<const char*, const char*> declarations[] = {{"opacity", "0.5"}, {"position", "fixed"}, {"position", "sticky"},
		{"isolation", "isolate"}, {"will-change", "transform"}, {"will-change", "opacity"}};
	for (const auto& declaration : declarations)
	{
		INFO(declaration.first << ": " << declaration.second);
		first->SetProperty(declaration.first, declaration.second);
		TestsShell::RenderLoop();
		CHECK(context->GetElementAtPoint(at) == child);
		first->RemoveProperty(declaration.first);
		TestsShell::RenderLoop();
		CHECK(context->GetElementAtPoint(at) == first);
	}
	(void)second;

	document->Close();
	TestsShell::ShutdownShell();
}
