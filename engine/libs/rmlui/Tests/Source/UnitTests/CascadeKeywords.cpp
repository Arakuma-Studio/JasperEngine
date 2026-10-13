#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.4.2, 3.4.3): the CSS-wide keywords inherit, initial, unset,
// revert and revert-layer for every property, custom properties included; the 'all' shorthand; 'revert' to the
// declarations of a style sheet linked with origin="user-agent".

static const String keywords_rml = R"(
<rml>
<head>
	<style>
		body { font-size: 20px; color: #f00; }
		div { display: block; }
		#parent { width: 200px; padding-left: 10px; font-size: 10px; color: #0f0; background-color: #00f; height: 50px; --v: 7px; }
		#parent > div { height: 5px; }
		#c-inherit { width: inherit; padding-left: inherit; background-color: inherit; color: inherit; }
		#c-initial { color: initial; font-size: initial; width: initial; padding-left: initial; }
		#c-unset { color: unset; background-color: unset; padding-left: unset; }
		#c-em { width: 2em; }
		#c-em > div { width: inherit; font-size: 30px; }
		#c-percent { width: 50%; }
		#c-percent > div { width: inherit; }
		#c-var { --v: initial; width: var(--v, 33px); }
		#c-var-inherit { --v: inherit; width: var(--v, 33px); }
		#c-chain > div { padding-left: inherit; }
		#c-chain { padding-left: inherit; }
		#c-all { all: initial; }
		#c-all-unset { all: unset; }
		.radius { border-radius: 7px; tab-index: auto; nav-down: auto; -rmlui-direction: rtl; }
		#c-all-radius { all: unset; }
		#c-shorthand { padding: inherit; }
		#c-case { color: INHERIT; }
	</style>
</head>
<body>
	<div id="parent">
		<div id="c-inherit"/>
		<div id="c-initial"/>
		<div id="c-unset"/>
		<div id="c-em"><div id="c-em-child"/></div>
		<div id="c-percent"><div id="c-percent-child"/></div>
		<div id="c-var"/>
		<div id="c-var-inherit"/>
		<div id="c-chain"><div id="c-chain-child"/></div>
		<div id="c-all"/>
		<div id="c-all-unset"/>
		<div id="c-all-radius" class="radius"/>
		<div id="c-shorthand"/>
		<div id="c-case"/>
	</div>
</body>
</rml>
)";

TEST_CASE("CascadeKeywords")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(keywords_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto values = [&](const char* id) -> const Style::ComputedValues& { return document->GetElementById(id)->GetComputedValues(); };
	const Colourb green(0, 255, 0), red(255, 0, 0), blue(0, 0, 255), black(0, 0, 0), transparent(0, 0, 0, 0);

	SUBCASE("inherit")
	{
		CHECK(values("c-inherit").width().value == 200.f);
		CHECK(values("c-inherit").padding_left().value == 10.f);
		CHECK(values("c-inherit").background_color() == blue);
		CHECK(values("c-inherit").color() == green);
		CHECK(document->GetElementById("c-inherit")->GetProperty(PropertyId::Width)->ToString() == "200px");
		CHECK(document->GetElementById("c-inherit")->GetLocalProperty(PropertyId::Width)->ToString() == "inherit");
		CHECK(document->GetElementById("c-inherit")->GetBox().GetSize().x == 200.f);
	}

	SUBCASE("initial")
	{
		CHECK(values("c-initial").color() == black);
		CHECK(values("c-initial").font_size() == 16.f);
		CHECK(values("c-initial").width().type == Style::Width::Auto);
		CHECK(values("c-initial").padding_left().value == 0.f);
	}

	SUBCASE("unset")
	{
		// inherited: the parent's; not inherited: the initial value
		CHECK(values("c-unset").color() == green);
		CHECK(values("c-unset").background_color() == transparent);
		CHECK(values("c-unset").padding_left().value == 0.f);
	}

	SUBCASE("inherit takes the computed value")
	{
		// 2em of the parent's 10px, not of the child's 30px
		CHECK(values("c-em").width().value == 20.f);
		CHECK(values("c-em-child").width().value == 20.f);
		// a percentage is inherited as a percentage
		CHECK(values("c-percent-child").width().type == Style::Width::Percentage);
		CHECK(values("c-percent-child").width().value == 50.f);
	}

	SUBCASE("custom properties")
	{
		// initial: the guaranteed-invalid value, so the fallback
		CHECK(values("c-var").width().value == 33.f);
		CHECK(values("c-var-inherit").width().value == 7.f);
	}

	SUBCASE("inherit through a chain and dirtied by the parent")
	{
		CHECK(values("c-chain").padding_left().value == 10.f);
		CHECK(values("c-chain-child").padding_left().value == 10.f);
		document->GetElementById("parent")->SetProperty("padding-left", "13px");
		context->Update();
		CHECK(values("c-chain").padding_left().value == 13.f);
		CHECK(values("c-chain-child").padding_left().value == 13.f);
		CHECK(values("c-inherit").padding_left().value == 13.f);
		document->GetElementById("parent")->SetProperty("width", "150px");
		context->Update();
		CHECK(values("c-inherit").width().value == 150.f);
	}

	SUBCASE("all")
	{
		CHECK(values("c-all").display() == Style::Display::Inline);
		CHECK(values("c-all").color() == black);
		CHECK(values("c-all").font_size() == 16.f);
		CHECK(values("c-all-unset").display() == Style::Display::Inline);
		CHECK(values("c-all-unset").color() == green);
		CHECK(values("c-all-unset").font_size() == 10.f);

		// every CSS property, the radii's -rmlui- longhands included; not direction nor RmlUi's interaction properties
		// (focusability, navigation: attributes in HTML)
		CHECK_FALSE(values("c-all-radius").has_border_radius());
		CHECK(values("c-all-radius").tab_index() == Style::TabIndex::Auto);
		CHECK(document->GetElementById("c-all-radius")->GetProperty(PropertyId::NavDown)->ToString() == "auto");
		CHECK(values("c-all-radius").direction() == Style::Direction::Rtl);
	}

	SUBCASE("shorthands and case")
	{
		CHECK(values("c-shorthand").padding_left().value == 10.f);
		CHECK(values("c-shorthand").padding_top().value == 0.f);
		CHECK(values("c-case").color() == green);
	}

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("CascadeKeywords.Parsing")
{
	TestsShell::GetContext();

	PropertyDictionary dictionary;
	auto parse = [&](const String& name, const String& value) {
		dictionary.Clear();
		return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
	};

	for (const char* keyword : {"inherit", "initial", "unset", "revert", "revert-layer"})
	{
		CHECK(parse("color", keyword));
		REQUIRE(dictionary.GetProperty(PropertyId::Color));
		CHECK(dictionary.GetProperty(PropertyId::Color)->unit == Unit::CSS_WIDE_KEYWORD);
	}
	CHECK(dictionary.GetProperty(PropertyId::Color)->ToString() == "revert");

	CHECK(parse("margin", "unset"));
	CHECK(dictionary.GetNumProperties() == 4);
	CHECK(parse("--x", "initial"));
	CHECK(dictionary.GetCustomProperty("--x")->unit == Unit::CSS_WIDE_KEYWORD);
	CHECK(parse("width", "inherit !important"));
	CHECK(dictionary.GetProperty(PropertyId::Width)->important);
	CHECK(dictionary.GetProperty(PropertyId::Width)->ToString() == "inherit !important");

	CHECK(parse("all", "initial"));
	CHECK(dictionary.GetProperty(PropertyId::Display));
	CHECK(dictionary.GetProperty(PropertyId::RmlUi_Direction) == nullptr);
	CHECK_FALSE(parse("all", "red"));
	CHECK_FALSE(parse("margin", "1px inherit"));

	TestsShell::ShutdownShell();
}

static const String revert_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/../Tests/Data/UnitTests/revert-ua.rcss" origin="user-agent"/>
	<style>
		ul { padding-left: 0; margin-top: 0; --ua: 9px; }
		#inner { padding-left: revert; margin-top: revert-layer; }
		#var-revert { --ua: revert; width: var(--ua); }
		p { margin-top: 0; }
		#p { margin-top: revert; }
		div { display: block; }
		#none { width: 30px; color: #0f0; }
		#none-revert { width: revert; color: revert; }
	</style>
</head>
<body>
	<ul id="outer"><ul id="inner"/></ul>
	<ul id="var-revert"/>
	<p id="p"/>
	<div id="none"><div id="none-revert"/></div>
</body>
</rml>
)";

TEST_CASE("CascadeKeywords.RevertToUserAgent")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(revert_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto values = [&](const char* id) -> const Style::ComputedValues& { return document->GetElementById(id)->GetComputedValues(); };

	CHECK(values("outer").padding_left().value == 0.f);
	CHECK(values("inner").padding_left().value == 40.f);
	CHECK(values("inner").margin_top().value == 16.f);
	CHECK(values("var-revert").width().value == 3.f);
	CHECK(values("p").margin_top().value == 10.f);
	// no user-agent declaration: as 'unset'
	CHECK(values("none-revert").width().type == Style::Width::Auto);
	CHECK(values("none-revert").color() == Colourb(0, 255, 0));
	CHECK(document->GetElementById("inner")->GetProperty(PropertyId::PaddingLeft)->ToString() == "40px");

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("CascadeKeywords.SetOrigin")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(R"(<rml><head><style>
		div { display: block; height: 0; }
		#a { margin-left: 5px; }
		#b { margin-left: revert; }
	</style></head><body><div id="a"/><div id="b"/></body></rml>)");
	REQUIRE(document);

	SharedPtr<StyleSheetContainer> user_agent = Factory::InstanceStyleSheetString("div { margin-left: 8px; }");
	REQUIRE(bool(user_agent));
	user_agent->SetOrigin(StyleOrigin::UserAgent);
	document->SetStyleSheetContainer(user_agent->CombineStyleSheetContainer(*document->GetStyleSheetContainer()));
	document->Show();
	context->Update();

	CHECK(document->GetElementById("a")->GetComputedValues().margin_left().value == 5.f);
	CHECK(document->GetElementById("b")->GetComputedValues().margin_left().value == 8.f);

	document->Close();
	TestsShell::ShutdownShell();
}

// The keywords with p3-values' value types: 'currentcolor' (and color-mix() of it) stays a keyword through 'inherit',
// so it is resolved against the element's own color; logical properties take keywords and importance as their
// physical longhands do.
TEST_CASE("CascadeKeywords.ValueTypes")
{
	const String rml = R"RML(
<rml>
<head>
	<style>
		body { font-family: LatoLatin; font-size: 16px; }
		div { display: block; }
		#p { color: #ff0000; border-top-color: currentcolor; border-right-color: color-mix(in srgb, currentcolor 50%, #0000ff);
			text-decoration-color: currentcolor; margin-left: 7px; padding-top: 3px; padding-bottom: 4px; }
		#c1 { color: #0000ff; border-top-color: inherit; border-right-color: inherit; }
		#c2 { color: #00ff00; border-top-color: #123456; }
		#c2 { border-top-color: initial; }
		#c3 { color: #0000ff; text-decoration-color: inherit; }
		#c4 { color: #0000ff; border-top-color: unset; border-top-color: revert; }
		#l1 { margin-inline-start: inherit; padding-block: inherit; }
		#l2 { margin-inline: 5px; margin-inline: initial; padding-block-end: unset; }
		#l3 { margin-inline-start: 9px !important; margin-left: 1px; }
		#l4 { margin-left: 2px !important; margin-inline-start: 11px; }
	</style>
</head>
<body>
	<div id="p"><div id="c1"/><div id="c2"/><div id="c3"/><div id="c4"/><div id="l1"/><div id="l2"/><div id="l3"/><div id="l4"/></div>
</body>
</rml>
)RML";

	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	auto ById = [&](const char* id) { return document->GetElementById(id); };

	// currentcolor inherited as a keyword: the child's own color.
	CHECK(ById("p")->GetComputedValues().border_top_color() == Colourb(255, 0, 0));
	CHECK(ById("c1")->GetComputedValues().border_top_color() == Colourb(0, 0, 255));
	// color-mix(currentcolor) too: half the child's blue, half blue.
	CHECK(ById("c1")->GetComputedValues().border_right_color() == Colourb(0, 0, 255));
	CHECK(ById("p")->GetComputedValues().border_right_color() == Colourb(128, 0, 128));
	// initial and unset (the initial value is currentcolor), revert without a user-agent rule.
	CHECK(ById("c2")->GetComputedValues().border_top_color() == Colourb(0, 255, 0));
	CHECK(ById("c4")->GetComputedValues().border_top_color() == Colourb(0, 0, 255));
	// An inherited property set to currentcolor: 'inherit' gives the keyword, resolved with the child's color.
	const Property* decoration = ById("c3")->GetProperty(PropertyId::TextDecorationColor);
	REQUIRE(decoration);
	CHECK(decoration->ToString() == "currentcolor");

	// A color change of the child follows.
	ById("c1")->SetProperty("color", "#00ff00");
	context->Update();
	CHECK(ById("c1")->GetComputedValues().border_top_color() == Colourb(0, 255, 0));

	// Logical properties.
	CHECK(ById("l1")->GetComputedValues().margin_left().value == 7.f);
	CHECK(ById("l1")->GetComputedValues().padding_top().value == 3.f);
	CHECK(ById("l1")->GetComputedValues().padding_bottom().value == 4.f);
	CHECK(ById("l2")->GetComputedValues().margin_left().value == 0.f);
	CHECK(ById("l2")->GetComputedValues().margin_right().value == 0.f);
	CHECK(ById("l2")->GetComputedValues().padding_bottom().value == 0.f);
	CHECK(ById("l3")->GetComputedValues().margin_left().value == 9.f);
	CHECK(ById("l4")->GetComputedValues().margin_left().value == 2.f);

	document->Close();
	TestsShell::ShutdownShell();
}
