#pragma once

#include "../../Include/RmlUi/Core/CallbackTexture.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/RenderInterface.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

struct BoxShadowRenderable;
class ElementBackgroundLayers;
class RenderManager;

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
	// Fork (S16-F2): the shape GetClipGeometry's geometry covers, relative to the element's border box (kept with the
	// geometry: a clipping element's shape is asked for by every element it clips, every frame).
	ClipShape GetClipShape(Element* element, BoxArea clip_area);

	// The size of a device pixel in the element's units, for its feathered edges.
	static float GetFeatherUnit(Element* element);

	// Fork (S16-F1): whether the background, border and box shadows were last generated as primitives, and when not
	// why (a static string; empty when the render interface draws no primitives).
	bool IsPaintedAsPrimitives() const { return painted_as_primitives; }

	// Fork (S16, culling): how far the background, border and box shadows reach outside the border box, once
	// generated (false while they are to be generated again: the reach is unknown).
	bool GetInkReach(float& reach) const
	{
		reach = ink_reach;
		return !background_dirty && !border_dirty;
	}
	const char* GetPaintReason() const { return paint_reason; }

private:
	enum class BackgroundType { BackgroundBorder, BoxShadowAndBackgroundBorder, ClipBorder, ClipPadding, ClipContent, BorderAbove, Outline, Count };
	struct Background {
		Geometry geometry;
		Texture texture;
		SharedPtr<BoxShadowRenderable> box_shadow_and_background_border;
		Vector<BoxPrimitive> primitives; // fork (S16-F1): drawn instead of the geometry
		ClipShape clip_shape;            // fork (S16-F2): the clip geometry's shape, while the geometry is valid
		bool clip_shape_valid = false;
	};
	static ClipShape ComputeClipShape(Element* element, BoxArea clip_area);
	// Renders the background's geometry, or its primitives.
	static void RenderBackground(RenderManager& render_manager, const Background& background, Vector2f offset);
	// Fork (S16-F1): why the element's boxes cannot be primitives (nullptr: they can).
	const char* GetPrimitiveObstacle(Element* element, const Array<ColourbPremultiplied, 4>& border_colors, float opacity, bool has_box_shadow);

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
	bool painted_as_primitives = false;
	const char* paint_reason = "";
	float ink_reach = 1.f;

	StableMap<BackgroundType, Background> backgrounds;
};

} // namespace Rml
