#include "../../../Source/Core/BackgroundLayerValues.h"
#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Decorator.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Filter.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Fork (phase 5, docs/html-ui/phase-5-effects.md): the hooks the engine's UI effects use.
// - P5-1: -jfx-text-fill, a filter list applied before the element's filter list (gradient text with effects).
// - P5-2: Decorator::PaintsOwnClip, a background layer drawn without a clip mask.
// - P5-3: DecoratorInstancer::InterpolatesAsImageLayer, background-image lists interpolating decorator layers.
// - P5-5: the region of a filter covers its subtree's ink overflow, not the empty boxes of collapsed whitespace.

namespace {

struct CompiledFill {
	String id;
	float value;
};
Vector<CompiledFill> compiled_fills;

struct FilterRegion {
	String id;
	Rectanglef region;
};
Vector<FilterRegion> filter_regions;

class FillFilter : public Filter {
public:
	explicit FillFilter(float value) : value(value) {}
	CompiledFilter CompileFilter(Element* element) const override
	{
		compiled_fills.push_back({element->GetId(), value});
		return element->GetRenderManager()->CompileFilter("FillFilter", {});
	}
	void ExtendInkOverflow(Element* element, Rectanglef& overflow) const override { filter_regions.push_back({element->GetId(), overflow}); }
	float value;
};

class FillFilterInstancer : public FilterInstancer {
public:
	FillFilterInstancer()
	{
		id = RegisterProperty("value", "0px").AddParser("length").GetId();
		RegisterShorthand("filter", "value", ShorthandType::FallThrough);
	}
	SharedPtr<Filter> InstanceFilter(const String& /*name*/, const PropertyDictionary& properties) override
	{
		const Property* p = properties.GetProperty(id);
		return p ? MakeShared<FillFilter>(p->Get<float>()) : nullptr;
	}

private:
	PropertyId id = {};
};

class LayerDecorator : public Decorator {
public:
	LayerDecorator(float value, bool paints_own_clip) : value(value), paints_own_clip(paints_own_clip) {}
	DecoratorDataHandle GenerateElementData(Element* /*element*/, BoxArea /*paint_area*/) const override { return 1; }
	void ReleaseElementData(DecoratorDataHandle /*element_data*/) const override {}
	void RenderElement(Element* /*element*/, DecoratorDataHandle /*element_data*/) const override {}
	bool PaintsOwnClip() const override { return paints_own_clip; }
	float value;
	bool paints_own_clip;
};

class LayerDecoratorInstancer : public DecoratorInstancer {
public:
	LayerDecoratorInstancer(bool paints_own_clip, bool interpolates) : paints_own_clip(paints_own_clip), interpolates(interpolates)
	{
		id = RegisterProperty("value", "0").AddParser("number").GetId();
		RegisterShorthand("decorator", "value", ShorthandType::FallThrough);
	}
	SharedPtr<Decorator> InstanceDecorator(const String& /*name*/, const PropertyDictionary& properties,
		const DecoratorInstancerInterface& /*instancer_interface*/) override
	{
		const Property* p = properties.GetProperty(id);
		return p ? MakeShared<LayerDecorator>(p->Get<float>(), paints_own_clip) : nullptr;
	}
	bool InterpolatesAsImageLayer() const override { return interpolates; }

private:
	PropertyId id = {};
	bool paints_own_clip, interpolates;
};

// the value of the first decorator layer of an element's background-image
float LayerValue(Element* element)
{
	const Property* property = element->GetProperty(PropertyId::BackgroundImage);
	if (!property || property->unit != Unit::STYLE_VALUE)
		return -1.f;
	const auto* list = rmlui_dynamic_cast<const ImageLayerList*>(property->value.GetReference<StyleValuePtr>().get());
	if (!list || !list->decorators || list->layers.empty() || list->layers[0].decorator_index < 0)
		return -1.f;
	const DecoratorDeclaration& declaration = list->decorators->list[list->layers[0].decorator_index];
	const auto& properties = declaration.properties.GetProperties();
	if (properties.empty())
		return -1.f;
	return properties.begin()->second.Get<float>();
}

} // namespace

TEST_CASE("EffectHooks.text_fill_before_filter")
{
	compiled_fills.clear();
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	FillFilterInstancer instancer;
	Factory::RegisterFilterInstancer("fill-test", &instancer);
	REQUIRE(StyleSheetSpecification::GetProperty("-jfx-text-fill"));

	ElementDocument* document = context->LoadDocumentFromMemory(R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; font-family: LatoLatin; }
		div { display: block; width: 100px; height: 50px; }
		#a { -jfx-text-fill: fill-test(1px); filter: fill-test(2px); }
		#b { -jfx-text-fill: fill-test(3px); }
		#c { filter: fill-test(4px); }
		#d { -jfx-text-fill: fill-test(5px) fill-test(6px); }
	</style>
</head>
<body><div id="a">text</div><div id="b">text</div><div id="c">text</div><div id="d">text</div></body>
</rml>
)");
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	auto values = [](const char* id) {
		Vector<float> out;
		for (const CompiledFill& fill : compiled_fills)
			if (fill.id == id)
				out.push_back(fill.value);
		return out;
	};
	// the text fill first, then the element's filter list; a text fill alone makes the element filtered
	CHECK(values("a") == Vector<float>{1.f, 2.f});
	CHECK(values("b") == Vector<float>{3.f});
	CHECK(values("c") == Vector<float>{4.f});
	CHECK(values("d") == Vector<float>{5.f, 6.f});

	// it cascades and is set like any property
	Element* c = document->GetElementById("c");
	c->SetProperty("-jfx-text-fill", "fill-test(7px)");
	compiled_fills.clear();
	TestsShell::RenderLoop();
	CHECK(values("c") == Vector<float>{7.f, 4.f});

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("EffectHooks.decorator_paints_own_clip")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	TestsRenderInterface* render = TestsShell::GetTestsRenderInterface();
	REQUIRE(render);
	LayerDecoratorInstancer clips_itself(true, false), clipped(false, false);
	Factory::RegisterDecoratorInstancer("clips-itself", &clips_itself);
	Factory::RegisterDecoratorInstancer("clipped", &clipped);

	auto clip_masks = [&](const char* image) {
		const String rml = CreateString(R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; }
		div { display: block; width: 100px; height: 50px; border-radius: 10px; background-image: %s; }
	</style>
</head>
<body><div/></body>
</rml>
)",
			image);
		ElementDocument* document = context->LoadDocumentFromMemory(rml);
		document->Show();
		context->Update();
		render->ResetCounters();
		context->Render();
		render->ResetCounters();
		// the mask is a layer of the rounded box's shape (SaveLayerAsMaskImage) around the layer drawn
		const size_t masks = render->GetCountersFromPreviousReset().push_layer;
		document->Close();
		context->Update();
		return masks;
	};
	// a rounded box clips a decorator layer with a mask layer, unless the decorator clips itself
	CHECK(clip_masks("clipped(1)") > 0);
	CHECK(clip_masks("clips-itself(1)") == 0);

	TestsShell::ShutdownShell();
}

TEST_CASE("EffectHooks.image_layer_interpolation")
{
	Context* context = TestsShell::GetContext();
	TestsSystemInterface* system = TestsShell::GetTestsSystemInterface();
	REQUIRE(context);
	system->SetManualTime(0.0);
	LayerDecoratorInstancer interpolates(true, true), discrete(true, false);
	Factory::RegisterDecoratorInstancer("fx-layer", &interpolates);
	Factory::RegisterDecoratorInstancer("plain-layer", &discrete);

	ElementDocument* document = context->LoadDocumentFromMemory(R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; }
		div { display: block; width: 100px; height: 50px; transition: background-image 1s linear; }
		#a { background-image: fx-layer(0); }
		#a.on { background-image: fx-layer(10); }
		#b { background-image: plain-layer(0); }
		#b.on { background-image: plain-layer(10); }
		#c { background-image: fx-layer(0), linear-gradient(red, blue); }
		#c.on { background-image: fx-layer(10), linear-gradient(red, blue); }
	</style>
</head>
<body><div id="a"/><div id="b"/><div id="c"/></body>
</rml>
)");
	REQUIRE(document);
	document->Show();
	context->Update();
	Element* a = document->GetElementById("a");
	Element* b = document->GetElementById("b");
	Element* c = document->GetElementById("c");
	for (Element* element : {a, b, c})
		element->SetClass("on", true);
	context->Update();
	system->SetManualTime(0.25);
	context->Update();
	const String quarter = a->GetProperty("background-image")->ToString();
	CHECK(LayerValue(a) == doctest::Approx(2.5f));
	system->SetManualTime(0.5);
	context->Update();
	// property by property; the layer's text follows the values (the element takes each frame's value)
	CHECK(LayerValue(a) == doctest::Approx(5.f));
	CHECK(a->GetProperty("background-image")->ToString() != quarter);
	CHECK(LayerValue(c) == doctest::Approx(5.f));
	// a decorator that does not opt in is discrete: no transition
	CHECK(LayerValue(b) == doctest::Approx(10.f));

	system->SetManualTime(1.5);
	context->Update();
	CHECK(LayerValue(a) == doctest::Approx(10.f));
	CHECK(a->GetProperty("background-image")->ToString() == "fx-layer(10)");

	document->Close();
	context->Update();
	system->SetManualTime(0.0);
	TestsShell::ShutdownShell();
}

TEST_CASE("EffectHooks.filter_region_of_the_subtree")
{
	filter_regions.clear();
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	FillFilterInstancer instancer;
	Factory::RegisterFilterInstancer("fill-test", &instancer);

	// whitespace between the children, a child outside the box, text
	ElementDocument* document = context->LoadDocumentFromMemory(R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; font-family: LatoLatin; }
		#f { position: absolute; left: 100px; top: 80px; width: 100px; height: 50px; filter: fill-test(1px); }
		.c { position: absolute; left: 10px; top: 10px; width: 20px; height: 20px; }
		#out { position: absolute; left: 90px; top: 40px; width: 30px; height: 30px; }
	</style>
</head>
<body><div id="f">
	<div class="c"></div>
	<div id="out"></div>
	text
</div></body>
</rml>
)");
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	REQUIRE(!filter_regions.empty());
	const Rectanglef region = filter_regions.back().region;
	CHECK(filter_regions.back().id == "f");
	// the box and the child beyond it (to 220 x 150), nothing toward the context's origin
	CHECK(region.Left() == doctest::Approx(100.f));
	CHECK(region.Top() == doctest::Approx(80.f));
	CHECK(region.Right() == doctest::Approx(220.f));
	CHECK(region.Bottom() == doctest::Approx(150.f));

	document->Close();
	TestsShell::ShutdownShell();
}
