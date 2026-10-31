#pragma once

#include "../../Include/RmlUi/Core/CompiledFilterShader.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Decorator;
class ElementBackgroundLayers;
class Element;
class Filter;
class RenderManager;

enum class RenderStage { Enter, Decoration, Exit };

/**
    Manages and renders an element's effects: decorators, filters, backdrop filters, and mask images.
 */

class ElementEffects {
public:
	ElementEffects(Element* element);
	~ElementEffects();

	void InstanceEffects();

	void RenderEffects(RenderStage render_stage);

	// False when RenderEffects has nothing to do at any stage: no effects to instance or reload, no decorators, filters,
	// backdrop filters, mask or group opacity. Element::Render tests it inline, as most elements have none of them.
	bool NeedsRenderEffects(float opacity, bool has_mask_image) const
	{
		return effects_dirty || effects_data_dirty || !decorators.empty() || !filters.empty() || !backdrop_filters.empty() || bool(group_opacity) ||
			opacity < 1.f || has_mask_image;
	}

	// Fork (P7-F2): extends 'region' (window coordinates: the ink of the element's subtree) by the extent of the element's
	// filters (a blur's and a shadow's spread), applied in its own space: scaled by its transform. Returns the largest
	// extent added on a side (the filters' reach), zero without filters.
	float ExtendByFilterExtent(Rectanglef& region);
	// The same for any element (its effects).
	static float ExtendByFilterExtent(Element* element, Rectanglef& region);

	// Mark effects as dirty and force them to reset themselves.
	void DirtyEffects();
	// Mark the element data of effects as dirty.
	void DirtyEffectsData();

private:
	// Releases existing element data of effects, and regenerates it.
	void ReloadEffectsData();
	// Releases all existing effects and their element data.
	void ReleaseEffects();

	struct DecoratorEntry {
		SharedPtr<const Decorator> decorator;
		DecoratorDataHandle decorator_data;
		BoxArea paint_area;
	};
	using DecoratorEntryList = Vector<DecoratorEntry>;

	struct FilterEntry {
		SharedPtr<const Filter> filter;
		CompiledFilter compiled;
	};
	using FilterEntryList = Vector<FilterEntry>;

	Element* element;

	// The list of decorators and filters used by this element.
	DecoratorEntryList decorators;
	UniquePtr<ElementBackgroundLayers> mask_layers; // mask-image layers
	FilterEntryList filters;
	FilterEntryList backdrop_filters;

	// Group opacity (phase 3, p3-paint): the element and its stacking context are drawn into a layer composited with
	// this filter, compiled for 'group_opacity_value'.
	CompiledFilter group_opacity;
	float group_opacity_value = 1.f;

	// Fork (P7-F1): the window of the element's layer, which the filters read (the filter region within the clips established
	// outside the element, extended by the filters' reach), set when the layer is pushed, and whether its clip root is pushed.
	Rectanglei layer_window = Rectanglei::MakeInvalid();
	bool layer_clip_root = false;
	// The rectangle of those clips' scissor region in the clip mask of the layer's composite, when the window extends
	// beyond it; built for 'clip_rect_manager' at 'clip_rect_size' (placed by the clip mask's offset).
	Geometry clip_rect_geometry;
	RenderManager* clip_rect_manager = nullptr;
	Vector2f clip_rect_size;

	// If set, a full reload is necessary.
	bool effects_dirty = false;
	// If set, element data of all decorators need to be regenerated.
	bool effects_data_dirty = false;
};

} // namespace Rml
