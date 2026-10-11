#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.4.4): the 'style' attribute replaces its declarations as in
// CSS; properties set through Element::SetProperty keep their per-property semantics.

static const String inline_style_rml = R"RML(
<rml>
<head>
	<style>
		div { display: block; width: 50px; height: 10px; }
	</style>
</head>
<body><div id="e" style="width: 10px; height: 20px; --x: 4px; margin: var(--x)"/></body>
</rml>
)RML";

TEST_CASE("InlineStyle")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(inline_style_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* e = document->GetElementById("e");
	const Style::ComputedValues& values = e->GetComputedValues();
	CHECK(values.width().value == 10.f);
	CHECK(values.height().value == 20.f);
	CHECK(values.margin_top().value == 4.f);

	SUBCASE("a shorter attribute removes the declarations it lost")
	{
		e->SetAttribute("style", "height: 30px");
		context->Update();
		CHECK(values.width().value == 50.f);
		CHECK(values.height().value == 30.f);
		CHECK(values.margin_top().value == 0.f);
		CHECK(e->GetLocalStyleProperties().size() == 1);
	}

	SUBCASE("removing the attribute removes its declarations")
	{
		e->RemoveAttribute("style");
		context->Update();
		CHECK(values.width().value == 50.f);
		CHECK(values.height().value == 10.f);
		CHECK(e->GetLocalStyleProperties().empty());
	}

	SUBCASE("SetProperty is kept")
	{
		e->SetProperty("padding-left", "3px");
		e->SetAttribute("style", "width: 12px");
		context->Update();
		CHECK(values.padding_left().value == 3.f);
		CHECK(values.width().value == 12.f);
		CHECK(values.height().value == 10.f);
	}

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("InlineStyle.EmptyInlineOnZeroHeightLine")
{
	// CSS 2.1 §9.4.2: a line with nothing but an empty inline box is a zero-height line box; Chromium gives the span no
	// height there, at the line's top. Next to text it keeps its content area.
	const String rml = R"RML(
<rml><head><style>
	body { font-family: LatoLatin; font-size: 16px; line-height: 20px; }
	div { display: block; width: 100px; }
	#a { height: 30px; }
</style></head>
<body><div id="a"><span id="empty"/></div><div id="b">x<span id="next"/></div></body></rml>
)RML";
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	Element* empty = document->GetElementById("empty");
	CHECK(empty->GetBox().GetSize(BoxArea::Border).y == 0.f);
	CHECK(empty->GetAbsoluteTop() == doctest::Approx(document->GetElementById("a")->GetAbsoluteTop()));
	CHECK(document->GetElementById("next")->GetBox().GetSize(BoxArea::Border).y > 10.f);
	document->Close();
	TestsShell::ShutdownShell();
}
