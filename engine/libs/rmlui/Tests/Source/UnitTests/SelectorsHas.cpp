#include "../../../Source/Core/ElementStyle.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.5.3): :has() and its invalidation. A change inside the
// subtree of a :has() subject (a class, an attribute, a pseudo-class its argument names, a child added or removed)
// re-matches the subject; changes that no :has() argument depends on (hover) re-match nothing more than before.

static const String has_rml = R"(
<rml>
<head>
	<style>
		div, li, ul { display: block; }
		li { height: 10px; }
		li:has(> input:checked) { height: 20px; }
		.card { height: 5px; }
		.card:has(.badge) { height: 15px; }
		.item { height: 1px; }
		.item:has(+ .item.sel) { height: 2px; }
		.wrap:has(> .x) > .target { height: 30px; }
		.target { height: 3px; }
	</style>
</head>
<body>
	<ul id="list"><li id="li1"><input id="cb1" type="checkbox"/></li><li id="li2"><input id="cb2" type="checkbox"/></li></ul>
	<div id="card" class="card"><div id="inner"><span id="flag"/></div></div>
	<div id="i1" class="item"/><div id="i2" class="item"/><div id="i3" class="item"/>
	<div id="wrap" class="wrap"><div id="w1"/><div id="t" class="target"/></div>
</body>
</rml>
)";

TEST_CASE("SelectorsHas")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(has_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto height = [&](const char* id) { return document->GetElementById(id)->GetComputedValues().height().value; };

	SUBCASE("checked child")
	{
		CHECK(height("li1") == 10.f);
		rmlui_dynamic_cast<ElementFormControlInput*>(document->GetElementById("cb1"))->SetAttribute("checked", "");
		context->Update();
		CHECK(height("li1") == 20.f);
		CHECK(height("li2") == 10.f);
		document->GetElementById("cb1")->RemoveAttribute("checked");
		context->Update();
		CHECK(height("li1") == 10.f);
	}

	SUBCASE("descendant class")
	{
		CHECK(height("card") == 5.f);
		document->GetElementById("flag")->SetClass("badge", true);
		context->Update();
		CHECK(height("card") == 15.f);
		document->GetElementById("flag")->SetClass("badge", false);
		context->Update();
		CHECK(height("card") == 5.f);
	}

	SUBCASE("descendant added and removed")
	{
		Element* inner = document->GetElementById("inner");
		ElementPtr badge = document->CreateElement("span");
		badge->SetClass("badge", true);
		Element* added = inner->AppendChild(std::move(badge));
		context->Update();
		CHECK(height("card") == 15.f);
		inner->RemoveChild(added);
		context->Update();
		CHECK(height("card") == 5.f);
	}

	SUBCASE("next sibling")
	{
		document->GetElementById("i3")->SetClass("sel", true);
		context->Update();
		CHECK(height("i1") == 1.f);
		CHECK(height("i2") == 2.f);
		CHECK(height("i3") == 1.f);
	}

	SUBCASE("in a compound that is not the subject")
	{
		CHECK(height("t") == 3.f);
		document->GetElementById("w1")->SetClass("x", true);
		context->Update();
		CHECK(height("t") == 30.f);
		document->GetElementById("w1")->SetClass("x", false);
		context->Update();
		CHECK(height("t") == 3.f);
	}

	SUBCASE("hover changes do not re-match")
	{
		const int before = ElementStyle::GetNumDefinitionUpdates();
		document->GetElementById("flag")->SetPseudoClass("hover", true);
		context->Update();
		// the element itself and its siblings, as without :has()
		CHECK(ElementStyle::GetNumDefinitionUpdates() - before <= 2);
	}

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("SelectorsHas.Invalidation")
{
	// 2000 elements: 400 list items of a checkbox and three spans. Toggling one checkbox re-matches its ancestors (the
	// :has() subjects) and the checkbox, not the document.
	String body = "<ul id='list'>";
	for (int i = 0; i < 400; i++)
		body += "<li><input type='checkbox' id='cb" + ToString(i) + "'/><span/><span/><span/></li>";
	body += "</ul>";
	const String rml = "<rml><head><style>li { display: block; height: 4px; } li:has(> input:checked) { height: 8px; } span { display: inline; }"
					   "</style></head><body>" +
		body + "</body></rml>";

	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* checkbox = document->GetElementById("cb200");
	REQUIRE(checkbox);
	const int before = ElementStyle::GetNumDefinitionUpdates();
	checkbox->SetAttribute("checked", "");
	context->Update();
	const int rematched = ElementStyle::GetNumDefinitionUpdates() - before;
	CHECK(checkbox->GetParentNode()->GetComputedValues().height().value == 8.f);
	CHECK(rematched < 50);
	MESSAGE("elements re-matched after toggling one checkbox: ", rematched);

	document->Close();
	TestsShell::ShutdownShell();
}
