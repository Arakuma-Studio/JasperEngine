#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 values (docs/html-ui/phase-3-fork-patches.md 3.3.3): transform-origin and perspective-origin take CSS
// <position> values, keywords in either order, edge offsets for perspective-origin, a z length for transform-origin.

namespace {

struct Origin {
	float x, y, z;
};

const String position_rml = R"(
<rml>
<head>
	<style>
		body { font-family: LatoLatin; }
		#o { display: block; width: 200px; height: 100px; }
	</style>
</head>
<body><div id="o"/></body>
</rml>
)";

bool Parses(const char* shorthand, const char* value)
{
	PropertyDictionary dictionary;
	return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, shorthand, value);
}

} // namespace

TEST_CASE("Position.origins")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(position_rml);
	REQUIRE(document);
	document->Show();
	Element* element = document->GetElementById("o");

	auto transform_origin = [&](const char* value) {
		REQUIRE(element->SetProperty("transform-origin", value));
		context->Update();
		const auto& computed = element->GetComputedValues();
		return Origin{ResolveValue(computed.transform_origin_x(), 200.f), ResolveValue(computed.transform_origin_y(), 100.f),
			computed.transform_origin_z()};
	};
	auto perspective_origin = [&](const char* value) {
		REQUIRE(element->SetProperty("perspective-origin", value));
		context->Update();
		const auto& computed = element->GetComputedValues();
		return Origin{ResolveValue(computed.perspective_origin_x(), 200.f), ResolveValue(computed.perspective_origin_y(), 100.f), 0.f};
	};

	struct Case {
		const char* value;
		Origin expected;
	};
	const Case transform_cases[] = {
		{"top left", {0.f, 0.f, 0.f}},
		{"left top", {0.f, 0.f, 0.f}},
		{"bottom right", {200.f, 100.f, 0.f}},
		{"left", {0.f, 50.f, 0.f}},
		{"top", {100.f, 0.f, 0.f}},
		{"center", {100.f, 50.f, 0.f}},
		{"center bottom", {100.f, 100.f, 0.f}},
		{"bottom center", {100.f, 100.f, 0.f}},
		{"10px 20px", {10.f, 20.f, 0.f}},
		{"25% 75%", {50.f, 75.f, 0.f}},
		{"left 10px", {0.f, 10.f, 0.f}},
		{"10px top", {10.f, 0.f, 0.f}},
		{"right bottom 5px", {200.f, 100.f, 5.f}},
		{"10px 20px 3px", {10.f, 20.f, 3.f}},
		{"calc(50% + 10px) top", {110.f, 0.f, 0.f}},
	};
	for (const Case& test : transform_cases)
	{
		INFO("transform-origin: ", test.value);
		const Origin origin = transform_origin(test.value);
		CHECK(origin.x == doctest::Approx(test.expected.x));
		CHECK(origin.y == doctest::Approx(test.expected.y));
		CHECK(origin.z == doctest::Approx(test.expected.z));
	}

	const Case perspective_cases[] = {
		{"top left", {0.f, 0.f, 0.f}},
		{"right 10px bottom 20%", {190.f, 80.f, 0.f}},
		{"bottom 10px right 20px", {180.f, 90.f, 0.f}},
		{"center top", {100.f, 0.f, 0.f}},
		{"30px", {30.f, 50.f, 0.f}},
	};
	for (const Case& test : perspective_cases)
	{
		INFO("perspective-origin: ", test.value);
		const Origin origin = perspective_origin(test.value);
		CHECK(origin.x == doctest::Approx(test.expected.x));
		CHECK(origin.y == doctest::Approx(test.expected.y));
	}

	CHECK(Parses("transform-origin", "calc(25% + min(1px, 2px)) calc(75% - max(1px, 2px))"));
	CHECK(Parses("transform-origin", "10px 20px calc(1px + min(2px, 3px))"));
	CHECK(Parses("transform-origin-z", "calc(1px + min(2px, 3px))"));

	// Omitted parts reset: z returns to 0.
	transform_origin("10px 20px 30px");
	CHECK(transform_origin("left").z == 0.f);

	for (const char* invalid : {"top 10px", "left left", "top bottom", "10px left", "1px 2px 3px 4px", "left top 5%", "right 10px bottom 20%", ""})
	{
		INFO("transform-origin: ", invalid);
		CHECK_FALSE(Parses("transform-origin", invalid));
	}
	for (const char* invalid : {"left top 50px", "center center center", "top 10px", "left 10px right 5px", "10px left"})
	{
		INFO("perspective-origin: ", invalid);
		CHECK_FALSE(Parses("perspective-origin", invalid));
	}

	document->Close();
	TestsShell::ShutdownShell();
}
