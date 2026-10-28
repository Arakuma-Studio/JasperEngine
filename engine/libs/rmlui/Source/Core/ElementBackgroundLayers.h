#pragma once

#include "../../Include/RmlUi/Core/CompiledFilterShader.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/RenderInterface.h"
#include "../../Include/RmlUi/Core/Texture.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "BackgroundLayerValues.h"

namespace Rml {

class Decorator;

// The tiles of one layer (CSS Backgrounds 3, 3.6-3.9): background-size (auto, cover, contain, lengths with the image's
// ratio), round (a whole number of tiles), the position in the positioning area, repeat / space / no-repeat, every tile
// that meets the clip rectangle, cropped to it, with the part of the image it shows (u, v: 0 to 1 over a tile).
struct LayerTileInput {
	Rectanglef positioning_area, clip;
	Vector2f intrinsic_size; // 0 in an axis: none
	LayerSize size;
	Vector2f resolved_size;  // the explicit size of 'size' in px (where not auto)
	LayerRepeat repeat_x = LayerRepeat::Repeat, repeat_y = LayerRepeat::Repeat;
	// The image can be sampled with repeat wrap along that axis: tiles repeated (or rounded) along it are one rectangle.
	bool wrap_x = false, wrap_y = false;
};
struct LayerTile {
	Rectanglef rect;
	Vector2f uv0, uv1; // beyond 0 to 1 along an axis whose tiles are one rectangle
};
struct LayerTiles {
	Vector2f tile_size;
	Vector<LayerTile> tiles;
	// The tiles along that axis are one rectangle (more than one tile met the clip): the texture must repeat there, as
	// Chromium's image shader does (SkTileMode::kRepeat where the destination leaves the tile, kClamp otherwise).
	bool wrapped_x = false, wrapped_y = false;
};
// 'resolve_position' gives the position along an axis for the free space (area - tile) of that axis.
LayerTiles ComputeLayerTiles(const LayerTileInput& input, const Function<float(int axis, float free_space)>& resolve_position);

/**
    The background (between the background colour and the border) or mask layers of an element (phase 3, p3-paint;
    docs/html-ui/phase-3-fork-patches.md 3.9.2): url() images as textured tiles, gradients as one shader over all of
    their tiles, other decorator functions painted over the layer's clip box. A clip box with rounded corners clips its
    layer through an anti-aliased mask.
 */
class ElementBackgroundLayers {
public:
	enum class Kind { Background, Mask };

	explicit ElementBackgroundLayers(Kind kind);
	~ElementBackgroundLayers();

	// The element has layers with an image of this kind.
	static bool HasLayers(Element* element, Kind kind);
	// The box the background colour is clipped to: the background-clip of the bottom layer.
	static BoxArea GetColourClip(Element* element);
	// The number of layers the element paints of an image list: its length, but for an interpolated (cross-faded) list no
	// more than the element's own value has (Chromium paints the layers of the element's style).
	static size_t GetLayerCount(Element* element, PropertyId image_id, const ImageLayerList& images);

	void Dirty() { dirty = true; }
	void Release();

	// Renders the layers, the last one first, at the element's place in the current render state. With a canvas (the
	// rectangle of the context, the document's canvas background), the layers are clipped to it instead of their clip
	// boxes and positioned in the boxes of 'area_element' (the root element, when the body's background is the canvas's).
	void Render(Element* element, const Rectanglef* canvas = nullptr, Element* area_element = nullptr);

private:
	// One image of a cross-fade: its tiles, their colour weighted.
	struct FadeImage {
		Geometry geometry;
		Texture texture;
		SharedPtr<void> image_owner;
	};
	struct Draw {
		Geometry geometry;
		Texture texture;
		CompiledShader shader;
		SharedPtr<const Decorator> decorator;
		uintptr_t decorator_data = 0;
		Geometry clip_mask; // rounded clip box: the anti-aliased shape the layer is masked with
		SharedPtr<void> image_owner; // the application's image (ImageSourceHandler)
		// A cross-fade (an interpolated background-image): the images added in a layer (BlendMode::Plus), as Chromium's
		// CrossfadeGeneratedImage draws them.
		Vector<FadeImage> fade;
		// Fork (S16-F5): a gradient image drawn as primitives (RenderInterface::SupportsGradientPrimitive) instead of the
		// geometry and, under a rounded clip box, the mask layer: their radii clip it.
		Vector<BoxPrimitive> primitives;
	};

	void Generate(Element* element, const Rectanglef* canvas, Element* area_element);
	void ReleaseDraws();

	Kind kind;
	bool dirty = true;
	Vector<Draw> draws;
	// What the geometry depends on beyond the properties: for fixed and local attachments and the canvas.
	Vector2f generated_offset, generated_scroll, generated_canvas;
	bool depends_on_offset = false;
};

} // namespace Rml
