#include "../../../Source/Core/ElementStyle.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.10, FORK.md P3-11): the animation layers of ElementStyle,
// the values running transitions and animations apply over the declared ones without touching the inline style, in CSS
// cascade order: transitions, animations, declarations.

static const String animation_layers_rml = R"(
<rml>
<head>
	<style>
		div { display: block; width: 20px; }
		#parent { color: #f00; }
	</style>
</head>
<body><div id="parent"><div id="element" style="width: 10px"/><div id="child"/></div></body>
</rml>
)";

TEST_CASE("AnimationLayers")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(animation_layers_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* element = document->GetElementById("element");
	ElementStyle* style = element->GetStyle();
	auto width = [&]() { return element->GetComputedValues().width().value; };
	auto declared_width = [&]() { return style->GetLocalStyleProperties().GetProperty(PropertyId::Width)->ToString(); };

	CHECK(width() == 10.f);
	CHECK(style->GetAnimatedProperties() == nullptr);
	CHECK(style->GetTransitionProperties() == nullptr);

	SUBCASE("cascade order")
	{
		style->SetAnimatedProperty(PropertyId::Width, Property(30.f, Unit::PX));
		context->Update();
		CHECK(width() == 30.f);
		CHECK(element->GetProperty(PropertyId::Width)->ToString() == "30px");
		CHECK(declared_width() == "10px"); // the inline style is untouched

		style->SetTransitionProperty(PropertyId::Width, Property(40.f, Unit::PX));
		context->Update();
		CHECK(width() == 40.f);

		// a new inline value does not override the layers
		element->SetProperty("width", "15px");
		context->Update();
		CHECK(width() == 40.f);

		style->RemoveTransitionProperty(PropertyId::Width);
		context->Update();
		CHECK(width() == 30.f);
		style->RemoveAnimatedProperty(PropertyId::Width);
		context->Update();
		CHECK(width() == 15.f);
		REQUIRE(style->GetAnimatedProperties());
		CHECK(style->GetAnimatedProperties()->GetNumProperties() == 0);
	}

	SUBCASE("a property without a declaration")
	{
		style->SetAnimatedProperty(PropertyId::Height, Property(7.f, Unit::PX));
		context->Update();
		CHECK(element->GetComputedValues().height().value == 7.f);
		CHECK(element->GetBox().GetSize().y == 7.f);
		style->RemoveAnimatedProperty(PropertyId::Height);
		context->Update();
		CHECK(element->GetComputedValues().height().type == Style::Height::Auto);
	}

	SUBCASE("inherited by the children")
	{
		Element* parent = document->GetElementById("parent");
		Element* child = document->GetElementById("child");
		parent->GetStyle()->SetAnimatedProperty(PropertyId::Color, Property(Colourb(0, 0, 255), Unit::COLOUR));
		context->Update();
		CHECK(child->GetComputedValues().color() == Colourb(0, 0, 255));
		CHECK(child->GetProperty(PropertyId::Color)->Get<Colourb>() == Colourb(0, 0, 255));
		parent->GetStyle()->RemoveAnimatedProperty(PropertyId::Color);
		context->Update();
		CHECK(child->GetComputedValues().color() == Colourb(255, 0, 0));
	}

	SUBCASE("removing what is not there changes nothing")
	{
		style->RemoveTransitionProperty(PropertyId::Width);
		style->RemoveAnimatedProperty(PropertyId::Width);
		CHECK_FALSE(style->AnyPropertiesDirty());
	}

	document->Close();
	TestsShell::ShutdownShell();
}
