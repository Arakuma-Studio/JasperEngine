#include "../../../Source/Core/BackgroundLayerValues.h"
#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Decorator.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
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
// - P5-6: image-layer lists parsed once per text (a var() resolved the same way again shares the list).
// - P7-F1: the content of a filter's layer is clipped by the filtered element's own clips and its descendants' only, within
//   the layer's window; the clips of its ancestors are applied to the layer's composite.

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


// P7-F1: what an element drawn into a filter's layer is clipped by, read while it renders.
struct ClipProbe {
	String id;
	String clip_root;
	bool scissored;
	Rectanglei scissor;
	size_t clip_masks;
};
Vector<ClipProbe> clip_probes;

class ProbeDecorator : public Decorator {
public:
	DecoratorDataHandle GenerateElementData(Element* /*element*/, BoxArea /*paint_area*/) const override { return 1; }
	void ReleaseElementData(DecoratorDataHandle /*element_data*/) const override {}
	void RenderElement(Element* element, DecoratorDataHandle /*element_data*/) const override
	{
		RenderManager* render_manager = element->GetRenderManager();
		const Element* root = render_manager->GetClipRoot();
		const RenderState& state = render_manager->GetState();
		clip_probes.push_back({element->GetId(), root ? root->GetId() : String(), state.scissor_region.Valid(), state.scissor_region,
			state.clip_mask_list.size()});
	}
};

class ProbeDecoratorInstancer : public DecoratorInstancer {
public:
	ProbeDecoratorInstancer()
	{
		RegisterProperty("value", "0").AddParser("number");
		RegisterShorthand("decorator", "value", ShorthandType::FallThrough);
	}
	SharedPtr<Decorator> InstanceDecorator(const String& /*name*/, const PropertyDictionary& /*properties*/,
		const DecoratorInstancerInterface& /*instancer_interface*/) override
	{
		return MakeShared<ProbeDecorator>();
	}
};

const ClipProbe* FindProbe(const String& id)
{
	for (const ClipProbe& probe : clip_probes)
	{
		if (probe.id == id)
			return &probe;
	}
	return nullptr;
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

TEST_CASE("EffectHooks.image_layers_parsed_once_per_text")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	LayerDecoratorInstancer first(true, true);
	Factory::RegisterDecoratorInstancer("fx-layer", &first);

	// the var() of four elements resolves to the same text in each, every time it is resolved (style, geometry, clip)
	ElementDocument* document = context->LoadDocumentFromMemory(R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; --k: 3; }
		div { display: block; width: 100px; height: 50px; background-image: fx-layer(var(--k)); }
	</style>
</head>
<body><div id="a"/><div id="b"/><div id="c"/><div id="d"/></body>
</rml>
)");
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();
	Element* a = document->GetElementById("a");
	Element* d = document->GetElementById("d");
	CHECK(LayerValue(a) == doctest::Approx(3.f));

	const uint64_t hits_before = GetImageLayerParseCacheHits();
	document->SetProperty("--k", "7");
	context->Update();
	context->Render();
	CHECK(LayerValue(a) == doctest::Approx(7.f));
	CHECK(LayerValue(d) == doctest::Approx(7.f));
	// one parse of "fx-layer(7)", the other resolutions share it
	CHECK(GetImageLayerParseCacheHits() - hits_before >= 3);
	const Property* pa = a->GetProperty(PropertyId::BackgroundImage);
	const Property* pd = d->GetProperty(PropertyId::BackgroundImage);
	REQUIRE(pa);
	REQUIRE(pd);
	const StyleValuePtr list_a = pa->value.GetReference<StyleValuePtr>();
	const StyleValuePtr list_d = pd->value.GetReference<StyleValuePtr>();
	CHECK((list_a.get() == list_d.get()));

	// a decorator instancer registered again: its lists are parsed again, with it
	LayerDecoratorInstancer second(true, true);
	Factory::RegisterDecoratorInstancer("fx-layer", &second);
	document->SetProperty("--k", "3");
	context->Update();
	document->SetProperty("--k", "7");
	context->Update();
	const Property* pa2 = a->GetProperty(PropertyId::BackgroundImage);
	REQUIRE(pa2);
	const auto* list = rmlui_dynamic_cast<const ImageLayerList*>(pa2->value.GetReference<StyleValuePtr>().get());
	REQUIRE(list);
	REQUIRE(bool(list->decorators));
	REQUIRE(list->decorators->list.size() == 1);
	CHECK((list->decorators->list[0].instancer == &second));
	CHECK((pa2->value.GetReference<StyleValuePtr>().get() != list_a.get()));

	document->Close();
	context->Update();
	TestsShell::ShutdownShell();
}

TEST_CASE("EffectHooks.filter_layer_clip_root")
{
	clip_probes.clear();
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ProbeDecoratorInstancer instancer;
	Factory::RegisterDecoratorInstancer("clip-probe", &instancer);

	// #f (blurred) in a rounded overflow clip, with a child in its own overflow clip (#leaf), a child without one
	// (#plain); a sibling after #f (#after), outside the layer
	ElementDocument* document = context->LoadDocumentFromMemory(R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; }
		div { display: block; }
		#clip { position: absolute; left: 50px; top: 50px; width: 100px; height: 80px; overflow: hidden; border-radius: 10px; }
		#f { position: absolute; left: -20px; top: -20px; width: 140px; height: 120px; filter: blur(5px); }
		#inner { position: absolute; left: 10px; top: 10px; width: 50px; height: 40px; overflow: hidden; }
		#leaf, #plain, #after { position: absolute; left: 0px; top: 0px; width: 10px; height: 10px; background-image: clip-probe(1); }
		#plain { left: 100px; }
	</style>
</head>
<body><div id="clip"><div id="f"><div id="inner"><div id="leaf"/></div><div id="plain"/></div><div id="after"/></div></body>
</rml>
)");
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	// the content's window: #clip's scissor (50, 50)-(150, 130) extended by twice the blur's reach (3 sigma: 15 px), the
	// filter region (#f's box (30, 30)-(170, 150) and the blur's extent) is larger
	const Rectanglei window = Rectanglei::FromCorners({20, 20}, {180, 160});

	const ClipProbe* plain = FindProbe("plain");
	REQUIRE(plain);
	CHECK(plain->clip_root == "f");
	CHECK(plain->scissored);
	CHECK(plain->scissor == window);
	CHECK(plain->clip_masks == 0); // #clip's rounded mask clips the layer's composite

	const ClipProbe* leaf = FindProbe("leaf");
	REQUIRE(leaf);
	CHECK(leaf->clip_root == "f");
	CHECK(leaf->scissor == Rectanglei::FromCorners({40, 40}, {90, 80})); // #inner's clip, not cut by #clip's
	CHECK(leaf->clip_masks == 0);

	const ClipProbe* after = FindProbe("after");
	REQUIRE(after);
	CHECK(after->clip_root.empty());
	CHECK(after->scissor == Rectanglei::FromCorners({50, 50}, {150, 130}));
	CHECK(after->clip_masks == 1);
	CHECK(context->GetRenderManager().GetClipRoot() == nullptr);

	// GetClippingRegion: a clip root leaves out its ancestors' clips
	Element* leaf_element = document->GetElementById("leaf");
	Element* f = document->GetElementById("f");
	Rectanglei region;
	ClipMaskGeometryList masks;
	REQUIRE(ElementUtilities::GetClippingRegion(leaf_element, region, &masks, false, f));
	CHECK(region == Rectanglei::FromCorners({40, 40}, {90, 80}));
	CHECK(masks.empty());
	REQUIRE(ElementUtilities::GetClippingRegion(leaf_element, region, &masks));
	CHECK(region == Rectanglei::FromCorners({50, 50}, {90, 80}));
	CHECK(masks.size() == 1);
	masks.clear();
	CHECK_FALSE(ElementUtilities::GetClippingRegion(document->GetElementById("plain"), region, &masks, false, f));

	document->Close();
	TestsShell::ShutdownShell();
}
