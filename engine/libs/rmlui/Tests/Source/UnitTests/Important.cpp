#include "../../../Source/Core/ElementStyle.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheet.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.4.1): '!important' in style sheets and inline styles, with the
// CSS precedence: normal stylesheet < normal inline < important stylesheet < important inline, transitions above all of
// them and animations between normal and important declarations.

static const String important_rml = R"(
<rml>
<head>
	<style>
		div { display: block; height: 10px; }
		#a.x { width: 30px; }
		#a { width: 20px !important; }
		.low { width: 5px !important; }
		#b { width: 40px !important; }
		#c { margin: 3px 4px !important; }
		#c.y { margin: 9px; }
		#d { --w: 12px; width: var(--w) !important; }
		#e { color: #00f !important; }
		#f { padding: var(--p) !important; --p: 6px; }
		#h { width: 21px !important; width: 22px; height: 3px !important; height: 4px !important; }
		@keyframes grow { from { width: 1px !important; height: 1px; } to { width: 2px; height: 2px; } }
	</style>
</head>
<body>
	<div id="a" class="x low"/>
	<div id="b" style="width: 10px"/>
	<div id="c" class="y" style="margin: 1px"/>
	<div id="d" style="width: 15px"/>
	<div id="e" style="color: #0f0 !important"/>
	<div id="f" style="padding: 2px"/>
	<div id="g" style="width: 8px !important; height: 9px"/>
	<div id="h"/>
</body>
</rml>
)";

TEST_CASE("Important")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(important_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto width = [&](const char* id) { return document->GetElementById(id)->GetComputedValues().width().value; };

	SUBCASE("stylesheet versus stylesheet")
	{
		// '#a.x' is more specific, '#a !important' wins; of two important declarations the more specific wins.
		CHECK(width("a") == 20.f);
	}

	SUBCASE("important stylesheet beats a normal inline style")
	{
		CHECK(width("b") == 40.f);
		// a later inline change does not change it either
		document->GetElementById("b")->SetProperty("width", "11px");
		context->Update();
		CHECK(width("b") == 40.f);
	}

	SUBCASE("important inline beats important stylesheet")
	{
		Element* b = document->GetElementById("b");
		b->SetProperty("width", "12px !important");
		context->Update();
		CHECK(width("b") == 12.f);
		CHECK(b->GetLocalStyleProperties().find(PropertyId::Width)->second.important);
		CHECK(b->GetProperty(PropertyId::Width)->ToString() == "12px !important");
		b->RemoveProperty("width");
		context->Update();
		CHECK(width("b") == 40.f);

		Element* e = document->GetElementById("e");
		CHECK(e->GetComputedValues().color() == Colourb(0, 255, 0));
	}

	SUBCASE("shorthands")
	{
		Element* c = document->GetElementById("c");
		CHECK(c->GetComputedValues().margin_top().value == 3.f);
		CHECK(c->GetComputedValues().margin_right().value == 4.f);
		CHECK(c->GetComputedValues().margin_bottom().value == 3.f);
		CHECK(c->GetComputedValues().margin_left().value == 4.f);
	}

	SUBCASE("var() and important")
	{
		CHECK(width("d") == 12.f);
		Element* f = document->GetElementById("f");
		CHECK(f->GetComputedValues().padding_top().value == 6.f);
		CHECK(f->GetComputedValues().padding_left().value == 6.f);
	}

	SUBCASE("in one block a later normal declaration does not override an important one")
	{
		CHECK(width("h") == 21.f);
		CHECK(document->GetElementById("h")->GetComputedValues().height().value == 4.f);
	}

	SUBCASE("inline important and normal in one attribute")
	{
		Element* g = document->GetElementById("g");
		CHECK(width("g") == 8.f);
		CHECK(g->GetComputedValues().height().value == 9.f);
		CHECK(g->GetLocalStyleProperties().find(PropertyId::Width)->second.important);
		CHECK_FALSE(g->GetLocalStyleProperties().find(PropertyId::Height)->second.important);
	}

	SUBCASE("animation layers")
	{
		Element* b = document->GetElementById("b");
		ElementStyle* style = b->GetStyle();
		style->SetAnimatedProperty(PropertyId::Width, Property(50.f, Unit::PX));
		context->Update();
		// an important declaration overrides animations
		CHECK(width("b") == 40.f);
		style->SetAnimatedProperty(PropertyId::Height, Property(50.f, Unit::PX));
		context->Update();
		// a normal one does not
		CHECK(b->GetComputedValues().height().value == 50.f);
		// transitions override important declarations
		style->SetTransitionProperty(PropertyId::Width, Property(60.f, Unit::PX));
		context->Update();
		CHECK(width("b") == 60.f);
		style->RemoveTransitionProperty(PropertyId::Width);
		style->RemoveAnimatedProperty(PropertyId::Width);
		style->RemoveAnimatedProperty(PropertyId::Height);
		context->Update();
		CHECK(width("b") == 40.f);
	}

	SUBCASE("keyframes ignore important declarations")
	{
		const Keyframes* keyframes = document->GetStyleSheet()->GetKeyframes("grow");
		REQUIRE(keyframes);
		REQUIRE(keyframes->blocks.size() == 2);
		CHECK(keyframes->blocks[0].properties.GetProperty(PropertyId::Width) == nullptr);
		CHECK(keyframes->blocks[0].properties.GetProperty(PropertyId::Height) != nullptr);
	}

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("Important.Parsing")
{
	TestsShell::GetContext();

	auto parse = [](const String& name, const String& value, PropertyDictionary& dictionary) {
		dictionary.Clear();
		return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
	};

	PropertyDictionary dictionary;
	CHECK(parse("width", "10px !important", dictionary));
	REQUIRE(dictionary.GetProperty(PropertyId::Width));
	CHECK(dictionary.GetProperty(PropertyId::Width)->important);

	CHECK(parse("width", "10px!IMPORTANT", dictionary));
	CHECK(dictionary.GetProperty(PropertyId::Width)->important);
	CHECK(parse("width", "10px ! important", dictionary));
	CHECK(dictionary.GetProperty(PropertyId::Width)->important);

	CHECK(parse("width", "10px", dictionary));
	CHECK_FALSE(dictionary.GetProperty(PropertyId::Width)->important);

	// a shorthand marks every longhand
	CHECK(parse("padding", "1px 2px !important", dictionary));
	CHECK(dictionary.GetNumProperties() == 4);
	for (const auto& entry : dictionary.GetProperties())
		CHECK(entry.second.important);

	// custom properties keep the value without the suffix
	CHECK(parse("--x", "a b !important", dictionary));
	REQUIRE(dictionary.GetCustomProperty("--x"));
	CHECK(dictionary.GetCustomProperty("--x")->Get<String>() == "a b");
	CHECK(dictionary.GetCustomProperty("--x")->important);

	// var() shorthands
	CHECK(parse("margin", "var(--m) !important", dictionary));
	REQUIRE(dictionary.GetVarShorthand(ShorthandId::Margin));
	CHECK(dictionary.GetVarShorthand(ShorthandId::Margin)->important);
	CHECK(dictionary.GetProperty(PropertyId::MarginTop)->important);

	// invalid forms
	CHECK_FALSE(parse("width", "!important", dictionary));
	CHECK_FALSE(parse("width", "10px !important !important", dictionary));
	CHECK_FALSE(parse("width", "10px !importantx", dictionary));
	CHECK_FALSE(parse("width", "10px !imp", dictionary));
	CHECK_FALSE(parse("width", "red !important", dictionary));
	// '!' inside a string is not the importance marker
	CHECK(parse("--s", "\"a !important\"", dictionary));
	CHECK_FALSE(dictionary.GetCustomProperty("--s")->important);

	TestsShell::ShutdownShell();
}
