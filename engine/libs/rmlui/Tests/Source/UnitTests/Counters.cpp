#include "../../../Source/Core/CounterResolver.h"
#include "../../../Source/Core/ListMarker.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.6.3): CSS counters as CSS Lists 3 defines them, counter()
// and counters() in 'content', quotes, the implicit list-item counter and HTML's <ol start>, <ol reversed>, <li value>.

static String Generated(Element* element, PseudoElementType type = PseudoElementType::Before)
{
	String text;
	Element* pseudo_element = element ? element->GetPseudoElement(type) : nullptr;
	if (!pseudo_element)
		return "<none>";
	for (int i = 0; i < pseudo_element->GetNumChildren(); i++)
	{
		if (auto* text_element = rmlui_dynamic_cast<ElementText*>(pseudo_element->GetChild(i)))
			text += text_element->GetText();
	}
	return text;
}

static const String counters_rml = R"RML(
<rml>
<head>
	<style>
		body { font-family: LatoLatin; }
		div, section, h1, h2, p, ol, ul, li { display: block; }
		#doc { counter-reset: chapter; }
		h1 { counter-increment: chapter; counter-reset: section; }
		h1::before { content: counter(chapter, upper-roman) ". "; }
		h2 { counter-increment: section; }
		h2::before { content: counter(chapter) "." counter(section, lower-alpha) " "; }
		.nest { counter-reset: item; }
		.nest > p { counter-increment: item; }
		.nest > p::before { content: counters(item, ".") ": "; }
		#order { counter-reset: n 5; counter-increment: n 2; counter-set: n 1; }
		#order::before { content: counter(n); }
		#neg { counter-reset: z -4; }
		#neg::before { content: counter(z, decimal-leading-zero) "|" counter(z, lower-roman); }
		#none-counter::before { content: counter(missing) counters(missing, "-"); }
		#hidden { display: none; counter-increment: chapter 100; }
		q::before { content: open-quote; }
		q::after { content: close-quote; }
		q { display: inline; }
		#custom-quotes { quotes: "<" ">" "[" "]"; }
		ol, ul { counter-reset: list-item; }
		li { display: list-item; }
		li::before { content: counter(list-item) "/"; }
		.sib > p, .sib2 > p { counter-reset: s; }
		.sib > span, .sib2 > span { counter-increment: s; display: inline; }
		.sib > span::before, .sib2 > span::before { content: counter(s); }
		.sib2 { counter-reset: s; }
	</style>
</head>
<body id="doc">
	<h1 id="c1"/><h2 id="s1"/><h2 id="s2"/><div id="hidden"/>
	<h1 id="c2"/><h2 id="s3"/>
	<div class="nest"><p id="n1"/><p id="n2"/><div class="nest"><p id="n21"/><p id="n22"/></div><p id="n3"/></div>
	<div id="order"/>
	<div id="neg"/>
	<div id="none-counter"/>
	<p><q id="q1"><q id="q2"/></q></p>
	<p id="custom-quotes"><q id="q3"><q id="q4"/></q></p>
	<ol id="ol1"><li id="a1"/><li id="a2"/><li id="a3"/></ol>
	<ol id="ol2" start="5"><li id="b1"/><li id="b2" value="10"/><li id="b3"/></ol>
	<ol id="ol3" reversed=""><li id="r1"/><li id="r2"/><li id="r3"/></ol>
	<ol id="ol4" reversed="" start="2"><li id="t1"/><li id="t2"/><li id="t3"/></ol>
	<div class="sib"><span id="x1"/><p/><span id="x2"/><span id="x3"/></div>
	<div class="sib2"><span id="y1"/><p/><span id="y2"/><span id="y3"/></div>
</body>
</rml>
)RML";

TEST_CASE("Counters")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(counters_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Update();

	auto ById = [&](const char* id) { return document->GetElementById(id); };

	SUBCASE("reset, increment, styles")
	{
		CHECK(Generated(ById("c1")) == "I. ");
		CHECK(Generated(ById("s1")) == "1.a ");
		CHECK(Generated(ById("s2")) == "1.b ");
		// display: none increments nothing
		CHECK(Generated(ById("c2")) == "II. ");
		CHECK(Generated(ById("s3")) == "2.a ");
	}

	SUBCASE("nested counters()")
	{
		CHECK(Generated(ById("n1")) == "1: ");
		CHECK(Generated(ById("n2")) == "2: ");
		CHECK(Generated(ById("n21")) == "2.1: ");
		CHECK(Generated(ById("n22")) == "2.2: ");
		CHECK(Generated(ById("n3")) == "3: ");
	}

	SUBCASE("order of reset, increment, set; negative values; missing counters")
	{
		CHECK(Generated(ById("order")) == "1");
		CHECK(Generated(ById("neg")) == "-4|-4");
		CHECK(Generated(ById("none-counter")) == "00");
	}

	SUBCASE("quotes")
	{
		CHECK(Generated(ById("q1")) == "\xE2\x80\x9C");
		CHECK(Generated(ById("q2")) == "\xE2\x80\x98");
		CHECK(Generated(ById("q2"), PseudoElementType::After) == "\xE2\x80\x99");
		CHECK(Generated(ById("q1"), PseudoElementType::After) == "\xE2\x80\x9D");
		CHECK(Generated(ById("q3")) == "<");
		CHECK(Generated(ById("q4")) == "[");
		CHECK(Generated(ById("q4"), PseudoElementType::After) == "]");
	}

	SUBCASE("list items")
	{
		CHECK(Generated(ById("a1")) == "1/");
		CHECK(Generated(ById("a3")) == "3/");
		CHECK(Generated(ById("b1")) == "5/");
		CHECK(Generated(ById("b2")) == "10/");
		CHECK(Generated(ById("b3")) == "11/");
		CHECK(Generated(ById("r1")) == "3/");
		CHECK(Generated(ById("r3")) == "1/");
		CHECK(Generated(ById("t1")) == "2/");
		CHECK(Generated(ById("t3")) == "0/");
	}

	SUBCASE("sibling inheritance (CSS Lists 3)")
	{
		// a reset on a sibling reaches the next siblings when the parent has no counter of the name
		CHECK(Generated(ById("x1")) == "1");
		CHECK(Generated(ById("x2")) == "1");
		CHECK(Generated(ById("x3")) == "2");
		// and not when the parent has one: the parent's counter goes on
		CHECK(Generated(ById("y1")) == "1");
		CHECK(Generated(ById("y2")) == "2");
		CHECK(Generated(ById("y3")) == "3");
	}

	SUBCASE("dynamic changes")
	{
		Element* ol = ById("ol1");
		ElementPtr item = document->CreateElement("li");
		ol->InsertBefore(std::move(item), ById("a1"));
		context->Update();
		CHECK(Generated(ById("a1")) == "2/");
		CHECK(Generated(ById("a3")) == "4/");
		ById("ol2")->SetAttribute("start", "20");
		context->Update();
		CHECK(Generated(ById("b1")) == "20/");
		ById("a2")->SetProperty("display", "none");
		context->Update();
		CHECK(Generated(ById("a3")) == "3/");
	}

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("Counters.Format")
{
	CHECK(CounterResolver::FormatCounter(4, "decimal") == "4");
	CHECK(CounterResolver::FormatCounter(4, "decimal-leading-zero") == "04");
	CHECK(CounterResolver::FormatCounter(-4, "decimal-leading-zero") == "-4");
	CHECK(CounterResolver::FormatCounter(28, "lower-alpha") == "ab");
	CHECK(CounterResolver::FormatCounter(28, "upper-latin") == "AB");
	CHECK(CounterResolver::FormatCounter(0, "lower-alpha") == "0");
	CHECK(CounterResolver::FormatCounter(1994, "upper-roman") == "MCMXCIV");
	CHECK(CounterResolver::FormatCounter(4000, "lower-roman") == "4000");
	CHECK(CounterResolver::FormatCounter(2, "lower-greek") == "\xCE\xB2");
	CHECK(CounterResolver::FormatCounter(7, "disc") == "\xE2\x80\xA2");
	CHECK(CounterResolver::FormatCounter(7, "none") == "");
	CHECK(CounterResolver::FormatCounter(7, "\"*") == "*");
	CHECK(CounterResolver::FormatCounter(7, "armenian") == "7");
}
