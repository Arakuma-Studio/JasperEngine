#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (p3-layout, docs/html-ui/phase-3-fork-patches.md 3.12.8): an element with 'display: contents' generates no box;
// its children are laid out and painted in its place, and it still takes part in events and inheritance.

namespace {

const String display_contents_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 300px; margin: 0; padding: 0; }
		div { display: block; }
		#flex { display: flex; width: 200px; height: 50px; }
		#wrapper { display: contents; padding: 10px; border: 5px #f00; background: #0f0; margin: 20px; color: #123456; }
		.item { width: 30px; height: 20px; }
		#block { width: 100px; }
		#inner { display: contents; }
	</style>
</head>
<body>
	<div id="flex"><div id="first" class="item"/><div id="wrapper"><div id="a" class="item"/><div id="b" class="item"/></div><div id="last" class="item"/></div>
	<div id="block"><div id="inner"><div id="c" style="height: 10px"/><div id="d" style="height: 15px"/></div><div id="e" style="height: 5px"/></div>
</body>
</rml>
)";

class CountingListener final : public EventListener {
public:
	void ProcessEvent(Event& /*event*/) override { count += 1; }
	int count = 0;
};

} // namespace

TEST_CASE("DisplayContents")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(display_contents_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();

	Element* wrapper = document->GetElementById("wrapper");
	auto offset = [&](const char* id) { return document->GetElementById(id)->GetAbsoluteOffset(BoxArea::Border); };

	SUBCASE("Its children are items of its parent's flex container")
	{
		// No box: its padding, border and margin take no space.
		CHECK(offset("first") == Vector2f(0.f, 0.f));
		CHECK(offset("a") == Vector2f(30.f, 0.f));
		CHECK(offset("b") == Vector2f(60.f, 0.f));
		CHECK(offset("last") == Vector2f(90.f, 0.f));
		CHECK(document->GetElementById("a")->GetOffsetParent() == document->GetElementById("flex"));
		CHECK(wrapper->GetBox().GetSize(BoxArea::Border) == Vector2f(0.f, 0.f));
	}

	SUBCASE("In a block container")
	{
		CHECK(offset("c").y == doctest::Approx(50.f));
		CHECK(offset("d").y == doctest::Approx(60.f));
		CHECK(offset("e").y == doctest::Approx(75.f));
		CHECK(document->GetElementById("block")->GetBox().GetSize().y == doctest::Approx(30.f));
	}

	SUBCASE("Inheritance and events go through it")
	{
		CHECK(document->GetElementById("a")->GetComputedValues().color() == Colourb(0x12, 0x34, 0x56));

		// The pointer hits its child, never the element itself; the event bubbles through it.
		CHECK(context->GetElementAtPoint(Vector2f(35.f, 5.f)) == document->GetElementById("a"));
		CountingListener listener;
		wrapper->AddEventListener(EventId::Click, &listener);
		context->ProcessMouseMove(35, 5, 0);
		context->ProcessMouseButtonDown(0, 0);
		context->ProcessMouseButtonUp(0, 0);
		CHECK(listener.count == 1);
		wrapper->RemoveEventListener(EventId::Click, &listener);
	}

	SUBCASE("Changing display")
	{
		// As a block it is a flex item with a box of its own, holding its children.
		wrapper->SetProperty("display", "block");
		context->Update();
		CHECK(wrapper->GetBox().GetSize(BoxArea::Border).x > 0.f);
		CHECK(document->GetElementById("a")->GetOffsetParent() == wrapper);

		// And back: the box is gone.
		wrapper->SetProperty("display", "contents");
		context->Update();
		CHECK(wrapper->GetBox().GetSize(BoxArea::Border) == Vector2f(0.f, 0.f));
		CHECK(offset("a") == Vector2f(30.f, 0.f));
		CHECK(offset("last") == Vector2f(90.f, 0.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
