#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.7, FORK.md P3-10): the CSS Box Alignment 3 keywords of
// align-* and justify-*, their 'safe'/'unsafe' and multi-word forms, justify-items/justify-self, the place-* shorthands,
// and flex layout of the 'normal' initial values as before.

namespace {

String Parsed(const String& name, const String& value)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value))
		return "<invalid>";
	const Property* property = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(name));
	return property ? property->ToString() : String("<none>");
}

} // namespace

TEST_CASE("Alignment.values")
{
	TestsShell::GetContext();

	SUBCASE("keywords")
	{
		for (const char* value :
			{"flex-start", "flex-end", "center", "space-between", "space-around", "space-evenly", "stretch", "normal", "start", "end"})
			CHECK(Parsed("align-content", value) == value);
		for (const char* value : {"normal", "start", "end", "left", "right", "stretch", "space-evenly"})
			CHECK(Parsed("justify-content", value) == value);
		for (const char* value : {"normal", "stretch", "baseline", "start", "end", "self-start", "self-end", "flex-start", "center"})
			CHECK(Parsed("align-items", value) == value);
		for (const char* value : {"auto", "normal", "start", "self-end"})
			CHECK(Parsed("align-self", value) == value);
		for (const char* value : {"legacy", "normal", "left", "right", "center", "start", "stretch"})
			CHECK(Parsed("justify-items", value) == value);
		for (const char* value : {"auto", "left", "end"})
			CHECK(Parsed("justify-self", value) == value);
		// keywords that do not apply to the property
		CHECK(Parsed("align-content", "left") == "<invalid>");
		CHECK(Parsed("align-items", "space-between") == "<invalid>");
		CHECK(Parsed("justify-self", "legacy") == "<invalid>");
	}

	SUBCASE("multi-word forms")
	{
		CHECK(Parsed("align-items", "first baseline") == "baseline");
		CHECK(Parsed("align-items", "last baseline") == "last baseline");
		CHECK(Parsed("align-self", "last  baseline") == "last baseline");
		CHECK(Parsed("justify-items", "legacy left") == "legacy");
		CHECK(Parsed("justify-items", "center legacy") == "legacy");
		CHECK(Parsed("justify-content", "safe center") == "safe center");
		CHECK(Parsed("align-items", "unsafe end") == "unsafe end");
		CHECK(Parsed("justify-self", "SAFE Right") == "safe right");
		// 'safe' and 'unsafe' only before a positional keyword
		CHECK(Parsed("align-content", "safe space-between") == "<invalid>");
		CHECK(Parsed("align-items", "safe") == "<invalid>");
		CHECK(Parsed("align-self", "unsafe auto") == "<invalid>");
		CHECK(Parsed("align-items", "center end") == "<invalid>");
		CHECK(Parsed("align-items", "first last baseline") == "<invalid>");
	}

	SUBCASE("initial values")
	{
		auto initial = [](PropertyId id) { return StyleSheetSpecification::GetProperty(id)->GetDefaultValue()->ToString(); };
		CHECK(initial(PropertyId::AlignContent) == "normal");
		CHECK(initial(PropertyId::AlignItems) == "normal");
		CHECK(initial(PropertyId::AlignSelf) == "auto");
		CHECK(initial(PropertyId::JustifyContent) == "normal");
		CHECK(initial(PropertyId::JustifyItems) == "legacy");
		CHECK(initial(PropertyId::JustifySelf) == "auto");
	}

	SUBCASE("place-* shorthands")
	{
		PropertyDictionary dictionary;
		auto value = [&](PropertyId id) {
			const Property* property = dictionary.GetProperty(id);
			return property ? property->ToString() : String("<none>");
		};
		REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "place-items", "center"));
		CHECK(value(PropertyId::AlignItems) == "center");
		CHECK(value(PropertyId::JustifyItems) == "center");
		REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "place-items", "safe end last baseline"));
		CHECK(value(PropertyId::AlignItems) == "safe end");
		CHECK(value(PropertyId::JustifyItems) == "last baseline");
		REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "place-self", "auto stretch"));
		CHECK(value(PropertyId::AlignSelf) == "auto");
		CHECK(value(PropertyId::JustifySelf) == "stretch");
		REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "place-content", "space-between end"));
		CHECK(value(PropertyId::AlignContent) == "space-between");
		CHECK(value(PropertyId::JustifyContent) == "end");
		// align-content takes no 'left'; place-content: a single baseline value justifies at 'start'
		PropertyDictionary content;
		CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(content, "place-content", "left"));
		CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(content, "place-items", "center nonsense"));
		CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(content, "place-items", "start end center"));
		CHECK(content.GetNumProperties() == 0);
		REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "place-items", "last baseline"));
		CHECK(value(PropertyId::JustifyItems) == "last baseline");
	}

	TestsShell::ShutdownShell();
}

static const String alignment_rml = R"(
<rml>
<head>
	<style>
		body { width: 400px; }
		div { display: block; }
		.flex { display: flex; width: 100px; height: 50px; }
		.flex > div { width: 10px; }
		.sized > div { height: 10px; }
	</style>
</head>
<body>
	<div class="flex" id="initial"><div id="initial-item"/></div>
	<div class="flex sized" id="end" style="justify-content: end; align-items: safe end"><div id="end-item"/></div>
	<div class="flex sized" id="self" style="align-items: start"><div id="self-item" style="align-self: end"/><div id="start-item"/></div>
	<div class="flex sized" id="right" style="justify-content: right"><div id="right-item"/></div>
</body>
</rml>
)";

TEST_CASE("Alignment.computed_and_flex")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(alignment_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	const ComputedValues& initial = document->GetElementById("initial")->GetComputedValues();
	CHECK(initial.align_items() == Style::AlignItems::Normal);
	CHECK(initial.align_content() == Style::AlignContent::Normal);
	CHECK(initial.justify_content() == Style::JustifyContent::Normal);
	CHECK(initial.justify_items() == Style::JustifyItems::Legacy);
	CHECK(initial.justify_self() == Style::JustifySelf::Auto);
	CHECK(initial.align_items_overflow() == Style::OverflowAlignment::Default);

	const ComputedValues& end = document->GetElementById("end")->GetComputedValues();
	CHECK(end.align_items() == Style::AlignItems::End);
	CHECK(end.align_items_overflow() == Style::OverflowAlignment::Safe);
	CHECK(end.justify_content() == Style::JustifyContent::End);

	auto box = [&](const char* id) {
		Element* element = document->GetElementById(id);
		return Rectanglef::FromPositionSize(element->GetRelativeOffset(BoxArea::Border), element->GetBox().GetSize(BoxArea::Border));
	};
	// 'normal' lays out as 6.3's defaults did: stretched items at the start
	CHECK(box("initial-item").Left() == 0.f);
	CHECK(box("initial-item").Height() == 50.f);
	// start and end (and left and right) of flex items
	CHECK(box("end-item").Left() == 90.f);
	CHECK(box("end-item").Top() == 40.f);
	CHECK(box("self-item").Top() == 40.f);
	CHECK(box("start-item").Top() == 0.f);
	CHECK(box("right-item").Left() == 90.f);

	document->Close();
	TestsShell::ShutdownShell();
}
