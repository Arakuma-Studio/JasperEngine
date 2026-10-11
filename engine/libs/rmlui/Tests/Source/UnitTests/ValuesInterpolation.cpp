#include "../../../Source/Core/TransformState.h"
#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Math.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StyleSheetTypes.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 values in transitions and animations (docs/html-ui/phase-3-fork-patches.md 3.3, 3.13; FORK.md P3-V11): calc()
// against a length is a calc mix (CSS Values 4, combining values: the percentage stays relative while the value runs), the units
// p3-values adds interpolate as lengths and angles, and 'currentcolor' interpolates with a colour (as Chromium: the
// element's colour at each frame), also in shadows and gradient stops; a calc() in a shadow is its length in pixels.

namespace {

const String document_head = R"(
<rml>
<head>
	<style>
		body { inset: 0; font-family: LatoLatin; font-size: 10px; }
		div { display: block; height: 10px; }
		#parent { width: 200px; }
)";
const String document_tail = R"(
	</style>
</head>
<body>
	<div id="parent"><div id="a"/><div id="b"/><div id="ref"/></div>
</body>
</rml>
)";

struct Fixture {
	Context* context = nullptr;
	TestsSystemInterface* system = nullptr;
	ElementDocument* document = nullptr;
	Element *parent = nullptr, *a = nullptr, *b = nullptr, *ref = nullptr;
	double t = 0.0;

	explicit Fixture(const String& style) : context(TestsShell::GetContext()), system(TestsShell::GetTestsSystemInterface())
	{
		system->SetManualTime(0.0);
		document = context->LoadDocumentFromMemory(document_head + style + document_tail);
		REQUIRE(document);
		document->Show();
		context->Update();
		parent = document->GetElementById("parent");
		a = document->GetElementById("a");
		b = document->GetElementById("b");
		ref = document->GetElementById("ref");
	}
	~Fixture()
	{
		document->Close();
		system->SetManualTime(0.0);
		TestsShell::ShutdownShell();
	}
	void Update() { context->Update(); }
	void AdvanceTo(double time)
	{
		while (t < time)
		{
			t = Math::Min(t + 0.05, time);
			system->SetManualTime(t);
			context->Update();
		}
	}
	// The width a length gives, measured on the reference element.
	float Measure(const char* width)
	{
		ref->SetProperty("width", width);
		context->Update();
		return ref->GetBox().GetSize().x;
	}
};

float Width(Element* element)
{
	return element->GetBox().GetSize().x;
}

// The rotation of the element's transform, in degrees (the transform state is updated when the context renders).
float RotationDegrees(Element* element)
{
	element->GetContext()->Render();
	const TransformState* state = element->GetTransformState();
	if (!state || !state->GetTransform())
		return 0.f;
	const Matrix4f& matrix = *state->GetTransform();
	return Math::RadiansToDegrees(Math::ATan2(matrix[0][1], matrix[0][0]));
}

bool Near(Colourb colour, Colourb expected)
{
	return Math::Absolute(int(colour.red) - int(expected.red)) <= 1 && Math::Absolute(int(colour.green) - int(expected.green)) <= 1 &&
		Math::Absolute(int(colour.blue) - int(expected.blue)) <= 1 && Math::Absolute(int(colour.alpha) - int(expected.alpha)) <= 1;
}

} // namespace

TEST_CASE("ValuesInterpolation.calc_mix_transition")
{
	Fixture f(R"(
		#a { width: 10px; transition: width 1s linear; }
		#b { width: calc(50% + 2em); transition: width 1s linear; }
	)");
	CHECK(Width(f.b) == doctest::Approx(120.f));

	// 10px -> calc(50% + 2em): not a discrete flip at the midpoint.
	f.a->SetProperty("width", "calc(50% + 2em)");
	f.b->SetProperty("width", "10px");
	f.Update();
	f.AdvanceTo(0.25);
	CHECK(Width(f.a) == doctest::Approx(0.75f * 10.f + 0.25f * 120.f));
	CHECK(Width(f.b) == doctest::Approx(0.25f * 10.f + 0.75f * 120.f));

	// A calc mix: the percentage follows the containing block while the transition runs.
	f.parent->SetProperty("width", "400px");
	f.AdvanceTo(0.5);
	CHECK(Width(f.a) == doctest::Approx(0.5f * 10.f + 0.5f * 220.f));
	CHECK(Width(f.b) == doctest::Approx(0.5f * 10.f + 0.5f * 220.f));
	f.AdvanceTo(1.5);
	CHECK(Width(f.a) == doctest::Approx(220.f));
	CHECK(Width(f.b) == doctest::Approx(10.f));
}

TEST_CASE("ValuesInterpolation.calc_mix_animation")
{
	Fixture f(R"(
		@keyframes grow { from { width: 10px; } to { width: calc(50% + 2em); } }
		@keyframes units { from { width: calc(1vmin + 2ch); } to { width: 4ex; } }
		#a { animation: grow 1s linear; }
		#b { animation: units 1s linear; }
	)");
	const float from_units = f.Measure("calc(1vmin + 2ch)");
	const float to_units = f.Measure("4ex");
	CHECK(from_units > 8.f);
	CHECK(to_units > 0.f);

	f.AdvanceTo(0.5);
	CHECK(Width(f.a) == doctest::Approx(0.5f * 10.f + 0.5f * 120.f));
	CHECK(Width(f.b) == doctest::Approx(0.5f * (from_units + to_units)).epsilon(0.01));
	f.parent->SetProperty("width", "400px");
	f.AdvanceTo(0.75);
	CHECK(Width(f.a) == doctest::Approx(0.25f * 10.f + 0.75f * 220.f));
}

TEST_CASE("ValuesInterpolation.units")
{
	Fixture f(R"(
		#a { width: 10vmin; transition: width 1s linear; }
		#b { width: 2ch; transition: width 1s linear; }
	)");
	const float vmin10 = f.Measure("10vmin");
	const float ch4 = f.Measure("4ch");
	const float ch6 = f.Measure("6ch");
	const float ex3 = f.Measure("3ex");
	CHECK(vmin10 == doctest::Approx(80.f));
	CHECK(ch4 > 0.f);
	CHECK(ex3 > 0.f);

	// vmin -> ch (lengths in px), ch -> ch (in ch).
	f.a->SetProperty("width", "4ch");
	f.b->SetProperty("width", "6ch");
	f.Update();
	f.AdvanceTo(0.5);
	CHECK(Width(f.a) == doctest::Approx(0.5f * (vmin10 + ch4)).epsilon(0.01));
	CHECK(Width(f.b) == doctest::Approx(0.5f * (0.5f * ch4 + ch6)).epsilon(0.01));
	f.AdvanceTo(1.5);
	CHECK(Width(f.a) == doctest::Approx(ch4));

	// ch -> ex.
	f.a->SetProperty("width", "3ex");
	f.Update();
	f.AdvanceTo(2.0);
	CHECK(Width(f.a) == doctest::Approx(0.5f * (ch4 + ex3)).epsilon(0.01));
}

TEST_CASE("ValuesInterpolation.angles")
{
	Fixture f(R"(
		@keyframes spin { from { transform: rotate(0.25turn); } to { transform: rotate(200grad); } }
		@keyframes spin_calc { from { transform: rotate(10deg); } to { transform: rotate(calc(0.25turn + 10grad)); } }
		#a { transform-origin: 0 0; animation: spin 1s linear; }
		#b { transform-origin: 0 0; transform: rotate(0.125turn); transition: transform 1s linear; }
		#ref { transform-origin: 0 0; animation: spin_calc 1s linear; }
	)");
	CHECK(RotationDegrees(f.b) == doctest::Approx(45.f).epsilon(0.001));
	f.b->SetProperty("transform", "rotate(100grad)");
	f.Update();
	f.AdvanceTo(0.5);
	// 90deg -> 180deg, 45deg -> 90deg, 10deg -> 99deg.
	CHECK(RotationDegrees(f.a) == doctest::Approx(135.f).epsilon(0.001));
	CHECK(RotationDegrees(f.b) == doctest::Approx(67.5f).epsilon(0.001));
	CHECK(RotationDegrees(f.ref) == doctest::Approx(54.5f).epsilon(0.001));
}

TEST_CASE("ValuesInterpolation.currentcolor_transition")
{
	Fixture f(R"(
		#a { color: #ff0000; border-width: 2px; border-color: currentcolor; transition: border-color 1s linear; }
		#b { color: #ff0000; border-width: 2px; border-color: #0000ff; transition: border-color 1s linear; }
		#ref { color: #ff0000; border-width: 2px; border-color: currentcolor; transition: border-color 1s linear; }
	)");
	CHECK(f.a->GetComputedValues().border_top_color() == Colourb(255, 0, 0));

	// currentcolor -> blue and back: interpolated, not discrete.
	f.a->SetProperty("border-color", "#0000ff");
	f.b->SetProperty("border-color", "currentcolor");
	// The computed value 'currentcolor' does not change with 'color': no border-color transition, the border follows.
	f.ref->SetProperty("color", "#00ff00");
	f.Update();
	CHECK(f.ref->GetComputedValues().border_top_color() == Colourb(0, 255, 0));
	f.AdvanceTo(0.5);
	CHECK(Near(f.a->GetComputedValues().border_top_color(), Colourb(128, 0, 128)));
	CHECK(Near(f.b->GetComputedValues().border_top_color(), Colourb(128, 0, 128)));
	CHECK(f.ref->GetComputedValues().border_top_color() == Colourb(0, 255, 0));

	// The 'currentcolor' end is the element's colour at each frame (Chromium: a colour mix with currentcolor).
	f.b->SetProperty("color", "#00ff00");
	f.AdvanceTo(0.75);
	CHECK(Near(f.b->GetComputedValues().border_top_color(), Colourb(0, 191, 64)));
	f.AdvanceTo(1.5);
	CHECK(f.a->GetComputedValues().border_top_color() == Colourb(0, 0, 255));
	CHECK(f.b->GetComputedValues().border_top_color() == Colourb(0, 255, 0));
}

TEST_CASE("ValuesInterpolation.currentcolor_animation_and_shadows")
{
	Fixture f(R"(
		@keyframes border { from { border-color: currentcolor; } to { border-color: #0000ff; } }
		@keyframes shadow { from { box-shadow: 0 0 0 2px currentcolor; } to { box-shadow: 0 0 0 4px #0000ff; } }
		#a { color: #ff0000; border-width: 2px; animation: border 1s linear; }
		@keyframes gradient { from { decorator: linear-gradient(currentcolor, #0000ff); } to { decorator: linear-gradient(#0000ff, #0000ff); } }
		#b { color: #ff0000; animation: shadow 1s linear; }
		#ref { color: #ff0000; animation: gradient 1s linear; }
	)");
	f.AdvanceTo(0.5);
	CHECK(Near(f.a->GetComputedValues().border_top_color(), Colourb(128, 0, 128)));

	const Property* shadow = f.b->GetProperty("box-shadow");
	REQUIRE(shadow);
	REQUIRE(shadow->unit == Unit::BOXSHADOWLIST);
	const BoxShadowList& list = shadow->value.GetReference<BoxShadowList>();
	REQUIRE(list.size() == 1);
	CHECK(!list[0].current_color);
	CHECK(Near(list[0].color.ToNonPremultiplied(), Colourb(128, 0, 128)));
	CHECK(list[0].spread_distance.number == doctest::Approx(3.f));

	// A gradient's colour stops.
	const Property* decorator = f.ref->GetProperty("decorator");
	REQUIRE(decorator);
	REQUIRE(decorator->unit == Unit::DECORATOR);
	const DecoratorsPtr& decorators = decorator->value.GetReference<DecoratorsPtr>();
	REQUIRE(bool(decorators));
	REQUIRE(decorators->list.size() == 1);
	const ColorStopList* stops = nullptr;
	for (const auto& id_property : decorators->list[0].properties.GetProperties())
		if (id_property.second.unit == Unit::COLORSTOPLIST)
			stops = &id_property.second.value.GetReference<ColorStopList>();
	REQUIRE(stops);
	REQUIRE(stops->size() == 2);
	CHECK(!(*stops)[0].current_color);
	CHECK(Near((*stops)[0].color.ToNonPremultiplied(), Colourb(128, 0, 128)));
	CHECK(Near((*stops)[1].color.ToNonPremultiplied(), Colourb(0, 0, 255)));
}

TEST_CASE("ValuesInterpolation.shadow_calc")
{
	Fixture f(R"(
		@keyframes shadow { from { box-shadow: 0 0 0 calc(1em + 2px) #000; } to { box-shadow: 0 0 0 4px #000; } }
		#a { animation: shadow 1s linear; }
	)");
	f.AdvanceTo(0.5);
	const Property* shadow = f.a->GetProperty("box-shadow");
	REQUIRE(shadow);
	REQUIRE(shadow->unit == Unit::BOXSHADOWLIST);
	const BoxShadowList& list = shadow->value.GetReference<BoxShadowList>();
	REQUIRE(list.size() == 1);
	CHECK(list[0].spread_distance.unit == Unit::PX);
	CHECK(list[0].spread_distance.number == doctest::Approx(8.f));
}
