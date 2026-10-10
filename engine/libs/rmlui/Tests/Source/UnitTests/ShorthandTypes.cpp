#include "../../../Source/Core/PropertyShorthandDefinition.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.4, FORK.md P3-5): the 'AnyOrder' and 'Custom' shorthand
// types the CSS shorthands of phase 3 are built on, the list splitters, and the rejection of trailing values by every
// shorthand type.

namespace {

String ParsedValue(const PropertyDictionary& dictionary, const String& name)
{
	const Property* property = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(name));
	return property ? property->ToString() : String("<none>");
}

bool Parse(PropertyDictionary& dictionary, const String& name, const String& value)
{
	dictionary = PropertyDictionary();
	return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
}

int custom_parser_calls = 0;
String custom_parser_value;

// "test-custom: <color> [/ <length>]?": the color, and the width (initial value when omitted)
bool ParseTestCustom(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	custom_parser_calls++;
	custom_parser_value = value;
	REQUIRE(definition.items.size() == 2);
	REQUIRE(definition.type == ShorthandType::Custom);

	StringList parts;
	StringUtilities::ExpandString(parts, value, '/');
	if (parts.empty() || parts.size() > 2)
		return false;
	if (!specification.ParsePropertyDeclaration(dictionary, definition.items[0].property_id, parts[0]))
		return false;
	if (parts.size() == 2)
		return specification.ParsePropertyDeclaration(dictionary, definition.items[1].property_id, parts[1]);
	dictionary.SetProperty(definition.items[1].property_id, *definition.items[1].property_definition->GetDefaultValue());
	return true;
}

} // namespace

TEST_CASE("ShorthandTypes.any_order")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	const ShorthandId id = StyleSheetSpecification::RegisterShorthand("test-any-order", "overflow-x, color, width", ShorthandType::AnyOrder);
	REQUIRE(id != ShorthandId::Invalid);
	PropertyDictionary dictionary;

	SUBCASE("every order")
	{
		for (const char* value : {"hidden #f00 10px", "#f00 10px hidden", "10px hidden #f00", "10px #f00 hidden"})
		{
			INFO(String(value));
			CHECK(Parse(dictionary, "test-any-order", value));
			CHECK(ParsedValue(dictionary, "overflow-x") == "hidden");
			CHECK(ParsedValue(dictionary, "color") == "#ff0000");
			CHECK(ParsedValue(dictionary, "width") == "10px");
		}
	}

	SUBCASE("omitted items take their initial value")
	{
		CHECK(Parse(dictionary, "test-any-order", "10px"));
		CHECK(ParsedValue(dictionary, "width") == "10px");
		CHECK(ParsedValue(dictionary, "overflow-x") == "visible");
		CHECK(ParsedValue(dictionary, "color") == StyleSheetSpecification::GetProperty(PropertyId::Color)->GetDefaultValue()->ToString());
		CHECK(dictionary.GetNumProperties() == 3);
	}

	SUBCASE("each value goes to the first item that parses it")
	{
		// 'auto' parses as overflow-x and as width: the first item takes it, then the second 'auto' the width
		CHECK(Parse(dictionary, "test-any-order", "auto auto"));
		CHECK(ParsedValue(dictionary, "overflow-x") == "auto");
		CHECK(ParsedValue(dictionary, "width") == "auto");
	}

	SUBCASE("a value no item takes fails the declaration")
	{
		CHECK_FALSE(Parse(dictionary, "test-any-order", "10px #f00 nonsense"));
		CHECK_FALSE(Parse(dictionary, "test-any-order", "#f00 #00f")); // the color is assigned already
		CHECK_FALSE(Parse(dictionary, "test-any-order", "hidden #f00 10px 20px"));
		CHECK(dictionary.GetNumProperties() == 0);

		// the destination keeps what it had
		PropertyDictionary existing;
		REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(existing, "width", "5px"));
		CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(existing, "test-any-order", "10px #f00 nonsense"));
		CHECK(ParsedValue(existing, "width") == "5px");
		CHECK(existing.GetNumProperties() == 1);
	}

	SUBCASE("var() is substituted at computed-value time")
	{
		CHECK(Parse(dictionary, "test-any-order", "var(--c) 10px"));
		CHECK(dictionary.GetProperty(PropertyId::Width)->unit == Unit::SHORTHAND_PLACEHOLDER);
		CHECK(dictionary.GetProperty(PropertyId::Color)->unit == Unit::SHORTHAND_PLACEHOLDER);
	}

	SUBCASE("shorthand items")
	{
		const ShorthandId nested = StyleSheetSpecification::RegisterShorthand("test-any-order-nested", "padding, color", ShorthandType::AnyOrder);
		REQUIRE(nested != ShorthandId::Invalid);
		CHECK(Parse(dictionary, "test-any-order-nested", "#00f 4px"));
		for (const char* side : {"padding-top", "padding-right", "padding-bottom", "padding-left"})
			CHECK(ParsedValue(dictionary, side) == "4px");
		CHECK(ParsedValue(dictionary, "color") == "#0000ff");

		// an omitted shorthand item sets every longhand under it to its initial value
		CHECK(Parse(dictionary, "test-any-order-nested", "#00f"));
		for (const char* side : {"padding-top", "padding-right", "padding-bottom", "padding-left"})
			CHECK(ParsedValue(dictionary, side) == "0px");
	}

	SUBCASE("in a document")
	{
		ElementDocument* document = context->LoadDocumentFromMemory(R"(<rml><head><style>
			div { display: block; test-any-order: 40px hidden #0f0; }
			#second { test-any-order: 20px; }
		</style></head><body><div id="first"/><div id="second"/></body></rml>)");
		REQUIRE(document);
		document->Show();
		context->Update();
		const ComputedValues& first = document->GetElementById("first")->GetComputedValues();
		CHECK(first.width().value == 40.f);
		CHECK(first.overflow_x() == Style::Overflow::Hidden);
		CHECK(first.color() == Colourb(0, 255, 0));
		// the later shorthand resets the items it omits
		const ComputedValues& second = document->GetElementById("second")->GetComputedValues();
		CHECK(second.width().value == 20.f);
		CHECK(second.overflow_x() == Style::Overflow::Visible);
		document->Close();
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("ShorthandTypes.custom")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	const ShorthandId id = StyleSheetSpecification::RegisterShorthand("test-custom", "color, width", ShorthandType::Custom);
	REQUIRE(id != ShorthandId::Invalid);
	PropertyDictionary dictionary;
	custom_parser_calls = 0;

	SUBCASE("without a parser the declaration fails")
	{
		CHECK_FALSE(Parse(dictionary, "test-custom", "#f00"));
	}

	SUBCASE("dispatch")
	{
		REQUIRE(StyleSheetSpecification::RegisterShorthandParser(id, &ParseTestCustom));
		CHECK(Parse(dictionary, "test-custom", "#f00 / 12px"));
		CHECK(custom_parser_calls == 1);
		CHECK(custom_parser_value == "#f00 / 12px"); // the raw value
		CHECK(ParsedValue(dictionary, "color") == "#ff0000");
		CHECK(ParsedValue(dictionary, "width") == "12px");

		// omitted: the initial value
		CHECK(Parse(dictionary, "test-custom", "#f00"));
		CHECK(ParsedValue(dictionary, "width") == "auto");

		// a failing parser sets nothing, also what it set before failing
		PropertyDictionary existing;
		REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(existing, "width", "5px"));
		CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(existing, "test-custom", "#0f0 / nonsense"));
		CHECK(ParsedValue(existing, "width") == "5px");
		CHECK(existing.GetProperty(PropertyId::Color) == nullptr);

		// var() never reaches the parser
		const int calls = custom_parser_calls;
		CHECK(Parse(dictionary, "test-custom", "var(--c) / 12px"));
		CHECK(custom_parser_calls == calls);
		CHECK(dictionary.GetProperty(PropertyId::Width)->unit == Unit::SHORTHAND_PLACEHOLDER);
	}

	SUBCASE("only custom shorthands take a parser")
	{
		TestsShell::SetNumExpectedWarnings(1);
		CHECK_FALSE(StyleSheetSpecification::RegisterShorthandParser(ShorthandId::Margin, &ParseTestCustom));
		TestsShell::SetNumExpectedWarnings(0);
	}

	TestsShell::ShutdownShell();
}

TEST_CASE("ShorthandTypes.trailing_values_are_rejected")
{
	TestsShell::GetContext();
	PropertyDictionary dictionary;
	// FallThrough, Replicate, Box, Flex, RecursiveRepeat, RecursiveCommaSeparated (upstream PR #983 for the existing types)
	CHECK_FALSE(Parse(dictionary, "flex-flow", "row wrap row"));
	CHECK_FALSE(Parse(dictionary, "overflow", "hidden scroll auto"));
	CHECK_FALSE(Parse(dictionary, "margin", "1px 2px 3px 4px 5px"));
	CHECK_FALSE(Parse(dictionary, "flex", "1 1 auto 3"));
	CHECK_FALSE(Parse(dictionary, "gap", "1px 2px 3px"));
	CHECK(Parse(dictionary, "flex-flow", "row wrap"));
	CHECK(Parse(dictionary, "margin", "1px 2px 3px 4px"));
	TestsShell::ShutdownShell();
}

TEST_CASE("ShorthandTypes.list_splitters")
{
	StringList items;
	SplitCommaList("linear-gradient(red, blue), url('a,b.png') 10px, \"x, y\", , last", items);
	REQUIRE(items.size() == 5);
	CHECK(items[0] == "linear-gradient(red, blue)");
	CHECK(items[1] == "url('a,b.png') 10px");
	CHECK(items[2] == "\"x, y\"");
	CHECK(items[3] == "");
	CHECK(items[4] == "last");

	SplitCommaList("single", items);
	REQUIRE(items.size() == 1);
	CHECK(items[0] == "single");

	SplitWhitespaceList("  1px  calc(100% - 2px)\tsolid  'a b' rgb(0 0 0 / 50%) ", items);
	REQUIRE(items.size() == 5);
	CHECK(items[0] == "1px");
	CHECK(items[1] == "calc(100% - 2px)");
	CHECK(items[2] == "solid");
	CHECK(items[3] == "'a b'");
	CHECK(items[4] == "rgb(0 0 0 / 50%)");

	SplitWhitespaceList("", items);
	CHECK(items.empty());
}
