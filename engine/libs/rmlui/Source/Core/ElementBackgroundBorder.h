#pragma once

#include "../../Include/RmlUi/Core/CallbackTexture.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

struct BoxShadowRenderable;
class ElementBackgroundLayers;

class ElementBackgroundBorder {
public:
	ElementBackgroundBorder();
	~ElementBackgroundBorder();
	void Render(Element* element);
	// The border of an element with decorators, drawn after them: CSS paints background images below the border.
	void RenderBorder(Element* element);
	// The outline, drawn after the content of the element's stacking context (Element::Render).
	void RenderOutline(Element* element);

	void DirtyBackground();
	void DirtyBorder();
	void DirtyOutline();

	Geometry* GetClipGeometry(Element* element, BoxArea clip_area);

	// The size of a device pixel in the element's units, for its feathered edges.
	static float GetFeatherUnit(Element* element);

private:
	enum class BackgroundType { BackgroundBorder, BoxShadowAndBackgroundBorder, ClipBorder, ClipPadding, ClipContent, BorderAbove, Outline, Count };
	struct Background {
		Geometry geometry;
		Texture texture;
		SharedPtr<BoxShadowRenderable> box_shadow_and_background_border;
	};

	Background* GetBackground(BackgroundType type);
	Background& GetOrCreateBackground(BackgroundType type);
	void EraseBackground(BackgroundType type);

	void GenerateGeometry(Element* element);
	// The canvas background, drawn by the document (phase 3, p3-paint): its source's colour and layers over the context.
	void RenderCanvas(Element* document);
	void GenerateOutline(Element* element);

	bool background_dirty = false;
	bool border_dirty = false;
	// The element's background is the canvas's (the document or the body): the canvas draws it, the element does not.
	bool background_on_canvas = false;
	UniquePtr<ElementBackgroundLayers> layers;        // background-image layers
	UniquePtr<ElementBackgroundLayers> canvas_layers; // the document: the canvas's layers
	Geometry canvas_colour;
	String canvas_signature;
	bool outline_dirty = true;
	bool border_above_decorators = false;   // the geometry draws the border in BorderAbove
	// The size of a device pixel in the element's units the geometry was feathered for: the inverse of the scale of the
	// element's transform, in steps (the geometry is generated again when the scale leaves its step).
	float feather_unit = 1.f;

	StableMap<BackgroundType, Background> backgrounds;
};

} // namespace Rml
