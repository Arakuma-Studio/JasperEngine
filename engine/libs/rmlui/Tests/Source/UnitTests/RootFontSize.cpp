#include "../../../Source/Core/ComputeProperty.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.1): the root element's '%' and 'em' font sizes resolve
// against the initial font size, as its 'rem' already did, instead of becoming 0 without a parent. 'rem' resolves
// against the document element's font size, so the usual pattern html { font-size: 62.5% } body { font-size: 1.6rem }
// (the HTML <html> is RmlUi's document element under the compatibility layer's mapping) gives the initial size back.

static const String root_font_size_rml = R"(
<rml>
<head>
	<style>
		body { font-family: LatoLatin; font-size: %s; }
		div { font-size: %s; }
	</style>
</head>
<body><div id="child"/><p id="deep"><span id="rem">x</span></p></body>
</rml>
)";

static float InitialFontSize()
{
	return DefaultComputedValues().font_size();
}

static ElementDocument* LoadFontSizes(Context* context, const char* body_size, const char* div_size)
{
	ElementDocument* document = context->LoadDocumentFromMemory(CreateString(root_font_size_rml.c_str(), body_size, div_size));
	REQUIRE(document);
	document->Show();
	context->Update();
	return document;
}

TEST_CASE("RootFontSize.percent_and_em_resolve_against_the_initial_size")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const float initial = InitialFontSize();
	REQUIRE(initial > 0.f);

	struct Case {
		const char* body;
		float expected_factor;
	};
	const Case cases[] = {
		{"62.5%", 0.625f},
		{"150%", 1.5f},
		{"2em", 2.f},
		{"0.5em", 0.5f},
		{"2rem", 2.f},
#ifdef RMLUI_MATH_EXPRESSIONS
		{"calc(50% + 0.5em)", 1.f},
		{"max(10%, 0.75em)", 0.75f},
#endif
	};

	for (const Case& c : cases)
	{
		const String body = c.body;
		INFO("body { font-size: ", body, " }");
		ElementDocument* document = LoadFontSizes(context, c.body, "1em");
		CHECK(document->GetComputedValues().font_size() == doctest::Approx(initial * c.expected_factor));
		CHECK(document->GetElementById("child")->GetComputedValues().font_size() == doctest::Approx(initial * c.expected_factor));
		document->Close();
		context->Update();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("RootFontSize.rem_is_relative_to_the_document_element")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const float initial = InitialFontSize();

	// html { font-size: 62.5% } body { font-size: 1.6rem }: 1.6 * 0.625 = the initial size
	ElementDocument* document = LoadFontSizes(context, "62.5%", "1.6rem");
	CHECK(document->GetComputedValues().font_size() == doctest::Approx(initial * 0.625f));
	Element* child = document->GetElementById("child");
	CHECK(child->GetComputedValues().font_size() == doctest::Approx(initial));

	// rem deep in the tree, not through its parent's em
	Element* deep = document->GetElementById("deep");
	deep->SetProperty(PropertyId::FontSize, Property(3.f, Unit::EM));
	Element* rem = document->GetElementById("rem");
	rem->SetProperty(PropertyId::FontSize, Property(2.f, Unit::REM));
	context->Update();
	CHECK(deep->GetComputedValues().font_size() == doctest::Approx(initial * 0.625f * 3.f));
	CHECK(rem->GetComputedValues().font_size() == doctest::Approx(initial * 0.625f * 2.f));

	// a change of the document's font size reaches every rem
	document->SetProperty(PropertyId::FontSize, Property(20.f, Unit::PX));
	context->Update();
	CHECK(child->GetComputedValues().font_size() == doctest::Approx(32.f));
	CHECK(rem->GetComputedValues().font_size() == doctest::Approx(40.f));

	// an inline percentage on the document element (style attribute, script) resolves like the sheet's
	document->SetProperty("font-size", "50%");
	context->Update();
	CHECK(document->GetComputedValues().font_size() == doctest::Approx(initial * 0.5f));
	CHECK(child->GetComputedValues().font_size() == doctest::Approx(initial * 0.5f * 1.6f));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("RootFontSize.element_without_parent")
{
	// The context's root element has no parent: its relative font sizes take the initial size too (6.3 gave 0).
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const float initial = InitialFontSize();

	Element* root = context->GetRootElement();
	REQUIRE(root);
	REQUIRE(root->GetParentNode() == nullptr);

	root->SetProperty(PropertyId::FontSize, Property(50.f, Unit::PERCENT));
	context->Update();
	CHECK(root->GetComputedValues().font_size() == doctest::Approx(initial * 0.5f));

	root->SetProperty(PropertyId::FontSize, Property(1.5f, Unit::EM));
	context->Update();
	CHECK(root->GetComputedValues().font_size() == doctest::Approx(initial * 1.5f));

	root->RemoveProperty(PropertyId::FontSize);
	context->Update();
	CHECK(root->GetComputedValues().font_size() == doctest::Approx(initial));

	TestsShell::ShutdownShell();
}
