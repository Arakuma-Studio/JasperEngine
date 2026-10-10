#include "../../../Source/Core/ListMarker.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.6.4): 'display: list-item' elements get a ::marker from
// their list-style-* values (or a ::marker rule's content); outside markers sit before the item without changing its
// size or content position, inside markers are the first inline box of its first line.

static const String markers_rml = R"RML(
<rml>
<head>
	<style>
		body { font-family: LatoLatin; font-size: 20px; line-height: 24px; }
		ol, ul { display: block; counter-reset: list-item; margin: 0; padding: 0 0 0 50px; width: 200px; }
		ol { list-style-type: decimal; }
		li { display: list-item; }
		#upper { list-style-type: upper-roman; }
		#inside { list-style-position: inside; }
		#none { list-style-type: none; }
		#custom li::marker { content: "*" counter(list-item) "*"; color: #f00; }
		#string { list-style-type: "-> "; }
		#short { list-style: square inside; }
		.block { display: block; }
	</style>
</head>
<body>
	<ol id="ol"><li id="o1">one</li><li id="o2">two</li></ol>
	<ol id="upper"><li id="u1">a</li><li id="u2">b</li><li id="u3">c</li><li id="u4">d</li></ol>
	<ul id="ul"><li id="d1">x</li></ul>
	<ol id="inside"><li id="i1">x</li></ol>
	<ul id="none"><li id="z1">x</li></ul>
	<ol id="custom"><li id="c1">x</li></ol>
	<ul id="string"><li id="s1">x</li></ul>
	<ul id="short"><li id="q1">x</li></ul>
	<ul><li id="b1" class="block">x</li></ul>
</body>
</rml>
)RML";

static ElementListMarker* MarkerOf(Element* element)
{
	return element ? rmlui_dynamic_cast<ElementListMarker*>(element->GetPseudoElement(PseudoElementType::Marker)) : nullptr;
}

TEST_CASE("ListMarkers")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(markers_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Update();

	auto ById = [&](const char* id) { return document->GetElementById(id); };

	SUBCASE("generated text")
	{
		REQUIRE(MarkerOf(ById("o1")));
		CHECK(MarkerOf(ById("o1"))->GetMarkerText() == "1. ");
		CHECK(MarkerOf(ById("o2"))->GetMarkerText() == "2. ");
		CHECK(MarkerOf(ById("u4"))->GetMarkerText() == "IV. ");
		REQUIRE(MarkerOf(ById("d1")));
		CHECK(MarkerOf(ById("d1"))->GetMarkerStyle() == "disc");
		CHECK(MarkerOf(ById("d1"))->GetMarkerText() == "");
		CHECK(MarkerOf(ById("s1"))->GetMarkerText() == "-> ");
		CHECK(MarkerOf(ById("q1"))->GetMarkerStyle() == "square");
		CHECK_FALSE(ById("z1")->GetPseudoElement(PseudoElementType::Marker));
		CHECK_FALSE(ById("b1")->GetPseudoElement(PseudoElementType::Marker));
	}

	SUBCASE("::marker content")
	{
		ElementListMarker* marker = MarkerOf(ById("c1"));
		REQUIRE(marker);
		REQUIRE(marker->GetNumChildren() == 1);
		auto* text = rmlui_dynamic_cast<ElementText*>(marker->GetChild(0));
		REQUIRE(text);
		CHECK(text->GetText() == "*1*");
		CHECK(marker->GetComputedValues().color() == Colourb(255, 0, 0));
	}

	SUBCASE("outside markers do not move the content")
	{
		Element* item = ById("o1");
		ElementListMarker* marker = MarkerOf(item);
		REQUIRE(marker);
		CHECK(marker->GetComputedValues().display() == Style::Display::InlineBlock);
		// The marker ends at the item's start edge.
		const float marker_right = marker->GetAbsoluteLeft() + marker->GetBox().GetSize(BoxArea::Border).x;
		CHECK(marker_right == doctest::Approx(item->GetAbsoluteLeft()));
		CHECK(item->GetAbsoluteLeft() == 50.f);
		// The item's text starts at its content edge.
		Element* text = item->GetChild(0);
		CHECK(text->GetAbsoluteLeft() == doctest::Approx(item->GetAbsoluteLeft()).epsilon(0.01));
		CHECK(item->GetBox().GetSize().y == 24.f);
	}

	SUBCASE("inside markers come first on the line")
	{
		Element* item = ById("i1");
		ElementListMarker* marker = MarkerOf(item);
		REQUIRE(marker);
		CHECK(marker->GetComputedValues().display() == Style::Display::Inline);
		Element* text = item->GetChild(0);
		CHECK(text->GetAbsoluteLeft() > item->GetAbsoluteLeft() + 10.f);
	}

	SUBCASE("changes")
	{
		ById("ol")->SetProperty("list-style-type", "lower-alpha");
		context->Update();
		CHECK(MarkerOf(ById("o2"))->GetMarkerText() == "b. ");
		ById("o1")->SetProperty("display", "block");
		context->Update();
		CHECK_FALSE(ById("o1")->GetPseudoElement(PseudoElementType::Marker));
		CHECK(MarkerOf(ById("o2"))->GetMarkerText() == "a. ");
	}

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("ListMarkers.Properties")
{
	TestsShell::GetContext();
	PropertyDictionary dictionary;
	auto parse = [&](const String& name, const String& value) {
		dictionary.Clear();
		return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
	};
	CHECK(parse("list-style-type", "lower-roman"));
	CHECK(dictionary.GetProperty(PropertyId::ListStyleType)->ToString() == "lower-roman");
	CHECK(parse("list-style-type", "\"\\2192  \""));
	CHECK(dictionary.GetProperty(PropertyId::ListStyleType)->Get<String>() == "\xE2\x86\x92 ");
	// another counter style counts in decimal
	CHECK(parse("list-style-type", "armenian"));
	CHECK(dictionary.GetProperty(PropertyId::ListStyleType)->ToString() == "decimal");
	CHECK(parse("list-style", "none"));
	CHECK(dictionary.GetProperty(PropertyId::ListStyleType)->ToString() == "none");
	CHECK(dictionary.GetProperty(PropertyId::ListStyleImage)->ToString() == "none");
	CHECK(dictionary.GetProperty(PropertyId::ListStylePosition)->ToString() == "outside");
	CHECK(parse("list-style", "url(\"a b.png\") inside \"*\""));
	CHECK(dictionary.GetProperty(PropertyId::ListStyleImage)->Get<String>() == "a b.png");
	CHECK(dictionary.GetProperty(PropertyId::ListStyleType)->Get<String>() == "*");
	CHECK(dictionary.GetProperty(PropertyId::ListStylePosition)->ToString() == "inside");
	CHECK_FALSE(parse("list-style", "none none none"));
	CHECK(parse("counter-reset", "a 2 b reversed(c)"));
	CHECK(dictionary.GetProperty(PropertyId::CounterReset)->ToString() == "a 2 b reversed(c)");
	CHECK_FALSE(parse("counter-increment", "reversed(c)"));
	CHECK(parse("content", "\"x\" attr(title) counter(a, upper-roman) counters(b, \".\") open-quote url(i.png) / \"alt\""));
	CHECK(dictionary.GetProperty(PropertyId::Content)->ToString() ==
		"\"x\" attr(title) counter(a, upper-roman) counters(b, \".\") open-quote url(\"i.png\")");
	CHECK(parse("quotes", "\"a\" \"b\""));
	CHECK_FALSE(parse("quotes", "\"a\""));
	TestsShell::ShutdownShell();
}
