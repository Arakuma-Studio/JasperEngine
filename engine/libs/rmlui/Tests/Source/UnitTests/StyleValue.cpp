#include "../../../Source/Core/ElementStyle.h"
#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/PropertyParser.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleValue.h>
#include <RmlUi/Core/Variant.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.5, FORK.md P3-6): StyleValue, the one Variant slot for
// the complex values of phase 3, through parsing, serialization, comparison, transitions and unit dependencies; and the
// value helpers its types use (LengthPercentageValue, ParsePosition).

namespace {

// A length, interpolable: "pair(<length>)"
class TestLength final : public StyleValue {
public:
	TestLength(float number, Unit unit) : value(number, unit) {}

	String ToString() const override { return "pair(" + Property(value.number, value.unit).ToString() + ")"; }
	bool Equals(const StyleValue& other) const override { return value == static_cast<const TestLength&>(other).value; }
	SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha, Element& /*element*/) const override
	{
		const TestLength& target = static_cast<const TestLength&>(to);
		if (target.value.unit != value.unit)
			return nullptr;
		interpolations++;
		return MakeShared<TestLength>(value.number + (target.value.number - value.number) * alpha, value.unit);
	}
	Units GetUnitDependencies() const override { return value.unit & (Unit::EM | Unit::REM | Unit::VW | Unit::VH); }

	NumericValue value;
	static int interpolations;
};
int TestLength::interpolations = 0;

// A name, not interpolable: "tag(<name>)"
class TestTag final : public StyleValue {
public:
	explicit TestTag(String name) : name(std::move(name)) {}
	String ToString() const override { return "tag(" + name + ")"; }
	bool Equals(const StyleValue& other) const override { return name == static_cast<const TestTag&>(other).name; }
	String name;
};

// Reads "name(argument)"
bool ReadFunction(const String& value, const char* name, String& argument)
{
	const String prefix = String(name) + "(";
	if (value.size() <= prefix.size() || value.compare(0, prefix.size(), prefix) != 0 || value.back() != ')')
		return false;
	argument = value.substr(prefix.size(), value.size() - prefix.size() - 1);
	return true;
}

class TestLengthParser final : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
	{
		String argument;
		if (!ReadFunction(value, "pair", argument))
			return false;
		Property length;
		if (!StyleSheetSpecification::GetParser("length")->ParseValue(length, argument, ParameterMap()) || !Any(length.unit & Unit::LENGTH))
			return false;
		property.value = Variant(StyleValuePtr(MakeShared<TestLength>(length.Get<float>(), length.unit)));
		property.unit = Unit::STYLE_VALUE;
		return true;
	}
};

class TestTagParser final : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
	{
		String argument;
		if (!ReadFunction(value, "tag", argument) || argument.empty())
			return false;
		property.value = Variant(StyleValuePtr(MakeShared<TestTag>(argument)));
		property.unit = Unit::STYLE_VALUE;
		return true;
	}
};

// (outlive Rml::Shutdown, as registered parsers must)
TestLengthParser test_length_parser;
TestTagParser test_tag_parser;

void RegisterTestProperties()
{
	StyleSheetSpecification::RegisterParser("test_length", &test_length_parser);
	StyleSheetSpecification::RegisterParser("test_tag", &test_tag_parser);
	StyleSheetSpecification::RegisterProperty("test-length", "pair(0px)", false, false).AddParser("test_length");
	StyleSheetSpecification::RegisterProperty("test-tag", "tag(none)", false, false).AddParser("test_tag");
}

String Serialized(Element* element, const String& name)
{
	const Property* property = element->GetProperty(name);
	return property ? property->ToString() : String("<none>");
}

} // namespace

TEST_CASE("StyleValue.variant_and_property")
{
	TestsShell::GetContext();
	RegisterTestProperties();

	const StyleValuePtr a = MakeShared<TestLength>(10.f, Unit::PX);
	const StyleValuePtr same = MakeShared<TestLength>(10.f, Unit::PX);
	const StyleValuePtr other = MakeShared<TestLength>(11.f, Unit::PX);

	Variant variant(a);
	CHECK(variant.GetType() == Variant::STYLEVALUEPTR);
	CHECK(bool(variant.Get<StyleValuePtr>() == a));
	CHECK(variant.Get<String>() == "pair(10px)");
	CHECK(variant == Variant(same)); // equal values, different objects
	CHECK(variant != Variant(other));
	CHECK(Variant(StyleValuePtr()) == Variant(StyleValuePtr()));
	CHECK(variant != Variant(StyleValuePtr()));

	Variant copy = variant;
	CHECK(bool(copy.Get<StyleValuePtr>() == a));
	Variant moved = std::move(copy);
	CHECK(bool(moved.Get<StyleValuePtr>() == a));

	PropertyDictionary dictionary;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "test-length", "pair(2em)"));
	const Property* parsed = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId("test-length"));
	REQUIRE(parsed);
	CHECK(parsed->unit == Unit::STYLE_VALUE);
	CHECK(parsed->ToString() == "pair(2em)");
	const StyleValuePtr value = parsed->Get<StyleValuePtr>();
	REQUIRE(bool(value));
	CHECK(static_cast<const TestLength&>(*value).value == NumericValue(2.f, Unit::EM));
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "test-length", "pair(red)"));

	TestsShell::ShutdownShell();
}

static const String style_value_rml = R"(
<rml>
<head>
	<style>
		div { display: block; test-length: pair(10px); test-tag: tag(first); transition: test-length test-tag 1s linear allow-discrete; }
		div.end { test-length: pair(20px); test-tag: tag(second); }
		#em { test-length: pair(2em); transition: none; }
	</style>
</head>
<body><div id="transitioned"/><div id="em"/></body>
</rml>
)";

TEST_CASE("StyleValue.transitions")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	RegisterTestProperties();
	TestsSystemInterface* system_interface = TestsShell::GetTestsSystemInterface();
	REQUIRE(system_interface);

	system_interface->SetManualTime(0.0);
	ElementDocument* document = context->LoadDocumentFromMemory(style_value_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* element = document->GetElementById("transitioned");
	CHECK(Serialized(element, "test-length") == "pair(10px)");
	CHECK(Serialized(element, "test-tag") == "tag(first)");

	TestLength::interpolations = 0;
	element->SetClass("end", true);
	context->Update(); // the transitions start now, at 0 s
	double t = 0.0;
	auto advance_to = [&](double time) {
		// (animations advance at most 0.1 s per update)
		while (t < time)
		{
			t = Math::Min(t + 0.05, time);
			system_interface->SetManualTime(t);
			context->Update();
		}
	};

	advance_to(0.25);
	CHECK(TestLength::interpolations > 0);
	CHECK(Serialized(element, "test-tag") == "tag(first)"); // not interpolable: discrete, switches at half way
	advance_to(0.5);
	CHECK(Serialized(element, "test-length") == "pair(15px)");
	advance_to(0.75);
	CHECK(Serialized(element, "test-tag") == "tag(second)");
	advance_to(1.5);
	CHECK(Serialized(element, "test-length") == "pair(20px)");
	CHECK(Serialized(element, "test-tag") == "tag(second)");

	document->Close();
	system_interface->SetManualTime(0.0);
	TestsShell::ShutdownShell();
}

TEST_CASE("StyleValue.unit_dependencies")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	RegisterTestProperties();

	ElementDocument* document = context->LoadDocumentFromMemory(style_value_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	// a value in em is dirtied with the font size
	Element* em = document->GetElementById("em");
	REQUIRE_FALSE(em->GetStyle()->AnyPropertiesDirty());
	em->GetStyle()->DirtyPropertiesWithUnits(Unit::EM);
	CHECK(em->GetStyle()->AnyPropertiesDirty());

	// a value in px is not
	Element* px = document->GetElementById("transitioned");
	REQUIRE_FALSE(px->GetStyle()->AnyPropertiesDirty());
	px->GetStyle()->DirtyPropertiesWithUnits(Unit::EM);
	CHECK_FALSE(px->GetStyle()->AnyPropertiesDirty());

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("StyleValue.length_percentage")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	LengthPercentageValue value;
	REQUIRE(ParseLengthPercentage("10px", value));
	CHECK(value.ToString() == "10px");
	CHECK_FALSE(Any(value.GetUnitDependencies()));
	REQUIRE(ParseLengthPercentage("50%", value));
	CHECK(value == LengthPercentageValue(50.f, Unit::PERCENT));
	REQUIRE(ParseLengthPercentage("2em", value));
	CHECK(value.GetUnitDependencies() == Unit::EM);
	CHECK_FALSE(ParseLengthPercentage("red", value));
	CHECK_FALSE(ParseLengthPercentage("10deg", value));

	ElementDocument* document = context->LoadDocumentFromMemory(R"(<rml><head><style>body { font-size: 20px; }</style></head><body/></rml>)");
	REQUIRE(document);
	document->Show();
	context->Update();
	CHECK(ResolveLengthPercentage(LengthPercentageValue(10.f, Unit::PX), *document, 200.f) == doctest::Approx(10.f));
	CHECK(ResolveLengthPercentage(LengthPercentageValue(25.f, Unit::PERCENT), *document, 200.f) == doctest::Approx(50.f));
	CHECK(ResolveLengthPercentage(LengthPercentageValue(2.f, Unit::EM), *document, 200.f) == doctest::Approx(40.f));
#ifdef RMLUI_MATH_EXPRESSIONS
	REQUIRE(ParseLengthPercentage("calc(50% - 1em)", value));
	CHECK(bool(value.calculation));
	CHECK(value.GetUnitDependencies() == Unit::EM);
	CHECK(ResolveLengthPercentage(value, *document, 200.f) == doctest::Approx(80.f));
#endif
	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("StyleValue.parse_position")
{
	TestsShell::GetContext();

	struct Case {
		const char* tokens;
		const char* x;
		const char* y;
	};
	const Case valid[] = {
		{"center", "50%", "50%"},
		{"left", "0%", "50%"},
		{"top", "50%", "0%"},
		{"10px", "10px", "50%"},
		{"left top", "0%", "0%"},
		{"top left", "0%", "0%"},
		{"bottom right", "100%", "100%"},
		{"center left", "0%", "50%"},
		{"left center", "0%", "50%"},
		{"10px top", "10px", "0%"},
		{"25% 75%", "25%", "75%"},
		{"right 20% bottom 10%", "80%", "90%"},
		{"bottom 10% right 20%", "80%", "90%"},
		{"left 10px top", "10px", "0%"},
		{"center top 5px", "50%", "5px"},
#ifdef RMLUI_MATH_EXPRESSIONS
		{"right 10px top", "calc(100% - 10px)", "0%"},
		{"left 10px bottom 2em", "10px", "calc(100% - 2em)"},
#endif
	};
	for (const Case& c : valid)
	{
		INFO(String(c.tokens));
		StringList tokens;
		StringUtilities::ExpandString(tokens, c.tokens, ' ');
		LengthPercentageValue x, y;
		REQUIRE(ParsePosition(tokens, x, y));
		CHECK(x.ToString() == c.x);
		CHECK(y.ToString() == c.y);
	}

	const char* invalid[] = {"", "top 10px", "10px left", "left right", "top bottom", "center 10px top", "left 10px 20px", "left top center", "red",
		"left 10px top 5px 1px"};
	for (const char* tokens_text : invalid)
	{
		INFO(String(tokens_text));
		StringList tokens;
		StringUtilities::ExpandString(tokens, tokens_text, ' ');
		LengthPercentageValue x, y;
		CHECK_FALSE(ParsePosition(tokens, x, y));
	}

	TestsShell::ShutdownShell();
}
