#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/RenderBox.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-paint with p3-animation: the paint properties interpolate as Chromium interpolates them (outline colour,
// width and offset, percentage and elliptical radii, object-position, border widths from keywords, opacity); object-fit,
// isolation and the styles are discrete.

namespace {

const String document_head = R"(
<rml>
<head>
	<style>
		body { inset: 0; }
		div { display: block; width: 100px; height: 50px; }
)";
const String document_tail = R"(
	</style>
</head>
<body><div id="a"/></body>
</rml>
)";

struct Fixture {
	Context* context = nullptr;
	TestsSystemInterface* system = nullptr;
	ElementDocument* document = nullptr;
	Element* a = nullptr;
	double t = 0.0;

	explicit Fixture(const String& style) : context(TestsShell::GetContext()), system(TestsShell::GetTestsSystemInterface())
	{
		system->SetManualTime(0.0);
		document = context->LoadDocumentFromMemory(document_head + style + document_tail);
		REQUIRE(document);
		document->Show();
		context->Update();
		a = document->GetElementById("a");
	}
	~Fixture()
	{
		document->Close();
		system->SetManualTime(0.0);
		TestsShell::ShutdownShell();
	}
	void AdvanceTo(double time)
	{
		while (t < time)
		{
			t = Math::Min(t + 0.05, time);
			system->SetManualTime(t);
			context->Update();
		}
	}
	void Start()
	{
		a->SetClass("on", true);
		context->Update();
		AdvanceTo(0.5);
	}
};

} // namespace

TEST_CASE("PaintTransitions.outline")
{
	Fixture f(R"(
		#a { color: #000; outline: 2px solid #000000; outline-offset: 0px; transition: outline-color 1s linear, outline-width 1s linear, outline-offset 1s linear; }
		#a.on { outline: 6px solid #ff0000; outline-offset: 10px; }
	)");
	f.Start();
	const ComputedValues& computed = f.a->GetComputedValues();
	CHECK(computed.outline_width() == doctest::Approx(4.f));
	CHECK(computed.outline_offset() == doctest::Approx(5.f));
	CHECK(int(computed.outline_color().red) == doctest::Approx(128).epsilon(0.02));
	f.AdvanceTo(1.5);
	CHECK(computed.outline_width() == doctest::Approx(6.f));
}

TEST_CASE("PaintTransitions.line_width_keywords")
{
	// thin, medium and thick are lengths (1, 3, 5px): they interpolate with each other and with lengths.
	Fixture f(R"(
		#a { outline: thin solid; border: thin solid #000; transition: outline-width 1s linear, border-top-width 1s linear; }
		#a.on { outline-width: thick; border-top-width: 9px; }
	)");
	f.Start();
	const ComputedValues& computed = f.a->GetComputedValues();
	CHECK(computed.outline_width() == doctest::Approx(3.f));
	CHECK(computed.border_top_width() == doctest::Approx(5.f));
}

TEST_CASE("PaintTransitions.border_radius")
{
	// A length to a percentage, and an elliptical radius: each axis on its own (the percentages of the border box).
	Fixture f(R"(
		#a { border-radius: 10px; transition: border-radius 1s linear; }
		#a.on { border-radius: 50% / 20%; }
	)");
	f.Start();
	const CornerSizes2 radii = f.a->GetRenderBox(BoxArea::Border).GetBorderRadius();
	CHECK(radii[0].x == doctest::Approx(30.f)); // (10 + 50) / 2
	CHECK(radii[0].y == doctest::Approx(10.f)); // (10 + 10) / 2
	f.AdvanceTo(1.5);
	CHECK(f.a->GetRenderBox(BoxArea::Border).GetBorderRadius()[2].x == doctest::Approx(50.f));
}

TEST_CASE("PaintTransitions.object_position")
{
	Fixture f(R"(
		#a { object-fit: none; object-position: left top; transition: object-position 1s linear, object-fit 1s linear; }
		#a.on { object-fit: cover; object-position: right 10px bottom; }
	)");
	f.Start();
	const ComputedValues& computed = f.a->GetComputedValues();
	// 0% to calc(100% - 10px): half way, 50% - 5px of the free space.
	CHECK(ResolveValue(computed.object_position_x(), 100.f) == doctest::Approx(45.f));
	CHECK(ResolveValue(computed.object_position_y(), 100.f) == doctest::Approx(50.f));
	// object-fit is discrete: at once without allow-discrete.
	CHECK(computed.object_fit() == Style::ObjectFit::Cover);
}

TEST_CASE("PaintTransitions.opacity")
{
	Fixture f(R"(
		#a { opacity: 1; transition: opacity 1s linear; }
		#a.on { opacity: 0.5; }
	)");
	f.Start();
	CHECK(f.a->GetComputedValues().opacity() == doctest::Approx(0.75f));
}
