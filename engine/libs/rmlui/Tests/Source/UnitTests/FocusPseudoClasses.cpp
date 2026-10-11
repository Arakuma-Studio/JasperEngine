#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.5.4): ':focus' on the focused element only (6.3 set it on
// the whole chain), ':focus-within' on it and its ancestors, ':focus-visible' when the focus came from the keyboard;
// disabled form controls take no focus (without an inline 'focus: none').

static const String focus_rml = R"(
<rml>
<head>
	<style>
		div, form { display: block; }
		input { display: inline-block; width: 50px; height: 20px; }
		#outer:focus { width: 1px; }
		#outer:focus-within { height: 7px; }
		#a:focus { width: 99px; }
		#a:focus-visible { height: 33px; }
	</style>
</head>
<body>
	<div id="outer"><form id="form"><input id="a" type="checkbox"/><input id="b" type="checkbox"/></form></div>
	<input id="c" type="checkbox"/>
	<input id="d" type="checkbox" disabled=""/>
	<input id="t" type="text"/>
</body>
</rml>
)";

TEST_CASE("FocusPseudoClasses")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(focus_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* outer = document->GetElementById("outer");
	Element* form = document->GetElementById("form");
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	Element* c = document->GetElementById("c");

	SUBCASE("pointer focus")
	{
		REQUIRE(a->Focus());
		context->Update();
		CHECK(a->IsPseudoClassSet("focus"));
		CHECK_FALSE(a->IsPseudoClassSet("focus-visible"));
		CHECK_FALSE(form->IsPseudoClassSet("focus"));
		CHECK_FALSE(outer->IsPseudoClassSet("focus"));
		CHECK(a->IsPseudoClassSet("focus-within"));
		CHECK(form->IsPseudoClassSet("focus-within"));
		CHECK(outer->IsPseudoClassSet("focus-within"));
		CHECK(document->IsPseudoClassSet("focus-within"));
		CHECK(a->GetComputedValues().width().value == 99.f);
		CHECK(a->GetComputedValues().height().value == 20.f);
		CHECK(outer->GetComputedValues().height().value == 7.f);
		CHECK(outer->GetComputedValues().width().type == Style::Width::Auto);

		// moving the focus inside the chain keeps :focus-within on the shared ancestors
		REQUIRE(b->Focus());
		context->Update();
		CHECK_FALSE(a->IsPseudoClassSet("focus"));
		CHECK_FALSE(a->IsPseudoClassSet("focus-within"));
		CHECK(b->IsPseudoClassSet("focus"));
		CHECK(outer->IsPseudoClassSet("focus-within"));

		// and leaving the chain removes it
		REQUIRE(c->Focus());
		context->Update();
		CHECK_FALSE(outer->IsPseudoClassSet("focus-within"));
		CHECK(outer->GetComputedValues().height().type == Style::Height::Auto);
		CHECK(document->IsPseudoClassSet("focus-within"));
	}

	SUBCASE("keyboard focus is visible")
	{
		REQUIRE(a->Focus(true));
		context->Update();
		CHECK(a->IsPseudoClassSet("focus-visible"));
		CHECK_FALSE(form->IsPseudoClassSet("focus-visible"));
		CHECK(a->GetComputedValues().height().value == 33.f);

		// focusing the same element with the pointer clears it
		REQUIRE(a->Focus(false));
		context->Update();
		CHECK_FALSE(a->IsPseudoClassSet("focus-visible"));

		// tab navigation
		context->ProcessKeyDown(Input::KI_TAB, 0);
		context->Update();
		CHECK(b->IsPseudoClassSet("focus"));
		CHECK(b->IsPseudoClassSet("focus-visible"));
		CHECK_FALSE(a->IsPseudoClassSet("focus-visible"));
	}

	SUBCASE("text fields are focus-visible from the pointer too")
	{
		Element* t = document->GetElementById("t");
		REQUIRE(t->Focus());
		context->Update();
		CHECK(t->IsPseudoClassSet("focus"));
		CHECK(t->IsPseudoClassSet("focus-visible"));
		REQUIRE(c->Focus());
		CHECK_FALSE(t->IsPseudoClassSet("focus-visible"));
	}

	SUBCASE("disabled controls take no focus")
	{
		Element* d = document->GetElementById("d");
		CHECK_FALSE(d->Focus());
		CHECK(d->GetLocalProperty(PropertyId::Focus) == nullptr);
		d->RemoveAttribute("disabled");
		context->Update();
		CHECK(d->Focus());
		// disabling the focused control moves the focus away
		d->SetAttribute("disabled", "");
		context->Update();
		CHECK(context->GetFocusElement() != d);
		CHECK_FALSE(d->IsPseudoClassSet("focus"));
		// tab navigation skips it
		REQUIRE(c->Focus(true));
		context->ProcessKeyDown(Input::KI_TAB, 0);
		context->Update();
		CHECK(context->GetFocusElement() == document->GetElementById("t"));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
