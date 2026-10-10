#include "../../../Source/Core/Layout/LayoutDetails.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.6, FORK.md P3-7): the boxes a container lays out, in
// order (pseudo-elements, 'display: contents' children in place of their parent, flex items by 'order'), the paint order
// that follows it, and the display and position values phase 3 adds.

static const String layout_children_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 400px; }
		div { display: block; }
		.flex { display: flex; height: 20px; }
		.flex > div { width: 10px; height: 10px; }
		.wide { width: 20px; }
	</style>
</head>
<body>
	<div class="flex" id="ordered"><div id="o2" style="order: 2"/><div id="o0a"/><div id="om1" style="order: -1"/><div id="o0b"/></div>
	<div id="block"><span id="contents" style="display: contents"><div id="inner" style="height: 10px"/><div id="inner2" style="height: 5px"/></span><div id="after" style="height: 5px"/></div>
	<div class="flex" id="flex-contents"><div id="x"/><span style="display: contents"><div id="y" class="wide"/><div id="z"/></span></div>
	<div class="flex" id="paint"><div id="p" style="order: 1; width: 30px; height: 30px; margin-right: -20px"/><div id="q" style="width: 30px; height: 30px; margin-right: -20px"/></div>
	<div id="item" style="display: list-item; height: 7px"/><div id="sticky" style="position: sticky; top: 5px; left: 3px; height: 4px"/>
</body>
</rml>
)";

TEST_CASE("LayoutChildren.order_contents_and_pseudo_elements")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(layout_children_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto ids = [](const LayoutChildList& list) {
		String result;
		for (Element* element : list)
		{
			if (element->GetId().empty())
				continue; // white space between the elements
			result += (result.empty() ? "" : " ") + element->GetId();
		}
		return result;
	};

	LayoutChildList children;
	Element* ordered = document->GetElementById("ordered");
	LayoutDetails::GetLayoutChildren(ordered, children, false);
	CHECK(ids(children) == "o2 o0a om1 o0b");
	LayoutDetails::GetLayoutChildren(ordered, children, true);
	CHECK(ids(children) == "om1 o0a o0b o2"); // stable among equal orders

	LayoutDetails::GetLayoutChildren(document->GetElementById("block"), children, false);
	CHECK(ids(children) == "inner inner2 after");

	// no pseudo-element is generated yet
	for (PseudoElementType type : {PseudoElementType::Before, PseudoElementType::After, PseudoElementType::Marker})
		CHECK(ordered->GetPseudoElement(type) == nullptr);

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("LayoutChildren.layout")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(layout_children_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto left = [&](const char* id) { return document->GetElementById(id)->GetRelativeOffset(BoxArea::Border).x; };
	auto top = [&](const char* id) { return document->GetElementById(id)->GetRelativeOffset(BoxArea::Border).y; };

	SUBCASE("flex items in order")
	{
		CHECK(left("om1") == 0.f);
		CHECK(left("o0a") == 10.f);
		CHECK(left("o0b") == 20.f);
		CHECK(left("o2") == 30.f);
	}

	SUBCASE("display: contents in a block container")
	{
		// laid out as children of #block
		CHECK(document->GetElementById("contents")->GetComputedValues().display() == Style::Display::Contents);
		CHECK(top("inner") == 0.f);
		CHECK(top("inner2") == 10.f);
		CHECK(top("after") == 15.f);
		CHECK(document->GetElementById("block")->GetBox().GetSize().y == 20.f);
	}

	SUBCASE("display: contents in a flex container: its children are flex items")
	{
		CHECK(left("x") == 0.f);
		CHECK(left("y") == 10.f);
		CHECK(left("z") == 30.f);
	}

	SUBCASE("list-item and sticky")
	{
		Element* item = document->GetElementById("item");
		CHECK(item->GetComputedValues().display() == Style::Display::ListItem);
		CHECK(item->GetBox().GetSize().x == 400.f); // a block box
		CHECK(item->GetBox().GetSize().y == 7.f);
		// sticky is laid out in flow: its insets only keep it in the scrollport (p3-layout, PositionSticky.cpp). Here it is
		// in view, and as wide as its containing block.
		Element* sticky = document->GetElementById("sticky");
		CHECK(sticky->GetComputedValues().position() == Style::Position::Sticky);
		CHECK(sticky->GetRelativeOffset(BoxArea::Border).y == doctest::Approx(item->GetRelativeOffset(BoxArea::Border).y + 7.f));
		CHECK(sticky->GetRelativeOffset(BoxArea::Border).x == doctest::Approx(0.f));
	}

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("LayoutChildren.paint_order_follows_order")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(layout_children_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();

	// Each item overlaps the next one with its negative margin, so the one placed (and painted) second is on top: #p
	// (order 1) after #q.
	Element* p = document->GetElementById("p");
	Element* q = document->GetElementById("q");
	CHECK(p->GetAbsoluteLeft() == doctest::Approx(q->GetAbsoluteLeft() + 10.f));
	CHECK(context->GetElementAtPoint(q->GetAbsoluteOffset(BoxArea::Border) + Vector2f(20.f, 15.f)) == p);

	// in DOM order again, #q is placed and painted second
	p->SetProperty("order", "0");
	context->Update();
	context->Render();
	CHECK(q->GetAbsoluteLeft() == doctest::Approx(p->GetAbsoluteLeft() + 10.f));
	CHECK(context->GetElementAtPoint(p->GetAbsoluteOffset(BoxArea::Border) + Vector2f(20.f, 15.f)) == q);

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("LayoutChildren.values")
{
	TestsShell::GetContext();
	PropertyDictionary dictionary;
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "display", "contents"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "display", "list-item"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "position", "sticky"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "order", "-3"));
	// grid layout is not there yet: the keywords stay unknown until it is (ForkFeature::Grid's probe)
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "display", "grid"));
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "display", "inline-grid"));
	TestsShell::ShutdownShell();
}
