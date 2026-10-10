#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Input.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.6.1, 3.6.2): ::before and ::after are generated from
// 'content', live as non-DOM children of their originating element (invisible to the DOM API, :nth-child() and
// queries), inherit from it, are laid out and hit-tested through it.

static String TextOf(Element* element)
{
	String text;
	if (!element)
		return text;
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		if (auto* text_element = rmlui_dynamic_cast<ElementText*>(element->GetChild(i)))
			text += text_element->GetText();
		else
			text += "[" + element->GetChild(i)->GetTagName() + "]";
	}
	return text;
}

static const String pseudo_rml = R"RML(
<rml>
<head>
	<style>
		body { font-family: LatoLatin; font-size: 16px; }
		div, p { display: block; }
		#a::before { content: "B"; display: block; width: 30px; height: 7px; color: #f00; }
		#a::after { content: "A" "Z"; }
		#b:before { content: "legacy"; }
		#c::before { content: none; }
		#d::before { content: normal; }
		#e::before { content: attr(data-label) " (" attr(data-n) ")"; }
		#f { color: #0f0; }
		#f::after { content: "x"; }
		#t::after { content: attr(title); }
		#g:hover::after { content: "hover"; }
		#h::before { content: "\201C" "\A" "end"; white-space: pre; }
		#i::before { content: "a" url(missing.png) "b"; }
		p:nth-child(2) { width: 77px; }
		#list p::before { content: "item"; }
		#hit { position: absolute; left: 100px; top: 100px; width: 50px; height: 20px; }
		#hit::after { content: ""; display: block; position: absolute; left: 60px; top: 0; width: 40px; height: 20px; }
	</style>
</head>
<body>
	<div id="a">text</div>
	<div id="b"/>
	<div id="c"/>
	<div id="d"/>
	<div id="e" data-label="Hello" data-n="3"/>
	<div id="f"/>
	<div id="g"/>
	<div id="h"/>
	<div id="i"/>
	<div id="list"><p id="p1"/><p id="p2"/><p id="p3"/></div>
	<div id="hit"/>
	<div id="t" title="TTT">x</div>
</body>
</rml>
)RML";

TEST_CASE("PseudoElements")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(pseudo_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Update();

	Element* a = document->GetElementById("a");

	SUBCASE("generation")
	{
		Element* before = a->GetPseudoElement(PseudoElementType::Before);
		Element* after = a->GetPseudoElement(PseudoElementType::After);
		REQUIRE(before);
		REQUIRE(after);
		CHECK(before->GetTagName() == "::before");
		CHECK(before->GetPseudoElementType() == PseudoElementType::Before);
		CHECK(after->GetPseudoElementType() == PseudoElementType::After);
		CHECK(a->GetPseudoElementType() == PseudoElementType::None);
		CHECK(TextOf(before) == "B");
		CHECK(TextOf(after) == "AZ");
		CHECK(before->GetComputedValues().display() == Style::Display::Block);
		CHECK(before->GetBox().GetSize().x == 30.f);
		CHECK(before->GetBox().GetSize().y == 7.f);
		CHECK(before->GetComputedValues().color() == Colourb(255, 0, 0));

		CHECK(document->GetElementById("b")->GetPseudoElement(PseudoElementType::Before));
		CHECK(TextOf(document->GetElementById("b")->GetPseudoElement(PseudoElementType::Before)) == "legacy");
		CHECK_FALSE(document->GetElementById("c")->GetPseudoElement(PseudoElementType::Before));
		CHECK_FALSE(document->GetElementById("d")->GetPseudoElement(PseudoElementType::Before));
		CHECK_FALSE(document->GetElementById("d")->GetPseudoElement(PseudoElementType::After));
	}

	SUBCASE("invisible to the DOM")
	{
		// The text child and nothing else
		CHECK(a->GetNumChildren() == 1);
		CHECK(a->GetNumChildren(true) >= 3);
		ElementList found;
		document->QuerySelectorAll(found, "#a *");
		CHECK(found.empty());
		CHECK(a->GetInnerRML() == "text");
		// :nth-child() counts the DOM children only
		CHECK(document->GetElementById("p2")->GetComputedValues().width().value == 77.f);
		CHECK(document->GetElementById("p1")->GetComputedValues().width().type == Style::Width::Auto);
	}

	SUBCASE("inherits from the originating element")
	{
		Element* after = document->GetElementById("f")->GetPseudoElement(PseudoElementType::After);
		REQUIRE(after);
		CHECK(after->GetComputedValues().color() == Colourb(0, 255, 0));
		document->GetElementById("f")->SetProperty("color", "#00f");
		context->Update();
		CHECK(after->GetComputedValues().color() == Colourb(0, 0, 255));
	}

	SUBCASE("attr() follows the attribute")
	{
		Element* e = document->GetElementById("e");
		Element* before = e->GetPseudoElement(PseudoElementType::Before);
		REQUIRE(before);
		CHECK(TextOf(before) == "Hello (3)");
		e->SetAttribute("data-label", "Bye");
		context->Update();
		CHECK(TextOf(before) == "Bye (3)");
		e->RemoveAttribute("data-n");
		context->Update();
		CHECK(TextOf(before) == "Bye ()");

		// A text that becomes empty draws nothing anymore.
		Element* h = document->GetElementById("t");
		Element* h_after = h->GetPseudoElement(PseudoElementType::After);
		REQUIRE(h_after);
		REQUIRE(h_after->GetNumChildren() == 1);
		auto* h_text = rmlui_dynamic_cast<ElementText*>(h_after->GetChild(0));
		REQUIRE(h_text);
		REQUIRE(h_text->GetLines().size() == 1);
		CHECK(h_text->GetLines().front().text == "TTT");
		h->RemoveAttribute("title");
		context->Update();
		CHECK(h_text->GetText() == "");
		for (const ElementText::Line& line : h_text->GetLines())
			CHECK(line.text == "");

		// The same for a text node of the document that ends its block.
		auto* dom_text = rmlui_dynamic_cast<ElementText*>(h->GetChild(0));
		REQUIRE(dom_text);
		REQUIRE(dom_text->GetLines().size() == 1);
		dom_text->SetText("");
		context->Update();
		CHECK(dom_text->GetLines().empty());
		dom_text->SetText("y");
		context->Update();
		REQUIRE(dom_text->GetLines().size() == 1);
		CHECK(dom_text->GetLines().front().text == "y");
	}

	SUBCASE("generated and removed with the originating element's state")
	{
		Element* g = document->GetElementById("g");
		CHECK_FALSE(g->GetPseudoElement(PseudoElementType::After));
		g->SetPseudoClass("hover", true);
		context->Update();
		REQUIRE(g->GetPseudoElement(PseudoElementType::After));
		CHECK(TextOf(g->GetPseudoElement(PseudoElementType::After)) == "hover");
		g->SetPseudoClass("hover", false);
		context->Update();
		CHECK_FALSE(g->GetPseudoElement(PseudoElementType::After));
	}

	SUBCASE("escapes and images")
	{
		CHECK(TextOf(document->GetElementById("h")->GetPseudoElement(PseudoElementType::Before)) == "\xE2\x80\x9C\nend");
		TestsShell::SetNumExpectedWarnings(-1);
		CHECK(TextOf(document->GetElementById("i")->GetPseudoElement(PseudoElementType::Before)) == "a[img]b");
	}

	SUBCASE("hit testing targets the originating element")
	{
		Element* hit = document->GetElementById("hit");
		REQUIRE(hit->GetPseudoElement(PseudoElementType::After));
		// inside the ::after box, outside the element's own box
		CHECK(context->GetElementAtPoint(Vector2f(170.f, 110.f)) == hit);
		context->ProcessMouseMove(170, 110, 0);
		context->Update();
		CHECK(hit->IsPseudoClassSet("hover"));
	}

	SUBCASE("destroyed with the originating element")
	{
		Element* list = document->GetElementById("list");
		Element* p1 = document->GetElementById("p1");
		REQUIRE(p1->GetPseudoElement(PseudoElementType::Before));
		list->RemoveChild(p1);
		context->Update();
		CHECK(list->GetNumChildren() == 2);
	}

	TestsShell::SetNumExpectedWarnings(0);
	document->Close();
	TestsShell::ShutdownShell();
}

static const String pseudo_transition_rml = R"RML(
<rml>
<head>
	<style>
		div { display: block; }
		#t { width: 50px; height: 20px; }
		#t::after { content: ""; display: block; width: 10px; height: 10px; transition: width 1s linear-in-out; }
		#t:hover::after { width: 30px; }
	</style>
</head>
<body><div id="t"/></body>
</rml>
)RML";

TEST_CASE("PseudoElements.Transition")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	REQUIRE(system_interface);
	system_interface->SetManualTime(0.0);
	ElementDocument* document = context->LoadDocumentFromMemory(pseudo_transition_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Update();

	Element* t = document->GetElementById("t");
	Element* after = t->GetPseudoElement(PseudoElementType::After);
	REQUIRE(after);
	CHECK(after->GetComputedValues().width().value == 10.f);

	// (RmlUi advances an animation by at most 0.1 s per update)
	auto AdvanceTo = [&](double end) {
		static double time = 0.0;
		while (time < end - 1e-9)
		{
			time = Math::Min(time + 0.05, end);
			system_interface->SetManualTime(time);
			context->Update();
		}
	};
	t->SetPseudoClass("hover", true);
	context->Update();
	AdvanceTo(0.5);
	const float width = after->GetComputedValues().width().value;
	CHECK(width > 15.f);
	CHECK(width < 25.f);
	AdvanceTo(1.5);
	CHECK(after->GetComputedValues().width().value == 30.f);

	document->Close();
	TestsShell::ShutdownShell();
}
