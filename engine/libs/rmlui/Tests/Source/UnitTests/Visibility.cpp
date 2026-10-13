#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-paint (docs/html-ui/phase-3-fork-patches.md 3.8.3): a hidden element is laid out but neither painted nor
// hit; its descendants with 'visibility: visible' are painted and hit. 'collapse' is 'hidden'.

namespace {

const String document_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; }
	div { display: block; }
	#parent { width: 200px; height: 100px; background-color: #f00; visibility: hidden; }
	#child { width: 50px; height: 50px; background-color: #00f; }
	#after { width: 50px; height: 50px; }
</style>
</head>
<body>
	<div id="parent"><div id="child"/></div>
	<div id="after"/>
</body>
</rml>
)";

} // namespace

TEST_CASE("Visibility")
{
	Context* context = TestsShell::GetContext();
	TestsRenderInterface* render_interface = TestsShell::GetTestsRenderInterface();
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	Element* parent = document->GetElementById("parent");
	Element* child = document->GetElementById("child");
	Element* after = document->GetElementById("after");
	TestsShell::RenderLoop();

	auto count_draws = [&] {
		context->Update();
		render_interface->Reset();
		context->Render();
		return render_interface->GetCounters().render_geometry;
	};

	// Laid out: the next element is below the hidden one.
	CHECK(after->GetAbsoluteTop() == doctest::Approx(100.f));
	CHECK(child->GetComputedValues().visibility() == Style::Visibility::Hidden);

	// Neither the parent nor its hidden child are drawn or hit.
	const size_t draws_hidden = count_draws();
	CHECK(context->GetElementAtPoint(Vector2f(10.f, 10.f)) != parent);
	CHECK(context->GetElementAtPoint(Vector2f(10.f, 10.f)) != child);
	CHECK(context->GetElementAtPoint(Vector2f(150.f, 10.f)) != parent);

	// A visible child of the hidden parent is drawn and hit; the parent still is not.
	child->SetProperty("visibility", "visible");
	CHECK(count_draws() == draws_hidden + 1);
	CHECK(context->GetElementAtPoint(Vector2f(10.f, 10.f)) == child);
	CHECK(context->GetElementAtPoint(Vector2f(150.f, 10.f)) != parent);

	// The parent visible: drawn too.
	parent->SetProperty("visibility", "visible");
	CHECK(count_draws() == draws_hidden + 2);
	CHECK(context->GetElementAtPoint(Vector2f(150.f, 10.f)) == parent);

	// 'collapse' hides as 'hidden'.
	parent->SetProperty("visibility", "collapse");
	context->Update();
	CHECK(parent->GetComputedValues().visibility() == Style::Visibility::Hidden);
	CHECK(context->GetElementAtPoint(Vector2f(150.f, 10.f)) != parent);

	// A hidden document hides everything in it, whatever its elements say.
	document->Hide();
	CHECK(count_draws() == 0);
	Element* hit = context->GetElementAtPoint(Vector2f(10.f, 10.f));
	CHECK((hit == nullptr || hit->GetOwnerDocument() != document));

	document->Close();
	TestsShell::ShutdownShell();
}
