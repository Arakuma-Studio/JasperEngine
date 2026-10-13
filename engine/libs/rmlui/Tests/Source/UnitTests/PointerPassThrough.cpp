#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <doctest.h>

using namespace Rml;

// Fork, phase 4 (docs/html-ui/FORK.md P4-1): Context::SetPointerPassThrough. An element the filter names is not hit
// itself; its descendants are, and the point goes on to the documents below.

namespace {

const String lower_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; }
	#a { display: block; position: absolute; left: 0; top: 0; width: 100px; height: 100px; }
</style>
</head>
<body id="lower"><div id="a"/></body>
</rml>
)";

const String upper_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; }
	#b { display: block; position: absolute; left: 200px; top: 0; width: 100px; height: 100px; }
	#child { display: block; position: absolute; left: 50px; top: 50px; width: 100px; height: 100px; }
</style>
</head>
<body id="upper"><div id="b"/><div id="child"/></body>
</rml>
)";

struct ClickCounter : EventListener {
	int clicks = 0;
	void ProcessEvent(Event&) override { clicks++; }
};

} // namespace

TEST_CASE("PointerPassThrough.documents")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* lower = context->LoadDocumentFromMemory(lower_rml);
	ElementDocument* upper = context->LoadDocumentFromMemory(upper_rml);
	REQUIRE(lower);
	REQUIRE(upper);
	lower->Show();
	upper->Show();
	context->PullDocumentToFront(upper);
	context->Update();

	Element* a = lower->GetElementById("a");
	Element* b = upper->GetElementById("b");
	Element* child = upper->GetElementById("child");

	// Without a filter the upper document's body takes every point it covers.
	CHECK(context->GetElementAtPoint({10.f, 10.f}) == upper);
	CHECK(context->GetElementAtPoint({250.f, 10.f}) == b);

	context->SetPointerPassThrough([](Element* element) { return element->GetTagName() == "body"; });
	CHECK(context->GetElementAtPoint({10.f, 10.f}) == a);
	CHECK(context->GetElementAtPoint({250.f, 10.f}) == b);
	CHECK(context->GetElementAtPoint({75.f, 75.f}) == child);
	CHECK(context->GetElementAtPoint({350.f, 250.f}) == nullptr);

	// Hover and clicks follow it.
	ClickCounter counter;
	a->AddEventListener(EventId::Click, &counter);
	context->ProcessMouseMove(10, 10, 0);
	CHECK(context->GetHoverElement() == a);
	context->ProcessMouseButtonDown(0, 0);
	CHECK(a->IsPseudoClassSet("active"));
	context->ProcessMouseButtonUp(0, 0);
	CHECK(counter.clicks == 1);
	context->ProcessMouseMove(350, 250, 0);
	CHECK(context->GetHoverElement() == nullptr);
	a->RemoveEventListener(EventId::Click, &counter);

	context->SetPointerPassThrough(nullptr);
	CHECK(context->GetElementAtPoint({10.f, 10.f}) == upper);
	context->ProcessMouseLeave();

	upper->Close();
	lower->Close();
	TestsShell::ShutdownShell();
}
