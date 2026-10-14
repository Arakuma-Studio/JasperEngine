#include "../../../Source/Core/ElementStyle.h"
#include "../Common/TestsInterface.h"
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

// Registered custom properties (fork, phase 5; docs/html-ui/phase-5-effects.md 3.11, FORK.md P5-4): '@property' as the
// compat layer registers it, StyleSheetSpecification::RegisterCustomProperty. A registered '--name' is a typed property:
// inherited or not, with an initial value, transitioned and animated, and var(--name) substitutes its computed value.
// Every Chromium value checked here was read from Chromium 153 (getComputedStyle) for the same CSS.

namespace {

const String document_head = R"(
<rml>
<head>
	<style>
		body { inset: 0; font-size: 10px; }
		div { display: block; height: 10px; }
)";
const String document_tail = R"(
	</style>
</head>
<body><div id="parent"><div id="child"><div id="grandchild"/></div></div><div id="other"/></body>
</rml>
)";

struct Fixture {
	Context* context = nullptr;
	TestsSystemInterface* system = nullptr;
	ElementDocument* document = nullptr;
	double t = 0.0;

	explicit Fixture(const String& style) : context(TestsShell::GetContext()), system(TestsShell::GetTestsSystemInterface())
	{
		system->SetManualTime(0.0);
		document = context->LoadDocumentFromMemory(document_head + style + document_tail);
		REQUIRE(document);
		document->Show();
		context->Update();
	}
	~Fixture()
	{
		document->Close();
		system->SetManualTime(0.0);
		TestsShell::ShutdownShell();
	}
	Element* Get(const String& id) { return document->GetElementById(id); }
	void AdvanceTo(double time)
	{
		while (t < time)
		{
			t = Math::Min(t + 0.05, time);
			system->SetManualTime(t);
			context->Update();
		}
	}
	float Width(const String& id) { return Get(id)->GetComputedValues().width().value; }
	String Value(const String& id, const String& name)
	{
		const Property* property = Get(id)->GetProperty(name);
		return property ? property->ToString() : String("<null>");
	}
};

PropertyId Register(const String& name, RegisteredPropertySyntax syntax, bool inherits, const String& initial)
{
	return StyleSheetSpecification::RegisterCustomProperty(name, syntax, inherits, initial);
}

} // namespace

TEST_CASE("registered_properties.registration")
{
	TestsShell::GetContext();
	const int free_before = StyleSheetSpecification::GetNumFreePropertyIds();
	CHECK(free_before > 16);

	const PropertyId id = Register("--reg-a", RegisteredPropertySyntax::Length, false, "3px");
	REQUIRE(id != PropertyId::Invalid);
	CHECK(StyleSheetSpecification::IsRegisteredCustomProperty(id));
	CHECK(StyleSheetSpecification::GetRegisteredCustomProperties().Contains(id));
	CHECK(StyleSheetSpecification::AnyRegisteredCustomProperties());
	CHECK(StyleSheetSpecification::GetNumFreePropertyIds() == free_before - 1);
	CHECK(StyleSheetSpecification::GetPropertyId("--reg-a") == id);
	const RegisteredCustomProperty* registration = StyleSheetSpecification::GetRegisteredCustomProperty(id);
	REQUIRE(registration);
	CHECK(registration->syntax == RegisteredPropertySyntax::Length);
	CHECK(registration->inherits == false);
	CHECK(registration->has_initial_value);
	CHECK(registration->initial_value == "3px");
	CHECK(StyleSheetSpecification::GetProperty(id)->GetDefaultValue()->ToString() == "3px");
	CHECK(!StyleSheetSpecification::GetProperty(id)->IsInherited());

	// A second registration of the name keeps the first (compat compares them and reports the conflict).
	CHECK(Register("--reg-a", RegisteredPropertySyntax::Color, true, "red") == id);
	CHECK(StyleSheetSpecification::GetRegisteredCustomProperty(id)->syntax == RegisteredPropertySyntax::Length);

	// Invalid registrations.
	CHECK(Register("reg-b", RegisteredPropertySyntax::Length, false, "3px") == PropertyId::Invalid);   // not a custom property name
	CHECK(Register("--", RegisteredPropertySyntax::Length, false, "3px") == PropertyId::Invalid);
	CHECK(Register("--reg-c", RegisteredPropertySyntax::Length, false, "red") == PropertyId::Invalid);  // initial value of another type
	CHECK(Register("--reg-c", RegisteredPropertySyntax::Length, false, "") == PropertyId::Invalid);     // none, not the universal syntax
	CHECK(Register("--reg-c", RegisteredPropertySyntax::Angle, false, "0") == PropertyId::Invalid);     // no unitless zero for an angle
	CHECK(Register("width", RegisteredPropertySyntax::Length, false, "3px") == PropertyId::Invalid);    // another property
	CHECK(StyleSheetSpecification::GetPropertyId("--reg-c") == PropertyId::Invalid);
	CHECK(!StyleSheetSpecification::IsRegisteredCustomProperty(PropertyId::Width));

	// The universal syntax without an initial value.
	const PropertyId any = Register("--reg-any", RegisteredPropertySyntax::Universal, false, "");
	REQUIRE(any != PropertyId::Invalid);
	CHECK(!StyleSheetSpecification::GetRegisteredCustomProperty(any)->has_initial_value);

	// Registrations end with RmlUi.
	TestsShell::ShutdownShell();
	TestsShell::GetContext();
	CHECK(StyleSheetSpecification::GetPropertyId("--reg-a") == PropertyId::Invalid);
	CHECK(!StyleSheetSpecification::AnyRegisteredCustomProperties());
	TestsShell::ShutdownShell();
}

TEST_CASE("registered_properties.parsing")
{
	TestsShell::GetContext();
	struct Case {
		RegisteredPropertySyntax syntax;
		const char* value;
		bool valid;
	};
	const Case cases[] = {
		{RegisteredPropertySyntax::Length, "10px", true},
		{RegisteredPropertySyntax::Length, "2em", true},
		{RegisteredPropertySyntax::Length, "0", true},
		{RegisteredPropertySyntax::Length, "calc(1px + 2em)", true},
		{RegisteredPropertySyntax::Length, "10%", false},
		{RegisteredPropertySyntax::Length, "1px 2px", false},
		{RegisteredPropertySyntax::Length, "1p2px", false},
		{RegisteredPropertySyntax::Length, "red", false},
		{RegisteredPropertySyntax::Number, "1.5", true},
		{RegisteredPropertySyntax::Number, "-2e3", true},
		{RegisteredPropertySyntax::Number, "1px", false},
		{RegisteredPropertySyntax::Integer, "3", true},
		{RegisteredPropertySyntax::Integer, "2.5", false},
		{RegisteredPropertySyntax::Integer, "calc(2.5)", true},
		{RegisteredPropertySyntax::Percentage, "50%", true},
		{RegisteredPropertySyntax::Percentage, "10px", false},
		{RegisteredPropertySyntax::LengthPercentage, "50%", true},
		{RegisteredPropertySyntax::LengthPercentage, "calc(10% + 2px)", true},
		{RegisteredPropertySyntax::Color, "#ff000080", true},
		{RegisteredPropertySyntax::Color, "rgb(10 20 30 / 50%)", true},
		{RegisteredPropertySyntax::Color, "currentcolor", true},
		{RegisteredPropertySyntax::Color, "10px", false},
		{RegisteredPropertySyntax::Angle, "90deg", true},
		{RegisteredPropertySyntax::Angle, "0.25turn", true},
		{RegisteredPropertySyntax::Angle, "0", false},
		{RegisteredPropertySyntax::Time, "250ms", true},
		{RegisteredPropertySyntax::Time, "2s", true},
		{RegisteredPropertySyntax::Time, "2", false},
		{RegisteredPropertySyntax::Universal, "foo bar(1, 2)", true},
	};
	int index = 0;
	for (const Case& c : cases)
	{
		const String name = "--parse-" + ToString(index++);
		const PropertyId id = Register(name, c.syntax, false, c.syntax == RegisteredPropertySyntax::Universal ? "" : "");
		// (no initial value is valid for the universal syntax only: register the others with one)
		const PropertyId typed = (id != PropertyId::Invalid ? id : Register(name, c.syntax, false, [&]() -> String {
			switch (c.syntax)
			{
			case RegisteredPropertySyntax::Color: return "red";
			case RegisteredPropertySyntax::Angle: return "0deg";
			case RegisteredPropertySyntax::Time: return "0s";
			case RegisteredPropertySyntax::Percentage: return "0%";
			default: return "0";
			}
		}()));
		REQUIRE(typed != PropertyId::Invalid);
		Property parsed;
		INFO(name << ": " << c.value);
		CHECK(StyleSheetSpecification::GetProperty(typed)->ParseValue(parsed, c.value) == c.valid);

		// Every value is a valid declaration: one that does not match is kept as written (invalid at computed-value time).
		PropertyDictionary dictionary;
		CHECK(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, c.value));
		const Property* declared = dictionary.GetProperty(typed);
		REQUIRE(declared);
		CHECK((declared->unit == Unit::VAR_EXPRESSION) == !c.valid);
		CHECK(dictionary.GetCustomProperties().empty());
	}
	TestsShell::ShutdownShell();
}

TEST_CASE("registered_properties.lookup")
{
	TestsShell::GetContext();
	Register("--len", RegisteredPropertySyntax::Length, false, "7px");
	Register("--ilen", RegisteredPropertySyntax::Length, true, "7px");
	Register("--col", RegisteredPropertySyntax::Color, false, "#ff0000");
	Register("--ang", RegisteredPropertySyntax::Angle, false, "0deg");
	Register("--num", RegisteredPropertySyntax::Number, false, "0");
	Register("--int", RegisteredPropertySyntax::Integer, false, "0");
	Register("--time", RegisteredPropertySyntax::Time, false, "0s");
	Register("--pct", RegisteredPropertySyntax::Percentage, false, "0%");
	Register("--star", RegisteredPropertySyntax::Universal, false, "");

	Fixture f(R"(
		#parent { --len: 2em; --ilen: 3em; font-size: 10px; width: var(--len); }
		#child { font-size: 20px; width: var(--len); height: var(--ilen); }
		#grandchild { width: var(--ilen); }
		#other { --col: rgb(0 0 255 / 50%); --ang: 0.25turn; --num: calc(1 / 4); --int: calc(2.5); --time: 250ms; --pct: 50%;
			--star: foo bar; }
	)");

	SUBCASE("non-inherited")
	{
		// Chromium: #parent --len 20px (2em of 10px); #child gets the initial 7px, not its parent's value.
		CHECK(f.Width("parent") == 20.f);
		CHECK(f.Width("child") == 7.f);
		CHECK(f.Value("parent", "--len") == "20px");
		CHECK(f.Value("child", "--len") == "7px");
	}
	SUBCASE("inherited")
	{
		// The computed value (px, against the declaring element's font size) is inherited: 30px everywhere below.
		CHECK(f.Get("child")->GetComputedValues().height().value == 30.f);
		CHECK(f.Width("grandchild") == 30.f);
		CHECK(f.Value("grandchild", "--ilen") == "30px");
	}
	SUBCASE("serialization")
	{
		Element* other = f.Get("other");
		other->SetProperty("--s", "var(--col) var(--ang) var(--num) var(--int) var(--time) var(--pct) var(--star)");
		f.context->Update();
		const Property* s = other->GetProperty("--s");
		REQUIRE(s);
		CHECK(s->Get<String>() == "#0000ff80 90deg 0.25 3 0.25s 50% foo bar");
		// The computed values: angles in deg, integers rounded.
		CHECK(f.Value("other", "--ang") == "90deg");
		CHECK(f.Value("other", "--int") == "3");
	}
	SUBCASE("universal without initial value")
	{
		f.Get("parent")->SetProperty("--s", "var(--star, fallback)");
		f.context->Update();
		CHECK(f.Get("parent")->GetProperty("--s")->Get<String>() == "fallback");
		f.Get("parent")->SetProperty("width", "var(--star)");
		f.context->Update();
		CHECK(f.Width("parent") == 0.f); // invalid at computed-value time: 'width' is unset (auto)
	}
}

TEST_CASE("registered_properties.invalid_at_computed_value_time")
{
	TestsShell::GetContext();
	Register("--len", RegisteredPropertySyntax::Length, false, "7px");
	Register("--ilen", RegisteredPropertySyntax::Length, true, "7px");
	Fixture f(R"(
		#parent { --len: 3px; --ilen: 3px; }
		#child { --len: 5px; --len: red; --wrong: red; --ilen: var(--wrong); width: var(--len); height: var(--ilen); }
	)");
	// Chromium: the last declaration wins at parse time ('red'), and is invalid at computed-value time: the non-inherited
	// property takes its initial value, the inherited one its parent's.
	CHECK(f.Width("child") == 7.f);
	CHECK(f.Get("child")->GetComputedValues().height().value == 3.f);
	CHECK(f.Value("child", "--len") == "7px");
	CHECK(f.Value("child", "--ilen") == "3px");

	// A valid value set later applies.
	f.Get("child")->SetProperty("--len", "12px");
	f.context->Update();
	CHECK(f.Width("child") == 12.f);
}

TEST_CASE("registered_properties.css_wide_keywords")
{
	TestsShell::GetContext();
	Register("--len", RegisteredPropertySyntax::Length, false, "5px");
	Fixture f(R"(
		#parent { --len: 3px; all: initial; display: block; }
		#child { --len: inherit; width: var(--len); }
		#grandchild { --len: initial; width: var(--len); }
	)");
	// 'all' leaves custom properties alone; 'inherit' takes the parent's value of a non-inherited property.
	CHECK(f.Value("parent", "--len") == "3px");
	CHECK(f.Width("child") == 3.f);
	CHECK(f.Width("grandchild") == 5.f);
}

TEST_CASE("registered_properties.transition")
{
	TestsShell::GetContext();
	Register("--len", RegisteredPropertySyntax::Length, false, "0px");
	Register("--col", RegisteredPropertySyntax::Color, false, "#ff0000");
	Register("--int", RegisteredPropertySyntax::Integer, false, "0");
	Fixture f(R"(
		#parent { --len: 0px; --col: #ff0000; --int: 0; width: var(--len); transition: --len 1s linear, --col 1s linear, --int 1s linear; }
		#parent.on { --len: 100px; --col: #0000ff00; --int: 5; }
		#child { width: var(--len); }
		#other { --len: 0px; width: var(--len); transition: all 1s linear; }
		#other.on { --len: 100px; }
	)");
	Element* parent = f.Get("parent");
	parent->SetClass("on", true);
	f.Get("other")->SetClass("on", true);
	f.context->Update();
	f.AdvanceTo(0.5);
	// Chromium at 50%: --len 50px, --col rgba(255, 0, 0, 0.5) (premultiplied interpolation), --int 3 (2.5 rounded up).
	CHECK(f.Width("parent") == doctest::Approx(50.f).epsilon(0.02));
	CHECK(f.Value("parent", "--len") == "50px");
	CHECK(f.Value("parent", "--int") == "3");
	const Colourb colour = parent->GetProperty("--col")->Get<Colourb>();
	CHECK(colour.red == 255);
	CHECK(colour.blue == 0);
	CHECK(int(colour.alpha) == doctest::Approx(128).epsilon(0.02));
	// Non-inherited: the child keeps its initial value.
	CHECK(f.Width("child") == 0.f);
	// 'transition: all' does not transition custom properties (Chromium): the change applies at once (and 'all' transitions
	// the width that reads it).
	CHECK(f.Value("other", "--len") == "100px");
	CHECK(f.Width("other") == doctest::Approx(50.f).epsilon(0.02));

	f.AdvanceTo(1.1);
	CHECK(f.Width("parent") == 100.f);
	CHECK(f.Value("parent", "--int") == "5");
}

TEST_CASE("registered_properties.inherited_transition")
{
	TestsShell::GetContext();
	Register("--ilen", RegisteredPropertySyntax::Length, true, "0px");
	Fixture f(R"(
		#parent { --ilen: 0px; transition: --ilen 1s linear; }
		#parent.on { --ilen: 100px; }
		#child { width: var(--ilen); transition: width 1s linear; }
		#grandchild { width: var(--ilen); }
	)");
	f.Get("parent")->SetClass("on", true);
	f.context->Update();
	f.AdvanceTo(0.5);
	// Chromium: the descendants follow the parent's transitioned value. (The child's own 'width' transition, of the same
	// timing, runs from the before-change to the after-change value of its declarations, phase 3's rule for inherited
	// values: the same values.)
	CHECK(f.Width("grandchild") == doctest::Approx(50.f).epsilon(0.02));
	CHECK(f.Width("child") == doctest::Approx(50.f).epsilon(0.02));
	f.AdvanceTo(1.1);
	CHECK(f.Width("grandchild") == 100.f);
	CHECK(f.Width("child") == 100.f);
}

TEST_CASE("registered_properties.keyframes")
{
	TestsShell::GetContext();
	Register("--ang", RegisteredPropertySyntax::Angle, false, "0deg");
	Register("--num", RegisteredPropertySyntax::Number, false, "0");
	Fixture f(R"(
		@keyframes spin { to { --ang: 360deg; } }
		@keyframes grow { from { --num: 0; } to { --num: 1; } }
		#parent { animation: spin 4s linear infinite; transform: rotate(var(--ang)); }
		#child { animation: grow 2s linear; opacity: var(--num); }
	)");
	f.AdvanceTo(1.0);
	CHECK(f.Value("parent", "--ang") == "90deg");
	CHECK(f.Get("child")->GetComputedValues().opacity() == doctest::Approx(0.5f).epsilon(0.01));
	f.AdvanceTo(5.0);
	CHECK(f.Value("parent", "--ang") == "90deg");
	const Property* transform = f.Get("parent")->GetProperty(PropertyId::Transform);
	REQUIRE(transform);
	CHECK(transform->ToString() == "rotate(90deg)");
}

TEST_CASE("registered_properties.var_dependencies")
{
	TestsShell::GetContext();
	Register("--a", RegisteredPropertySyntax::Length, true, "1px");
	Register("--b", RegisteredPropertySyntax::Length, true, "1px");
	Fixture f(R"(
		#parent { --a: 10px; --b: calc(var(--a) * 2); font-size: var(--size, 10px); }
		#child { width: var(--b); }
	)");
	CHECK(f.Width("child") == 20.f);
	f.Get("parent")->SetProperty("--a", "30px");
	f.context->Update();
	CHECK(f.Width("child") == 60.f);

	// A registered length in em follows the font size; a var() in font-size follows its variable.
	f.Get("parent")->SetProperty("--a", "1em");
	f.context->Update();
	CHECK(f.Width("child") == 20.f);
	f.Get("parent")->SetProperty("--size", "20px");
	f.context->Update();
	CHECK(f.Get("parent")->GetComputedValues().font_size() == 20.f);
	CHECK(f.Width("child") == 40.f);

}

TEST_CASE("registered_properties.cycle")
{
	TestsShell::GetContext();
	Register("--a", RegisteredPropertySyntax::Length, true, "1px");
	Register("--b", RegisteredPropertySyntax::Length, true, "1px");
	Register("--c", RegisteredPropertySyntax::Length, true, "1px");
	// (RmlUi reports each cycle it finds: at each resolution of a property of the cycle)
	TestsShell::SetNumExpectedWarnings(7);
	Fixture f(R"(
		#parent { --a: var(--b); --b: calc(var(--a) * 2); --c: calc(var(--a) + 5px); }
		#child { width: var(--b); height: var(--c); }
	)");
	// Chromium: --a and --b are in a cycle, invalid at computed-value time: 'unset', the initial 1px (inherited); --c is
	// not in it and reads --a's 1px.
	CHECK(f.Value("parent", "--a") == "1px");
	CHECK(f.Value("parent", "--b") == "1px");
	CHECK(f.Width("child") == 1.f);
	CHECK(f.Get("child")->GetComputedValues().height().value == 6.f);
	TestsShell::SetNumExpectedWarnings(0);
}

TEST_CASE("registered_properties.unregistered")
{
	TestsShell::GetContext();
	Fixture f(R"(
		#parent { --u: 0px; width: var(--u); transition: --u 1s linear; }
		#parent.on { --u: 100px; }
		#child { width: var(--u); }
	)");
	// An unregistered custom property inherits and changes discretely (no transition), as in Chromium.
	f.Get("parent")->SetClass("on", true);
	f.context->Update();
	f.AdvanceTo(0.5);
	CHECK(f.Width("parent") == 100.f);
	CHECK(f.Width("child") == 100.f);
}

TEST_CASE("registered_properties.universal_discrete")
{
	TestsShell::GetContext();
	Register("--pick", RegisteredPropertySyntax::Universal, false, "#e04040ff");
	Fixture f(R"(
		#parent { background-color: var(--pick); transition: --pick 1s linear allow-discrete; }
		#parent.blue { --pick: #4060e0ff; }
		#other { background-color: var(--pick); transition: --pick 1s linear; }
		#other.blue { --pick: #4060e0ff; }
	)");
	f.Get("parent")->SetClass("blue", true);
	f.Get("other")->SetClass("blue", true);
	f.context->Update();
	f.AdvanceTo(0.4);
	// Chromium: a '*' property transitions discretely with allow-discrete (the old value until half way), at once without
	CHECK(f.Value("parent", "--pick") == "#e04040ff");
	CHECK(f.Get("parent")->GetComputedValues().background_color() == Colourb(0xe0, 0x40, 0x40, 0xff));
	CHECK(f.Get("other")->GetComputedValues().background_color() == Colourb(0x40, 0x60, 0xe0, 0xff));
	f.AdvanceTo(0.6);
	CHECK(f.Value("parent", "--pick") == "#4060e0ff");
	CHECK(f.Get("parent")->GetComputedValues().background_color() == Colourb(0x40, 0x60, 0xe0, 0xff));
}

TEST_CASE("registered_properties.mixed_cycle_and_fallbacks")
{
	TestsShell::GetContext();
	Register("--r", RegisteredPropertySyntax::Length, false, "7px");
	// (RmlUi reports the cycles and the undefined variable)
	TestsShell::SetNumExpectedWarnings(4);
	Fixture f(R"(
		#parent { --u: var(--r); --r: var(--u); width: var(--u, 50px); height: var(--r); }
		#child { --v: var(--nothing); width: var(--v, 10px); }
	)");
	// Chromium: --u (unregistered) is in a cycle with --r: guaranteed-invalid, so the var() takes its fallback; --r
	// (registered) is invalid at computed-value time: its initial 7px. A custom property whose own var() fails is
	// guaranteed-invalid too.
	CHECK(f.Width("parent") == 50.f);
	CHECK(f.Get("parent")->GetComputedValues().height().value == 7.f);
	CHECK(f.Width("child") == 10.f);
	TestsShell::SetNumExpectedWarnings(0);
}
