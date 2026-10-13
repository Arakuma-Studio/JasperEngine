#include "../../../Source/Core/PaintProperties.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/ObjectFit.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-paint (docs/html-ui/phase-3-fork-patches.md 3.9.3): object-fit's five values, object-position's <position>.

namespace {

using Style::LengthPercentage;
using Style::ObjectFit;

String Text(const Rectanglef& r)
{
	return CreateString("%g %g %g %g", r.Left(), r.Top(), r.Width(), r.Height());
}

ObjectFitRect Fit(ObjectFit fit, Vector2f box, Vector2f object, LengthPercentage x = LengthPercentage(LengthPercentage::Percentage, 50.f),
	LengthPercentage y = LengthPercentage(LengthPercentage::Percentage, 50.f))
{
	ObjectFitRect out;
	REQUIRE(ComputeObjectFitRect(box, object, fit, x, y, out));
	return out;
}

} // namespace

TEST_CASE("ObjectFit.rects")
{
	const Vector2f box(200.f, 100.f), object(100.f, 100.f);

	// fill: the whole box, the whole image.
	ObjectFitRect r = Fit(ObjectFit::Fill, box, object);
	CHECK(Text(r.rect) == "0 0 200 100");
	CHECK(Text(r.uv) == "0 0 1 1");

	// contain: 100 x 100 in the middle.
	r = Fit(ObjectFit::Contain, box, object);
	CHECK(Text(r.rect) == "50 0 100 100");
	CHECK(Text(r.uv) == "0 0 1 1");

	// cover: 200 x 200 cropped to the box (no overdraw), the middle half of the image shown.
	r = Fit(ObjectFit::Cover, box, object);
	CHECK(Text(r.rect) == "0 0 200 100");
	CHECK(Text(r.uv) == "0 0.25 1 0.5");
	CHECK(r.size == Vector2f(200.f));

	// none: its own size, centred; larger than the box it is cropped.
	r = Fit(ObjectFit::None, box, object);
	CHECK(Text(r.rect) == "50 0 100 100");
	r = Fit(ObjectFit::None, box, Vector2f(300.f, 50.f));
	CHECK(Text(r.rect) == "0 25 200 50");
	CHECK(Text(r.uv) == CreateString("%g 0 %g 1", 50.f / 300.f, 200.f / 300.f));

	// scale-down: none when it fits, else contain.
	r = Fit(ObjectFit::ScaleDown, box, Vector2f(40.f, 20.f));
	CHECK(Text(r.rect) == "80 40 40 20");
	r = Fit(ObjectFit::ScaleDown, box, Vector2f(400.f, 100.f));
	CHECK(Text(r.rect) == "0 25 200 50");

	// object-position: percentages of the free space, lengths from the start edge.
	r = Fit(ObjectFit::Contain, box, object, LengthPercentage(LengthPercentage::Percentage, 0.f));
	CHECK(Text(r.rect) == "0 0 100 100");
	r = Fit(ObjectFit::Contain, box, object, LengthPercentage(LengthPercentage::Percentage, 100.f));
	CHECK(Text(r.rect) == "100 0 100 100");
	r = Fit(ObjectFit::Contain, box, object, LengthPercentage(LengthPercentage::Length, 10.f));
	CHECK(Text(r.rect) == "10 0 100 100");

	// An object without an intrinsic size fills the box; nothing visible is false.
	r = Fit(ObjectFit::Contain, box, Vector2f(0.f));
	CHECK(Text(r.rect) == "0 0 200 100");
	ObjectFitRect out;
	CHECK_FALSE(ComputeObjectFitRect(box, object, ObjectFit::None, LengthPercentage(LengthPercentage::Length, 500.f),
		LengthPercentage(LengthPercentage::Percentage, 50.f), out));
	CHECK_FALSE(ComputeObjectFitRect(Vector2f(0.f, 10.f), object, ObjectFit::Fill, LengthPercentage(), LengthPercentage(), out));
}

TEST_CASE("ObjectFit.position_syntax")
{
	TestsShell::GetContext();
	auto position = [](const char* value) {
		String x, y;
		if (!ParsePosition(value, x, y))
			return String("invalid");
		return x + " | " + y;
	};
	CHECK(position("center") == "50% | 50%");
	CHECK(position("left") == "0% | 50%");
	CHECK(position("top") == "50% | 0%");
	CHECK(position("30px") == "30px | 50%");
	CHECK(position("30px 40%") == "30px | 40%");
	CHECK(position("left bottom") == "0% | 100%");
	CHECK(position("bottom left") == "0% | 100%");
	CHECK(position("center top") == "50% | 0%");
	CHECK(position("top center") == "50% | 0%");
	CHECK(position("right 10px bottom 20%") == "calc(100% - 10px) | calc(100% - 20%)");
	CHECK(position("left 10px top") == "10px | 0%");
	CHECK(position("bottom 5px center") == "50% | calc(100% - 5px)");
	CHECK(position("top 10px") == "invalid");
	CHECK(position("10px left") == "invalid");
	CHECK(position("left right") == "invalid");
	CHECK(position("center 10px top") == "invalid");
	CHECK(position("1px 2px 3px") == "invalid");

	PropertyDictionary dictionary;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "object-position", "right 10px bottom 20px"));
	CHECK(dictionary.GetNumProperties() == 2);
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "object-fit", "scale-down"));
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "object-fit", "stretch"));
	TestsShell::ShutdownShell();
}
