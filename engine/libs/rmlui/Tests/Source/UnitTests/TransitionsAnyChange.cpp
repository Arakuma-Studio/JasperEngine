#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (docs/html-ui/phase-3-fork-patches.md 3.13.4, FORK.md P3-A3): transitions start on any change of a computed
// value, not only when the matched rules change.

namespace {

const String document_head = R"(
<rml>
<head>
	<style>
		body { inset: 0; }
		div { display: block; height: 10px; }
)";
const String document_tail = R"(
	</style>
</head>
<body>
	<div id="parent"><div id="a"/><div id="b"/></div>
</body>
</rml>
)";

struct TestClock {
	Context* context;
	TestsSystemInterface* system;
	double t = 0.0;
	void AdvanceTo(double time)
	{
		while (t < time)
		{
			t = Math::Min(t + 0.05, time);
			system->SetManualTime(t);
			context->Update();
		}
	}
};

float Width(Element* element)
{
	return element->GetBox().GetSize().x;
}

class EventLog : public EventListener {
public:
	void ProcessEvent(Event& event) override
	{
		log.push_back(event.GetType() + " " + event.GetParameter<String>("propertyName", "") + CreateString(" %g", event.GetParameter<float>("elapsedTime", -1.f)));
	}
	StringList log;
};

struct Fixture {
	Context* context = nullptr;
	TestClock clock;
	ElementDocument* document = nullptr;
	Element* parent = nullptr;
	Element* a = nullptr;
	Element* b = nullptr;

	explicit Fixture(const String& style) : context(TestsShell::GetContext()), clock{context, TestsShell::GetTestsSystemInterface()}
	{
		clock.system->SetManualTime(0.0);
		document = context->LoadDocumentFromMemory(document_head + style + document_tail);
		REQUIRE(document);
		document->Show();
		context->Update();
		parent = document->GetElementById("parent");
		a = document->GetElementById("a");
		b = document->GetElementById("b");
	}
	~Fixture()
	{
		document->Close();
		clock.system->SetManualTime(0.0);
		TestsShell::ShutdownShell();
	}
	// A change takes effect at the update of the current time, as a frame after a script runs.
	void Update() { context->Update(); }
};

} // namespace

TEST_CASE("TransitionsAnyChange.inline_style")
{
	Fixture f(R"(
		#a, #b { width: 10px; transition: width 1s linear; }
	)");

	// The style attribute and SetProperty transition (6.3 transitioned class and pseudo-class changes only).
	f.a->SetAttribute("style", "width: 20px");
	f.b->SetProperty("width", "30px");
	f.Update();
	CHECK(Width(f.a) == doctest::Approx(10.f));
	f.clock.AdvanceTo(0.5);
	CHECK(Width(f.a) == doctest::Approx(15.f));
	CHECK(Width(f.b) == doctest::Approx(20.f));
	f.clock.AdvanceTo(1.5);
	CHECK(Width(f.a) == doctest::Approx(20.f));
	CHECK(Width(f.b) == doctest::Approx(30.f));

	// Removing the inline value transitions back to the style sheet's.
	f.b->RemoveProperty("width");
	f.Update();
	f.clock.AdvanceTo(2.0);
	CHECK(Width(f.b) == doctest::Approx(20.f));
}

TEST_CASE("TransitionsAnyChange.var_and_inherited")
{
	Fixture f(R"(
		#parent { --w: 10px; color: #000; }
		#a { width: var(--w); transition: width 1s linear; }
		#b { width: 10px; transition: color 1s linear; }
	)");

	// A change of a variable, and of an inherited value (the parent itself does not transition).
	f.parent->SetProperty("--w", "30px");
	f.parent->SetProperty("color", "#ff0000");
	f.Update();
	CHECK(f.parent->GetComputedValues().color() == Colourb(255, 0, 0));
	f.clock.AdvanceTo(0.5);
	CHECK(Width(f.a) == doctest::Approx(20.f));
	const Colourb color = f.b->GetComputedValues().color();
	CHECK(color.red > 0);
	CHECK(color.red < 255);
	f.clock.AdvanceTo(1.5);
	CHECK(Width(f.a) == doctest::Approx(30.f));
	CHECK(f.b->GetComputedValues().color() == Colourb(255, 0, 0));
}

TEST_CASE("TransitionsAnyChange.class_toggle_and_reversing")
{
	Fixture f(R"(
		#a { width: 0px; transition: width 1s linear; }
		#a.wide { width: 100px; }
	)");

	f.a->SetClass("wide", true);
	f.Update();
	f.clock.AdvanceTo(0.25);
	CHECK(Width(f.a) == doctest::Approx(25.f));

	// Reversing at a quarter: back to the start in a quarter of the duration (CSS reversing shortening factor).
	f.a->SetClass("wide", false);
	f.Update();
	f.clock.AdvanceTo(0.375);
	CHECK(Width(f.a) == doctest::Approx(12.5f));
	f.clock.AdvanceTo(0.5);
	CHECK(Width(f.a) == doctest::Approx(0.f));

	// Not a reversal: a new transition from the current value, with the full duration.
	f.a->SetClass("wide", true);
	f.Update();
	f.clock.AdvanceTo(1.0);
	CHECK(Width(f.a) == doctest::Approx(50.f));
	f.a->SetProperty("width", "50px");
	f.Update();
	f.clock.AdvanceTo(2.0);
	CHECK(Width(f.a) == doctest::Approx(50.f));
}

TEST_CASE("TransitionsAnyChange.no_before_change_style")
{
	Fixture f(R"(
		#a { width: 10px; transition: width 1s linear; }
		#a.hidden { display: none; }
		.new { width: 40px; transition: width 1s linear; }
	)");

	// A change while the element is 'display: none', shown in the same update: no transition.
	f.a->SetClass("hidden", true);
	f.Update();
	f.clock.AdvanceTo(0.1);
	f.a->SetProperty("width", "50px");
	f.a->SetClass("hidden", false);
	f.Update();
	CHECK(Width(f.a) == doctest::Approx(50.f));

	// A new element is styled at once.
	ElementPtr element = f.document->CreateElement("div");
	element->SetClass("new", true);
	Element* added = f.parent->AppendChild(std::move(element));
	f.Update();
	CHECK(Width(added) == doctest::Approx(40.f));
	f.clock.AdvanceTo(0.5);
	CHECK(Width(added) == doctest::Approx(40.f));
}

TEST_CASE("TransitionsAnyChange.starting_style")
{
	Fixture f(R"(
		.pop { width: 40px; opacity: 1; transition: opacity 1s linear, display 1s linear allow-discrete; }
		.pop.hidden { display: none; opacity: 0; }
		@starting-style {
			.pop { opacity: 0; }
		}
	)");

	// A new element transitions from its starting style.
	ElementPtr element = f.document->CreateElement("div");
	element->SetClass("pop", true);
	Element* pop = f.parent->AppendChild(std::move(element));
	f.Update();
	CHECK(pop->GetComputedValues().opacity() == doctest::Approx(0.f));
	f.clock.AdvanceTo(0.5);
	CHECK(pop->GetComputedValues().opacity() == doctest::Approx(0.5f));
	f.clock.AdvanceTo(1.5);
	CHECK(pop->GetComputedValues().opacity() == doctest::Approx(1.f));

	// Hiding with 'allow-discrete': it stays displayed until the transition ends.
	pop->SetClass("hidden", true);
	f.Update();
	f.clock.AdvanceTo(2.0);
	CHECK(pop->GetComputedValues().display() == Style::Display::Block);
	CHECK(pop->GetComputedValues().opacity() == doctest::Approx(0.5f));
	f.clock.AdvanceTo(2.6);
	CHECK(pop->GetComputedValues().display() == Style::Display::None);

	// Shown again from 'display: none': from its starting style again.
	pop->SetClass("hidden", false);
	f.Update();
	CHECK(pop->GetComputedValues().display() == Style::Display::Block);
	CHECK(pop->GetComputedValues().opacity() == doctest::Approx(0.f));
	f.clock.AdvanceTo(3.1);
	CHECK(pop->GetComputedValues().opacity() == doctest::Approx(0.5f));
}

TEST_CASE("TransitionsAnyChange.discrete")
{
	Fixture f(R"(
		#a { width: 10px; transition: display 1s linear; }
		#b { width: 10px; transition: display 1s linear allow-discrete; }
		.gone { display: none; }
	)");

	// A discrete change without 'allow-discrete' is immediate.
	f.a->SetClass("gone", true);
	f.b->SetClass("gone", true);
	f.Update();
	f.clock.AdvanceTo(0.1);
	CHECK(f.a->GetComputedValues().display() == Style::Display::None);
	CHECK(f.b->GetComputedValues().display() == Style::Display::Block);
	f.clock.AdvanceTo(1.1);
	CHECK(f.b->GetComputedValues().display() == Style::Display::None);
}

TEST_CASE("TransitionsAnyChange.relative_units")
{
	Fixture f(R"(
		#a { font-size: 10px; padding-left: 2em; width: 10px; transition: all 1s linear; }
		#a.big { font-size: 20px; }
	)");

	// 'all' transitions every property whose computed value changes: the em padding follows the font size.
	f.a->SetClass("big", true);
	f.Update();
	f.clock.AdvanceTo(0.5);
	CHECK(f.a->GetComputedValues().font_size() == doctest::Approx(15.f));
	CHECK(f.a->GetBox().GetEdge(BoxArea::Padding, BoxEdge::Left) == doctest::Approx(30.f).epsilon(0.05));
	f.clock.AdvanceTo(1.5);
	CHECK(f.a->GetBox().GetEdge(BoxArea::Padding, BoxEdge::Left) == doctest::Approx(40.f));
}

TEST_CASE("TransitionsAnyChange.transition_comes_with_the_change")
{
	Fixture f(R"(
		#a { width: 10px; }
		#a.on { width: 30px; transition: width 1s linear; }
	)");

	// The transition is declared by the rule that changes the value (as 'a:hover { transition: ... }'): it runs.
	f.a->SetClass("on", true);
	f.Update();
	f.clock.AdvanceTo(0.5);
	CHECK(Width(f.a) == doctest::Approx(20.f));
	// Leaving the rule removes the transition: the value changes at once.
	f.a->SetClass("on", false);
	f.Update();
	f.clock.AdvanceTo(0.6);
	CHECK(Width(f.a) == doctest::Approx(10.f));
}

TEST_CASE("TransitionsAnyChange.events")
{
	Fixture f(R"(
		#a { width: 10px; transition: width 1s linear 0.5s; }
		#b { width: 10px; transition: width 1s linear; }
	)");
	EventLog log;
	for (Element* element : {f.a, f.b})
		for (const char* type : {"transitionrun", "transitionstart", "transitionend", "transitioncancel"})
			element->AddEventListener(type, &log);

	f.a->SetProperty("width", "20px");
	f.Update();
	f.clock.AdvanceTo(0.25);
	REQUIRE(log.log.size() == 1);
	CHECK(log.log[0] == "transitionrun width 0");
	f.clock.AdvanceTo(0.75);
	REQUIRE(log.log.size() == 2);
	CHECK(log.log[1] == "transitionstart width 0");
	f.clock.AdvanceTo(1.75);
	REQUIRE(log.log.size() == 3);
	CHECK(log.log[2] == "transitionend width 1");

	// Removing the property from the transition list cancels its running transition.
	f.b->SetProperty("width", "20px");
	f.Update();
	f.clock.AdvanceTo(2.0);
	f.b->SetProperty("transition", "none");
	f.Update();
	f.clock.AdvanceTo(2.1);
	REQUIRE(log.log.size() == 6);
	CHECK(log.log[3] == "transitionrun width 0");
	CHECK(log.log[4] == "transitionstart width 0");
	CHECK(log.log[5].substr(0, 22) == "transitioncancel width");
	CHECK(Width(f.b) == doctest::Approx(20.f));

	for (Element* element : {f.a, f.b})
		for (const char* type : {"transitionrun", "transitionstart", "transitionend", "transitioncancel"})
			element->RemoveEventListener(type, &log);
}

TEST_CASE("TransitionsAnyChange.writes_every_frame")
{
	Fixture f(R"(
		#a { width: 0px; transition: width 1s linear; }
		@keyframes grow { from { width: 0px; } to { width: 100px; } }
		#b { width: 0px; transition: width 1s linear; animation: grow 1s linear; }
	)");
	EventLog log;
	f.b->AddEventListener("transitionrun", &log);

	// A script writing the value every frame (as the 'anime' API does) restarts the transition each time, as in a
	// browser: the value follows the writes with the transition's lag.
	for (int i = 1; i <= 10; i++)
	{
		f.a->SetProperty("width", CreateString("%dpx", i * 10));
		f.clock.AdvanceTo(0.05 * i);
	}
	CHECK(Width(f.a) > 0.f);
	CHECK(Width(f.a) < 50.f);

	// The values an animation applies are not style changes: no transition starts.
	CHECK(Width(f.b) > 0.f);
	CHECK(log.log.empty());
	f.b->RemoveEventListener("transitionrun", &log);
}
