#include "../../../Source/Core/GridStyleValues.h"
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

// CSS Grid (phase 3, docs/html-ui/phase-3-fork-patches.md 3.14.1, FORK.md P3-G1): the grammar of the grid properties and
// shorthands, the values they parse into, their serialization, and the declarations they reject.

namespace {

// The declaration's longhands as written back, "<none>" for a longhand it does not set; "<invalid>" when it fails.
String Parse(const String& name, const String& value, const String& longhand)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value))
		return "<invalid>";
	const Property* property = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(longhand));
	return property ? property->ToString() : String("<none>");
}

String Parse(const String& name, const String& value)
{
	return Parse(name, value, name);
}

bool Valid(const String& name, const String& value)
{
	PropertyDictionary dictionary;
	return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
}

template <typename T>
SharedPtr<const T> ParsedValue(const String& name, const String& value)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value))
		return nullptr;
	const Property* property = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(name));
	if (!property || property->unit != Unit::STYLE_VALUE)
		return nullptr;
	return std::static_pointer_cast<const T>(property->Get<StyleValuePtr>());
}

} // namespace

TEST_CASE("GridParsing.display")
{
	TestsShell::GetContext();
	CHECK(Parse("display", "grid") == "grid");
	CHECK(Parse("display", "inline-grid") == "inline-grid");

	PropertyDictionary dictionary;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "display", "grid"));
	CHECK(dictionary.GetProperty(PropertyId::Display)->Get<int>() == int(Style::Display::Grid));
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "display", "inline-grid"));
	CHECK(dictionary.GetProperty(PropertyId::Display)->Get<int>() == int(Style::Display::InlineGrid));
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "display", "list-item"));
	CHECK(dictionary.GetProperty(PropertyId::Display)->Get<int>() == int(Style::Display::ListItem));
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "display", "contents"));
	CHECK(dictionary.GetProperty(PropertyId::Display)->Get<int>() == int(Style::Display::Contents));
}

TEST_CASE("GridParsing.track_list")
{
	TestsShell::GetContext();
	const char* name = "grid-template-columns";

	SUBCASE("round trips")
	{
		for (const char* value : {"none", "100px", "1fr", "0.5fr", "auto", "min-content", "max-content", "10%", "2em",
				 "100px 1fr auto", "minmax(100px, 1fr)", "minmax(auto, 300px)", "minmax(min-content, max-content)", "fit-content(200px)",
				 "fit-content(50%)", "repeat(3, 1fr)", "repeat(2, 10px 20px)", "repeat(auto-fill, 100px)", "repeat(auto-fit, minmax(120px, 1fr))",
				 "[a] 10px [b c] 20px [d]", "[start] repeat(2, [x] 1fr [y]) [end]", "repeat(auto-fill, [col] 40px)", "40px repeat(auto-fill, 40px) 40px",
				 "calc(100% - 20px) 1fr", "minmax(0px, 1fr)", "[] 10px"})
		{
			INFO(value);
			String expected = value;
			if (expected == "[] 10px")
				expected = "10px";
			CHECK(Parse(name, value) == expected);
		}
	}

	SUBCASE("normalization")
	{
		CHECK(Parse(name, "REPEAT( 3 ,  1FR )") == "repeat(3, 1fr)");
		CHECK(Parse(name, "  10px   20px ") == "10px 20px");
		CHECK(Parse(name, "0") == "0px");
		CHECK(Parse(name, "None") == "none");
	}

	SUBCASE("invalid")
	{
		for (const char* value : {"", "foo", "-10px", "-1fr", "minmax(1fr, 100px)", "minmax(10px)", "minmax(10px, 20px, 30px)",
				 "repeat(0, 10px)", "repeat(-1, 10px)", "repeat(2)", "repeat(2, )", "repeat(auto-fill, 1fr)", "repeat(auto-fill, auto)",
				 "repeat(auto-fill, 10px) repeat(auto-fit, 10px)", "repeat(auto-fill, 10px) 1fr", "repeat(2, repeat(2, 10px))",
				 "[a] [b] 10px", "[a]", "[auto] 10px", "[span] 10px", "[1a] 10px", "10px [a", "10px a]", "none 10px", "fit-content(auto)",
				 "fit-content(1fr)", "subgrid", "masonry", "10px / 20px", "\"a\""})
		{
			INFO(value);
			CHECK_FALSE(Valid(name, value));
		}
	}

	SUBCASE("value")
	{
		auto list = ParsedValue<GridTrackListValue>(name, "[a] 10px repeat(auto-fit, [b] minmax(20px, 1fr) [c]) [d e]");
		REQUIRE(bool(list));
		CHECK_FALSE(list->none);
		REQUIRE(list->components.size() == 2);
		REQUIRE(list->line_names.size() == 3);
		CHECK(list->line_names[0] == StringList{"a"});
		CHECK(list->line_names[1].empty());
		CHECK(list->line_names[2] == StringList{"d", "e"});
		CHECK_FALSE(list->components[0].is_repeat);
		CHECK(list->components[0].track.min.kind == GridTrackBreadth::Kind::Length);
		CHECK(list->components[0].track.min.length.value == NumericValue(10.f, Unit::PX));
		const GridTrackRepeat& repeat = list->components[1].repeat;
		CHECK(list->components[1].is_repeat);
		CHECK(repeat.count == GridTrackRepeat::Count::AutoFit);
		REQUIRE(repeat.tracks.size() == 1);
		CHECK(repeat.tracks[0].form == GridTrackSize::Form::MinMax);
		CHECK(repeat.tracks[0].max.kind == GridTrackBreadth::Kind::Fr);
		CHECK(repeat.tracks[0].max.fr == 1.f);
		REQUIRE(repeat.line_names.size() == 2);
		CHECK(repeat.line_names[0] == StringList{"b"});
		CHECK(repeat.line_names[1] == StringList{"c"});
		CHECK(list->GetAutoRepeat() == &repeat);

		auto fit = ParsedValue<GridTrackListValue>(name, "fit-content(40%)");
		REQUIRE(bool(fit));
		CHECK(fit->components[0].track.form == GridTrackSize::Form::FitContent);
		CHECK(fit->components[0].track.fit_content_limit.value == NumericValue(40.f, Unit::PERCENT));
		CHECK_FALSE(fit->components[0].track.HasFixedComponent());

		auto none = ParsedValue<GridTrackListValue>(name, "none");
		REQUIRE(bool(none));
		CHECK(none->none);
		CHECK(none->ToString() == "none");
	}

	SUBCASE("rows")
	{
		CHECK(Parse("grid-template-rows", "50px [middle] auto") == "50px [middle] auto");
		CHECK_FALSE(Valid("grid-template-rows", "auto-fill"));
	}
}

TEST_CASE("GridParsing.auto_tracks")
{
	TestsShell::GetContext();
	CHECK(Parse("grid-auto-rows", "auto") == "auto");
	CHECK(Parse("grid-auto-rows", "100px") == "100px");
	CHECK(Parse("grid-auto-columns", "minmax(10px, auto) 1fr") == "minmax(10px, auto) 1fr");
	CHECK(Parse("grid-auto-columns", "fit-content(10em)") == "fit-content(10em)");
	CHECK_FALSE(Valid("grid-auto-rows", "none"));
	CHECK_FALSE(Valid("grid-auto-rows", "repeat(2, 10px)"));
	CHECK_FALSE(Valid("grid-auto-rows", "[a] 10px"));
	CHECK_FALSE(Valid("grid-auto-rows", "-5px"));

	auto value = ParsedValue<GridAutoTracksValue>("grid-auto-rows", "10px 1fr");
	REQUIRE(bool(value));
	REQUIRE(value->tracks.size() == 2);
	CHECK(value->tracks[1].min.kind == GridTrackBreadth::Kind::Fr);
}

TEST_CASE("GridParsing.template_areas")
{
	TestsShell::GetContext();
	const char* name = "grid-template-areas";
	CHECK(Parse(name, "none") == "none");
	CHECK(Parse(name, "\"a b\" \"c d\"") == "\"a b\" \"c d\"");
	CHECK(Parse(name, "'head head'   'nav  main'") == "\"head head\" \"nav main\"");
	CHECK(Parse(name, "\"a...b\"") == "\"a . b\"");

	auto areas = ParsedValue<GridTemplateAreasValue>(name, "\"head head head\" \"nav main .\" \"foot foot foot\"");
	REQUIRE(bool(areas));
	CHECK(areas->num_rows == 3);
	CHECK(areas->num_columns == 3);
	REQUIRE(areas->areas.size() == 4);
	CHECK(areas->areas[0] == GridTemplateArea{"head", 1, 2, 1, 4});
	CHECK(areas->areas[1] == GridTemplateArea{"nav", 2, 3, 1, 2});
	CHECK(areas->areas[2] == GridTemplateArea{"main", 2, 3, 2, 3});
	CHECK(areas->areas[3] == GridTemplateArea{"foot", 3, 4, 1, 4});

	for (const char* value : {"", "a", "\"a b\" \"c\"", "\"a b a\"", "\"a a\" \"b a\"", "\"a\" \"b\" \"a\"", "\"a # b\"", "\"\"", "\"a\" 10px"})
	{
		INFO(value);
		CHECK_FALSE(Valid(name, value));
	}
}

TEST_CASE("GridParsing.line")
{
	TestsShell::GetContext();
	const char* name = "grid-row-start";
	for (const char* value : {"auto", "1", "-1", "3", "a", "2 a", "span 2", "span a", "span 3 a"})
	{
		INFO(value);
		CHECK(Parse(name, value) == value);
	}
	CHECK(Parse(name, "a 2") == "2 a");
	CHECK(Parse(name, "span 1") == "span 1");
	CHECK(Parse(name, "2 a span") == "span 2 a");
	CHECK(Parse(name, "SPAN 2") == "span 2");
	for (const char* value : {"", "0", "0 a", "span", "span -1", "span 0", "auto 2", "a b", "2 3", "span span", "2 span a", "1.5", "span auto",
			 "inherit-x y", "default"})
	{
		INFO(value);
		CHECK_FALSE(Valid(name, value));
	}
	// 'initial' is not a line name: it is the CSS-wide keyword (p3-cascade), valid on every property.
	CHECK(Valid(name, "initial"));
	CHECK(Parse(name, "initial") == "initial");

	auto line = ParsedValue<GridLineValue>(name, "-2 col");
	REQUIRE(bool(line));
	CHECK(line->kind == GridLineValue::Kind::NamedLine);
	CHECK(line->index == -2);
	CHECK(line->name == "col");
	CHECK_FALSE(line->IsCustomIdent());

	line = ParsedValue<GridLineValue>(name, "span 2");
	REQUIRE(bool(line));
	CHECK(line->kind == GridLineValue::Kind::Span);
	CHECK(line->index == 2);

	line = ParsedValue<GridLineValue>(name, "span x");
	REQUIRE(bool(line));
	CHECK(line->kind == GridLineValue::Kind::NamedSpan);
	CHECK(line->index == 1);
}

TEST_CASE("GridParsing.auto_flow")
{
	TestsShell::GetContext();
	const char* name = "grid-auto-flow";
	CHECK(Parse(name, "row") == "row");
	CHECK(Parse(name, "column") == "column");
	CHECK(Parse(name, "dense") == "row dense");
	CHECK(Parse(name, "dense column") == "column dense");
	CHECK(Parse(name, "row   dense") == "row dense");
	for (const char* value : {"", "rows", "row column", "dense dense", "row dense column"})
	{
		INFO(value);
		CHECK_FALSE(Valid(name, value));
	}
	PropertyDictionary dictionary;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, "column dense"));
	CHECK(dictionary.GetProperty(PropertyId::GridAutoFlow)->Get<int>() == int(Style::GridAutoFlow::ColumnDense));
}

TEST_CASE("GridParsing.line_shorthands")
{
	TestsShell::GetContext();
	CHECK(Parse("grid-row", "1 / 3", "grid-row-start") == "1");
	CHECK(Parse("grid-row", "1 / 3", "grid-row-end") == "3");
	CHECK(Parse("grid-row", "2", "grid-row-end") == "auto");
	CHECK(Parse("grid-row", "span 2", "grid-row-end") == "auto");
	CHECK(Parse("grid-row", "main", "grid-row-start") == "main");
	CHECK(Parse("grid-row", "main", "grid-row-end") == "main");
	CHECK(Parse("grid-row", "2 main", "grid-row-end") == "auto");
	CHECK(Parse("grid-column", "1 / -1", "grid-column-start") == "1");
	CHECK(Parse("grid-column", "1 / -1", "grid-column-end") == "-1");
	CHECK(Parse("grid-column", "span 2 / 5", "grid-column-start") == "span 2");
	CHECK(Parse("grid-column", "a/b", "grid-column-end") == "b");
	for (const char* value : {"", "/", "1 /", "/ 2", "1 / 2 / 3", "0", "1 / 0"})
	{
		INFO(value);
		CHECK_FALSE(Valid("grid-row", value));
	}

	// grid-area: row-start / column-start / row-end / column-end, omitted ones copied from a custom-ident.
	CHECK(Parse("grid-area", "header", "grid-row-start") == "header");
	CHECK(Parse("grid-area", "header", "grid-column-start") == "header");
	CHECK(Parse("grid-area", "header", "grid-row-end") == "header");
	CHECK(Parse("grid-area", "header", "grid-column-end") == "header");
	CHECK(Parse("grid-area", "2", "grid-column-start") == "auto");
	CHECK(Parse("grid-area", "2", "grid-row-end") == "auto");
	CHECK(Parse("grid-area", "1 / 2", "grid-row-end") == "auto");
	CHECK(Parse("grid-area", "1 / b", "grid-column-end") == "b");
	CHECK(Parse("grid-area", "a / 2 / 3", "grid-column-end") == "auto");
	CHECK(Parse("grid-area", "1 / 2 / 3 / 4", "grid-row-start") == "1");
	CHECK(Parse("grid-area", "1 / 2 / 3 / 4", "grid-column-start") == "2");
	CHECK(Parse("grid-area", "1 / 2 / 3 / 4", "grid-row-end") == "3");
	CHECK(Parse("grid-area", "1 / 2 / 3 / 4", "grid-column-end") == "4");
	CHECK_FALSE(Valid("grid-area", "1 / 2 / 3 / 4 / 5"));
	CHECK_FALSE(Valid("grid-area", "1 / / 3"));
}

TEST_CASE("GridParsing.template_shorthand")
{
	TestsShell::GetContext();
	CHECK(Parse("grid-template", "none", "grid-template-rows") == "none");
	CHECK(Parse("grid-template", "none", "grid-template-columns") == "none");
	CHECK(Parse("grid-template", "none", "grid-template-areas") == "none");

	CHECK(Parse("grid-template", "100px 1fr / 50px auto", "grid-template-rows") == "100px 1fr");
	CHECK(Parse("grid-template", "100px 1fr / 50px auto", "grid-template-columns") == "50px auto");
	CHECK(Parse("grid-template", "100px 1fr / 50px auto", "grid-template-areas") == "none");
	CHECK(Parse("grid-template", "repeat(2, 1fr) / repeat(auto-fill, 10px)", "grid-template-columns") == "repeat(auto-fill, 10px)");

	const String areas = "[header-top] \"a a a\" [header-bottom] [main-top] \"b b b\" 1fr [main-bottom] / auto 1fr auto";
	CHECK(Parse("grid-template", areas, "grid-template-areas") == "\"a a a\" \"b b b\"");
	CHECK(Parse("grid-template", areas, "grid-template-rows") == "[header-top] auto [header-bottom main-top] 1fr [main-bottom]");
	CHECK(Parse("grid-template", areas, "grid-template-columns") == "auto 1fr auto");
	CHECK(Parse("grid-template", "\"a\" \"b\"", "grid-template-rows") == "auto auto");
	CHECK(Parse("grid-template", "\"a\" \"b\"", "grid-template-columns") == "none");
	CHECK(Parse("grid-template", "\"a b\" 30px", "grid-template-rows") == "30px");

	for (const char* value : {"", "10px", "10px /", "/ 10px", "\"a\" / repeat(2, 10px)", "\"a\" 10px 20px", "10px \"a\"", "\"a b\" \"c\"",
			 "[x] [y] \"a\"", "\"a\" / 10px / 20px"})
	{
		INFO(value);
		CHECK_FALSE(Valid("grid-template", value));
	}
}

TEST_CASE("GridParsing.grid_shorthand")
{
	TestsShell::GetContext();
	CHECK(Parse("grid", "auto-flow / 1fr 1fr", "grid-auto-flow") == "row");
	CHECK(Parse("grid", "auto-flow / 1fr 1fr", "grid-template-columns") == "1fr 1fr");
	CHECK(Parse("grid", "auto-flow / 1fr 1fr", "grid-template-rows") == "none");
	CHECK(Parse("grid", "auto-flow / 1fr 1fr", "grid-auto-rows") == "auto");
	CHECK(Parse("grid", "auto-flow dense 40px / 1fr", "grid-auto-flow") == "row dense");
	CHECK(Parse("grid", "auto-flow dense 40px / 1fr", "grid-auto-rows") == "40px");
	CHECK(Parse("grid", "dense auto-flow / 1fr", "grid-auto-flow") == "row dense");
	CHECK(Parse("grid", "100px 300px / auto-flow 200px", "grid-auto-flow") == "column");
	CHECK(Parse("grid", "100px 300px / auto-flow 200px", "grid-auto-columns") == "200px");
	CHECK(Parse("grid", "100px 300px / auto-flow 200px", "grid-template-rows") == "100px 300px");
	CHECK(Parse("grid", "100px / auto-flow dense", "grid-auto-flow") == "column dense");
	CHECK(Parse("grid", "10px / auto-flow 10px 20px", "grid-auto-columns") == "10px 20px");

	// The grid-template forms reset the auto properties.
	CHECK(Parse("grid", "\"a b\" 10px / 1fr 2fr", "grid-template-areas") == "\"a b\"");
	CHECK(Parse("grid", "\"a b\" 10px / 1fr 2fr", "grid-auto-flow") == "row");
	CHECK(Parse("grid", "10px / 20px", "grid-auto-rows") == "auto");
	CHECK(Parse("grid", "none", "grid-auto-columns") == "auto");

	for (const char* value : {"auto-flow", "auto-flow / auto-flow", "auto-flow auto-flow / 1fr", "dense / 1fr"})
	{
		INFO(value);
		CHECK_FALSE(Valid("grid", value));
	}
}

TEST_CASE("GridParsing.gaps")
{
	TestsShell::GetContext();
	CHECK(Parse("gap", "10px", "row-gap") == "10px");
	CHECK(Parse("gap", "10px 20%", "column-gap") == "20%");
	CHECK(Parse("row-gap", "normal") == "0px");
	CHECK(Parse("gap", "normal", "column-gap") == "0px");
	CHECK_FALSE(Valid("row-gap", "-1px"));
	CHECK(Parse("grid-gap", "5px 7px", "row-gap") == "5px");
	CHECK(Parse("grid-gap", "5px 7px", "column-gap") == "7px");
	CHECK(Parse("grid-row-gap", "3px", "row-gap") == "3px");
	CHECK(Parse("grid-column-gap", "4px", "column-gap") == "4px");

	// The column gap is a fraction of the width (6.3 named the height).
	const PropertyDefinition* column_gap = StyleSheetSpecification::GetProperty(PropertyId::ColumnGap);
	REQUIRE(column_gap);
	CHECK(column_gap->GetRelativeTarget() == RelativeTarget::ContainingBlockWidth);
}

TEST_CASE("GridParsing.computed")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const String document_rml = R"(
<rml>
<head>
	<style>
		#grid { display: grid; grid: "a b" 20px / 1fr 2fr; grid-auto-flow: column dense; }
		#item { grid-area: b; }
	</style>
</head>
<body><div id="grid"><div id="item"/></div></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* grid = document->GetElementById("grid");
	Element* item = document->GetElementById("item");
	const ComputedValues& computed = grid->GetComputedValues();
	CHECK(computed.display() == Style::Display::Grid);
	CHECK(computed.grid_auto_flow() == Style::GridAutoFlow::ColumnDense);
	REQUIRE(bool(computed.grid_template_columns()));
	CHECK(computed.grid_template_columns()->ToString() == "1fr 2fr");
	REQUIRE(bool(computed.grid_template_areas()));
	CHECK(computed.grid_template_areas()->ToString() == "\"a b\"");
	REQUIRE(bool(item->GetComputedValues().grid_row_start()));
	CHECK(item->GetComputedValues().grid_row_start()->ToString() == "b");
	CHECK_FALSE(bool(document->GetComputedValues().grid_template_rows()));
	CHECK(document->GetComputedValues().grid_auto_flow() == Style::GridAutoFlow::Row);

	grid->SetProperty("grid-template-columns", "repeat(2, 30px)");
	context->Update();
	CHECK(grid->GetComputedValues().grid_template_columns()->ToString() == "repeat(2, 30px)");

	document->Close();
	TestsShell::ShutdownShell();
}
