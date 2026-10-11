#include "../../../Source/Core/BackgroundLayerValues.h"
#include "../../../Source/Core/ElementBackgroundLayers.h"
#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-paint (docs/html-ui/phase-3-fork-patches.md 3.9.1, 3.9.2): the background and mask layer properties and
// their shorthands, the tiles of a layer (size, position, repeat, space, round, the snapped destination of a tile that is
// not repeated, repeated tiles as one span), interpolation of positions and sizes, gradient stops with currentcolor
// resolved when the layer is drawn, and the canvas background.

namespace {

String Parsed(const PropertyDictionary& dictionary, const String& name)
{
	const Property* property = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(name));
	return property ? property->ToString() : String("<none>");
}

bool Parse(PropertyDictionary& dictionary, const String& name, const String& value)
{
	dictionary = PropertyDictionary();
	return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
}

String Text(const Rectanglef& r)
{
	return CreateString("%g %g %g %g", r.Left(), r.Top(), r.Width(), r.Height());
}

String Text(Vector2f v)
{
	return CreateString("%g %g", v.x, v.y);
}

LayerTiles Tiles(Rectanglef area, Vector2f intrinsic, LayerRepeat repeat_x, LayerRepeat repeat_y, Vector2f position = Vector2f(0.f),
	LayerSize size = LayerSize(), Vector2f resolved_size = Vector2f(0.f), bool wrap = false)
{
	LayerTileInput input;
	input.positioning_area = area;
	input.clip = area;
	input.intrinsic_size = intrinsic;
	input.size = size;
	input.resolved_size = resolved_size;
	input.repeat_x = repeat_x;
	input.repeat_y = repeat_y;
	input.wrap_x = input.wrap_y = wrap;
	return ComputeLayerTiles(input, [&](int axis, float free_space) {
		// 'position' as fractions of the free space (percentages)
		return (axis == 0 ? position.x : position.y) * free_space;
	});
}

} // namespace

TEST_CASE("Backgrounds.shorthand")
{
	TestsShell::GetContext();
	PropertyDictionary d;

	// Every longhand of every layer; the colour only in the last one.
	REQUIRE(Parse(d, "background",
		"url(a.png) right 10px bottom 20px / 30px auto no-repeat fixed padding-box content-box, linear-gradient(#ff0000, #0000ff) #00ff00"));
	CHECK(Parsed(d, "background-image") == "url(a.png), linear-gradient(#ff0000, #0000ff)");
	CHECK(Parsed(d, "background-position-x") == "calc(100% - 10px), 0%");
	CHECK(Parsed(d, "background-position-y") == "calc(100% - 20px), 0%");
	CHECK(Parsed(d, "background-size") == "30px, auto"); // a second auto is not serialized
	CHECK(Parsed(d, "background-repeat") == "no-repeat, repeat");
	CHECK(Parsed(d, "background-attachment") == "fixed, scroll");
	CHECK(Parsed(d, "background-origin") == "padding-box, padding-box");
	CHECK(Parsed(d, "background-clip") == "content-box, border-box");
	CHECK(Parsed(d, "background-color") == "#00ff00");

	// One box keyword is both the origin and the clip.
	REQUIRE(Parse(d, "background", "url(a.png) content-box"));
	CHECK(Parsed(d, "background-origin") == "content-box");
	CHECK(Parsed(d, "background-clip") == "content-box");

	// The shorthand resets every longhand.
	REQUIRE(Parse(d, "background", "#ff0000"));
	CHECK(Parsed(d, "background-image") == "none");
	CHECK(Parsed(d, "background-position-x") == "0%");
	CHECK(Parsed(d, "background-position-y") == "0%");
	CHECK(Parsed(d, "background-size") == "auto");
	CHECK(Parsed(d, "background-repeat") == "repeat");
	CHECK(Parsed(d, "background-attachment") == "scroll");
	CHECK(Parsed(d, "background-origin") == "padding-box");
	CHECK(Parsed(d, "background-clip") == "border-box");
	CHECK(Parsed(d, "background-color") == "#ff0000");

	// A colour in another layer than the last, or a size without a position, is invalid.
	CHECK_FALSE(Parse(d, "background", "#ff0000 url(a.png), url(b.png)"));
	CHECK_FALSE(Parse(d, "background", "url(a.png) / 10px"));

	// background-position: one to four values per layer.
	REQUIRE(Parse(d, "background-position", "center, left 5px top, 25% 75%"));
	CHECK(Parsed(d, "background-position-x") == "50%, 5px, 25%");
	CHECK(Parsed(d, "background-position-y") == "50%, 0%, 75%");

	// Sizes and repeats.
	REQUIRE(Parse(d, "background-size", "cover, contain, 50% auto, 10px 20px"));
	CHECK(Parsed(d, "background-size") == "cover, contain, 50%, 10px 20px");
	REQUIRE(Parse(d, "background-repeat", "repeat-x, repeat-y, space round"));
	CHECK(Parsed(d, "background-repeat") == "repeat-x, repeat-y, space round");

	// The mask shorthand and its longhands.
	REQUIRE(Parse(d, "mask", "linear-gradient(#000000, transparent) center / 50% no-repeat"));
	CHECK(Parsed(d, "mask-image") == "linear-gradient(#000000, transparent)");
	CHECK(Parsed(d, "mask-position-x") == "50%");
	CHECK(Parsed(d, "mask-position-y") == "50%");
	CHECK(Parsed(d, "mask-size") == "50%");
	CHECK(Parsed(d, "mask-repeat") == "no-repeat");
	REQUIRE(Parse(d, "mask-image", "url(m.png), none"));
	CHECK(Parsed(d, "mask-image") == "url(m.png), none");

	// CSS image() and image-set() take their first url or string; RCSS's image() decorator (an unquoted source) stays a
	// decorator layer, as 6.3's decorator-syntax mask-image read it.
	REQUIRE(Parse(d, "background-image", "image(\"a.png\"), image-set(url(b.png) 1x, url(c.png) 2x)"));
	CHECK(Parsed(d, "background-image") == "image(\"a.png\"), image-set(url(b.png) 1x, url(c.png) 2x)");
	auto kinds = [&](const char* name) {
		String result;
		const Property* property = d.GetProperty(StyleSheetSpecification::GetPropertyId(name));
		if (!property || property->unit != Unit::STYLE_VALUE)
			return result;
		const auto& list = static_cast<const ImageLayerList&>(*property->value.GetReference<StyleValuePtr>());
		for (const ImageLayer& layer : list.layers)
			result += (layer.kind == ImageLayer::Kind::Url ? "url:" + layer.url : String(layer.kind == ImageLayer::Kind::Decorator ? "decorator" : "none")) + ";";
		return result;
	};
	CHECK(kinds("background-image") == "url:a.png;url:b.png;");
	REQUIRE(Parse(d, "mask-image", "image(../common/alpha50.png)"));
	CHECK(Parsed(d, "mask-image") == "image(../common/alpha50.png)");
	CHECK(kinds("mask-image") == "decorator;");
}

TEST_CASE("Backgrounds.tiles")
{
	const Rectanglef area = Rectanglef::FromPositionSize({0.f, 0.f}, {100.f, 50.f});

	// repeat: tiles over the whole area, the last ones cropped.
	LayerTiles t = Tiles(area, {20.f, 15.f}, LayerRepeat::Repeat, LayerRepeat::Repeat);
	CHECK(t.tiles.size() == 5 * 4);
	CHECK(Text(t.tile_size) == "20 15");
	CHECK(Text(t.tiles.back().rect) == "80 45 20 5");
	CHECK(Text(t.tiles.back().uv1) == CreateString("1 %g", 5.f / 15.f));

	// ... drawn as one span sampled with repeat wrap.
	t = Tiles(area, {20.f, 15.f}, LayerRepeat::Repeat, LayerRepeat::Repeat, {}, {}, {}, true);
	REQUIRE(t.tiles.size() == 1);
	CHECK((t.wrapped_x && t.wrapped_y));
	CHECK(Text(t.tiles[0].rect) == "0 0 100 50");
	CHECK(Text(t.tiles[0].uv1) == CreateString("5 %g", 50.f / 15.f));

	// One tile along an axis: no wrap there.
	t = Tiles(area, {20.f, 50.f}, LayerRepeat::Repeat, LayerRepeat::Repeat, {}, {}, {}, true);
	CHECK(t.wrapped_x);
	CHECK_FALSE(t.wrapped_y);

	// space: the first and last tiles at the edges, the rest spread between them; no-repeat across.
	t = Tiles(area, {30.f, 10.f}, LayerRepeat::Space, LayerRepeat::NoRepeat);
	REQUIRE(t.tiles.size() == 3);
	CHECK(t.tiles[0].rect.Left() == 0.f);
	CHECK(t.tiles[1].rect.Left() == 35.f);
	CHECK(t.tiles[2].rect.Left() == 70.f);

	// round: a whole number of tiles; with an auto height the ratio is kept.
	t = Tiles(area, {30.f, 15.f}, LayerRepeat::Round, LayerRepeat::Repeat);
	CHECK(t.tile_size.x == doctest::Approx(100.f / 3.f));
	CHECK(t.tile_size.y == doctest::Approx(50.f / 3.f));
	t = Tiles(area, {30.f, 15.f}, LayerRepeat::Round, LayerRepeat::Round);
	CHECK(t.tile_size.x == doctest::Approx(100.f / 3.f));
	CHECK(t.tile_size.y == doctest::Approx(50.f / 3.f));

	// cover and contain.
	LayerSize cover;
	cover.mode = LayerSize::Mode::Cover;
	t = Tiles(area, {40.f, 40.f}, LayerRepeat::NoRepeat, LayerRepeat::NoRepeat, {0.5f, 0.5f}, cover);
	CHECK(Text(t.tile_size) == "100 100");
	REQUIRE(t.tiles.size() == 1);
	CHECK(Text(t.tiles[0].rect) == "0 0 100 50");
	CHECK(Text(t.tiles[0].uv0) == "0 0.25");
	CHECK(Text(t.tiles[0].uv1) == "1 0.75");
	LayerSize contain;
	contain.mode = LayerSize::Mode::Contain;
	t = Tiles(area, {40.f, 40.f}, LayerRepeat::NoRepeat, LayerRepeat::NoRepeat, {0.5f, 0.5f}, contain);
	CHECK(Text(t.tile_size) == "50 50");
	CHECK(Text(t.tiles[0].rect) == "25 0 50 50");

	// A width alone keeps the image's ratio.
	LayerSize width;
	width.auto_x = false;
	t = Tiles(area, {40.f, 20.f}, LayerRepeat::NoRepeat, LayerRepeat::NoRepeat, {}, width, {10.f, 0.f});
	CHECK(Text(t.tile_size) == "10 5");

	// A tile that is not repeated, after the area's start: drawn from its start snapped to one tile further, snapped.
	t = Tiles(area, {20.3f, 10.f}, LayerRepeat::NoRepeat, LayerRepeat::NoRepeat, {10.4f / 79.7f, 0.f});
	REQUIRE(t.tiles.size() == 1);
	CHECK(t.tiles[0].rect.Left() == 10.f);
	CHECK(t.tiles[0].rect.Right() == 30.f);
	CHECK(Text(t.tiles[0].uv0) == "0 0");
	CHECK(Text(t.tiles[0].uv1) == "1 1");

	// ... before the area's start (larger than the area): the image keeps its fractional place under the area.
	LayerSize sized;
	sized.auto_x = sized.auto_y = false;
	t = Tiles(area, {40.f, 40.f}, LayerRepeat::NoRepeat, LayerRepeat::NoRepeat, {0.5f, 0.5f}, sized, {100.f, 62.5f});
	REQUIRE(t.tiles.size() == 1);
	CHECK(Text(t.tiles[0].rect) == "0 0 100 50");
	CHECK(t.tiles[0].uv0.y == doctest::Approx(6.25f / 62.5f));
	CHECK(t.tiles[0].uv1.y == doctest::Approx(56.25f / 62.5f));
}

TEST_CASE("Backgrounds.computed_and_drawn")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	TestsRenderInterface* render = TestsShell::GetTestsRenderInterface();

	const String rml = R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; }
		div { display: block; width: 100px; height: 50px; }
		#a { color: #ff0000; background: linear-gradient(currentcolor, #0000ff) 0 0 / 50px 50px; }
		#b { background: url(backgrounds-test-tile.png) 0 0 / 20px 20px; }
		#c { background: url(backgrounds-test-single.png) no-repeat; }
		#d { mask: linear-gradient(#000000, transparent); }
	</style>
</head>
<body><div id="a"/><div id="b"/><div id="c"/><div id="d"/></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	if (render)
		render->Reset();
	context->Update();
	context->Render();

	Element* a = document->GetElementById("a");
	CHECK(a->GetComputedValues().has_background_image());
	CHECK_FALSE(document->GetElementById("d")->GetComputedValues().has_background_image());

	if (render)
	{
		// A repeated url() image is loaded for repeat wrap, one that is not repeated as it is.
		bool wrapped = false, single = false;
		for (const String& source : render->GetLoadedTextureSources())
		{
			wrapped |= (source.find("backgrounds-test-tile.png?rmlui-wrap=repeat") != String::npos);
			single |= (source.find("backgrounds-test-single.png") != String::npos && source.find("rmlui-wrap") == String::npos);
		}
		String loaded;
		StringUtilities::JoinString(loaded, render->GetLoadedTextureSources(), '|');
		INFO(loaded);
		CHECK(wrapped);
		CHECK(single);

		// The gradient's currentcolor stop is the element's colour when the layer is drawn.
		auto first_stop_colour = [&]() {
			for (const auto& shader : render->GetCompiledShaders())
			{
				auto it = shader.parameters.find("color_stop_list");
				if (shader.name == "linear-gradient" && it != shader.parameters.end())
					return it->second.GetReference<ColorStopList>().front().color;
			}
			return ColourbPremultiplied(0, 0);
		};
		CHECK(first_stop_colour() == ColourbPremultiplied(255, 0, 0, 255));
		render->Reset();
		a->SetProperty("color", "#00ff00");
		context->Update();
		context->Render();
		CHECK(first_stop_colour() == ColourbPremultiplied(0, 255, 0, 255));
	}

	document->Close();
	context->Update();
}

TEST_CASE("Backgrounds.interpolation")
{
	Context* context = TestsShell::GetContext();
	TestsSystemInterface* system = TestsShell::GetTestsSystemInterface();
	REQUIRE(context);
	system->SetManualTime(0.0);

	const String rml = R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; }
		div { display: block; width: 100px; height: 50px; }
		#a { background: url(i.png) 0px 0px / 10px 10px no-repeat;
			transition: background-position 1s linear, background-size 1s linear, background-image 1s linear; }
		#a.on { background: url(j.png) 100% 20px / 30px 50% no-repeat; }
	</style>
</head>
<body><div id="a"/></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	Element* a = document->GetElementById("a");
	a->SetClass("on", true);
	context->Update();
	system->SetManualTime(0.5);
	context->Update();

	// A length and a percentage mix as a calc(); lengths interpolate; url() images cross-fade (Chromium).
	CHECK(a->GetProperty("background-position-x")->ToString() == "50%"); // calc(50% + 0px)
	CHECK(a->GetProperty("background-position-y")->ToString() == "10px");
	CHECK(a->GetProperty("background-size")->ToString() == "20px calc(5px + 25%)");
	CHECK(a->GetProperty("background-image")->ToString() == "cross-fade(url(i.png) 50%, url(j.png))");

	system->SetManualTime(1.5);
	context->Update();
	CHECK(a->GetProperty("background-position-x")->ToString() == "100%");
	CHECK(a->GetProperty("background-size")->ToString() == "30px 50%");
	CHECK(a->GetProperty("background-image")->ToString() == "url(j.png)");

	document->Close();
	context->Update();
	system->SetManualTime(0.0);
}

// background-image interpolates as Chromium 153 does: lists of url() images cross-fade image by image (CSS Images 4
// cross-fade(), the shorter list repeated to the lowest common multiple of the lengths, no more layers painted than the
// element's own value has; the images are added in a layer), gradients, 'none' and mask-image are discrete: no
// transition starts.
TEST_CASE("Backgrounds.cross_fade")
{
	Context* context = TestsShell::GetContext();
	TestsSystemInterface* system = TestsShell::GetTestsSystemInterface();
	TestsRenderInterface* render = TestsShell::GetTestsRenderInterface();
	REQUIRE(context);
	REQUIRE(render);
	system->SetManualTime(0.0);

	const String rml = R"(
<rml>
<head>
	<style>
		body { display: block; width: 400px; height: 300px; }
		div { display: block; width: 100px; height: 50px; transition: background-image 1s linear, mask-image 1s linear; }
		#single { background-image: url(i.png); }
		#single.on { background-image: url(j.png); }
		#lists { background-image: url(i.png), url(j.png); background-size: 10px 10px; background-repeat: no-repeat; }
		#lists.on { background-image: url(k.png); }
		#grown { background-image: url(i.png); }
		#grown.on { background-image: url(j.png), url(k.png); }
		#gradient { background-image: linear-gradient(red, blue); }
		#gradient.on { background-image: linear-gradient(blue, red); }
		#none { background-image: none; }
		#none.on { background-image: url(j.png); }
		#mask { mask-image: url(i.png); }
		#mask.on { mask-image: url(j.png); }
		#same { background-image: url(i.png); }
		#same.on { background-image: url(i.png), url(i.png); }
	</style>
</head>
<body><div id="single"/><div id="lists"/><div id="grown"/><div id="gradient"/><div id="none"/><div id="mask"/><div id="same"/></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();
	const char* ids[] = {"single", "lists", "grown", "gradient", "none", "mask", "same"};
	for (const char* id : ids)
		document->GetElementById(id)->SetClass("on", true);
	context->Update();
	system->SetManualTime(0.25);
	context->Update();

	auto image = [&](const char* id, const char* property = "background-image") { return document->GetElementById(id)->GetProperty(property)->ToString(); };
	CHECK(image("single") == "cross-fade(url(i.png) 75%, url(j.png))");
	// two to one: the lowest common multiple, 2
	CHECK(image("lists") == "cross-fade(url(i.png) 75%, url(k.png)), cross-fade(url(j.png) 75%, url(k.png))");
	CHECK(image("grown") == "cross-fade(url(i.png) 75%, url(j.png)), cross-fade(url(i.png) 75%, url(k.png))");
	CHECK(image("same") == "url(i.png), url(i.png)");
	// discrete: no transition, the new value at once
	CHECK(image("gradient") == "linear-gradient(blue, red)");
	CHECK(image("none") == "url(j.png)");
	CHECK(image("mask", "mask-image") == "url(j.png)");

	// The layers painted: no more than the element's own value has (Chromium paints the layers of the element's style).
	auto painted = [&](const char* id) {
		Element* element = document->GetElementById(id);
		const Property* property = element->GetProperty(PropertyId::BackgroundImage);
		const auto* list = rmlui_dynamic_cast<const ImageLayerList*>(property->value.GetReference<StyleValuePtr>().get());
		REQUIRE(list);
		return ElementBackgroundLayers::GetLayerCount(element, PropertyId::BackgroundImage, *list);
	};
	CHECK(painted("lists") == 1);
	CHECK(painted("grown") == 2);

	// Each cross-faded layer is drawn in a layer of its own, its second image added to the first.
	render->ResetCounters();
	context->Render();
	render->ResetCounters();
	CHECK(render->GetCountersFromPreviousReset().composite_layers_plus == 4); // single 1, lists 1, grown 2

	// A transition from the middle of one cross-fades the cross-fade: a weighted mean of three images.
	Element* single = document->GetElementById("single");
	single->SetProperty("background-image", "url(m.png)");
	context->Update();
	system->SetManualTime(0.75);
	context->Update();
	CHECK(image("single") == "cross-fade(url(i.png) 37.5%, url(j.png) 12.5%, url(m.png))");
	single->RemoveProperty("background-image");
	context->Update();

	system->SetManualTime(5.0);
	context->Update();
	CHECK(image("single") == "url(j.png)");
	CHECK(image("grown") == "url(j.png), url(k.png)");

	document->Close();
	context->Update();
	system->SetManualTime(0.0);
}

TEST_CASE("Backgrounds.canvas")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	const String rml = R"(
<rml>
<head>
	<style>
		body { display: block; width: 100px; height: 50px; }
		htmlbody { display: block; background: #ff0000; }
	</style>
</head>
<body><htmlbody id="body"/></body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	CHECK_FALSE(document->HasCanvasBackground());
	document->SetCanvasBackground(true, "htmlbody");
	CHECK(document->HasCanvasBackground());
	CHECK(document->GetCanvasBodyTag() == "htmlbody");
	document->Show();
	context->Update();
	context->Render();
	document->SetCanvasBackground(false, "");
	CHECK_FALSE(document->HasCanvasBackground());
	context->Update();
	context->Render();

	document->Close();
	context->Update();
}

// A callback texture generated with repeat wrap (an SVG background tile the application rasterizes, repeated as one quad)
// reaches RenderInterface::GenerateTextureWithWrap; without an axis repeated it is an ordinary GenerateTexture.
TEST_CASE("Backgrounds.wrapped_callback_texture")
{
	Context* context = TestsShell::GetContext();
	TestsRenderInterface* render = TestsShell::GetTestsRenderInterface();
	REQUIRE(context);
	REQUIRE(render);
	ElementDocument* document = context->LoadDocumentFromMemory("<rml><body/></rml>");
	REQUIRE(document);
	RenderManager* render_manager = document->GetRenderManager();
	REQUIRE(render_manager);

	const byte pixels[4 * 4] = {};
	auto make = [&](bool repeat_x, bool repeat_y) {
		return render_manager->MakeCallbackTexture([&pixels, repeat_x, repeat_y](const CallbackTextureInterface& texture_interface) {
			return texture_interface.GenerateTexture({pixels, sizeof(pixels)}, Vector2i(2, 2), repeat_x, repeat_y);
		});
	};
	render->ResetCounters();
	CallbackTexture plain = make(false, false);
	CallbackTexture repeated = make(true, false);
	CHECK(Texture(plain).GetDimensions() == Vector2i(2, 2));
	CHECK(Texture(repeated).GetDimensions() == Vector2i(2, 2));
	render->ResetCounters();
	CHECK(render->GetCountersFromPreviousReset().generate_texture == 2);
	CHECK(render->GetCountersFromPreviousReset().generate_texture_with_wrap == 1);

	plain.Release();
	repeated.Release();
	document->Close();
	context->Update();
}
