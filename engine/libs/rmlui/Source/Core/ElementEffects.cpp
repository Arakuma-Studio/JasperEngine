#include "ElementEffects.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Decorator.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Filter.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "ElementBackgroundLayers.h"
#include "ElementMeta.h"
#include "ElementStyle.h"
#include "TransformState.h"

namespace Rml {

ElementEffects::ElementEffects(Element* _element) : element(_element) {}

ElementEffects::~ElementEffects()
{
	ReleaseEffects();
}

void ElementEffects::InstanceEffects()
{
	if (!effects_dirty)
		return;

	effects_dirty = false;
	effects_data_dirty = true;

	RMLUI_ZoneScopedC(0xB22222);
	ReleaseEffects();

	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
	{
		RMLUI_ERRORMSG("Decorators are being instanced before a render manager is available. Is this element attached to the document?");
		return;
	}

	const ComputedValues& computed = element->GetComputedValues();

	if (computed.has_decorator())
	{
		const StyleSheet* style_sheet = element->GetStyleSheet();
		if (!style_sheet)
			return;

		for (const auto id : {PropertyId::Decorator})
		{
			const Property* property = element->GetStyle()->GetLocalPropertyWithResolvedVariables(id);
			if (!property || property->unit != Unit::DECORATOR)
				continue;

			DecoratorsPtr decorators_ptr = property->Get<DecoratorsPtr>();
			if (!decorators_ptr)
				continue;

			PropertySource document_source("", 0, "");
			const PropertySource* source = property->source.get();

			if (!source)
			{
				if (ElementDocument* document = element->GetOwnerDocument())
				{
					document_source.path = document->GetSourceURL();
					source = &document_source;
				}
			}

			const DecoratorPtrList& decorator_list = style_sheet->InstanceDecorators(*render_manager, *decorators_ptr, source);
			RMLUI_ASSERT(decorator_list.empty() || decorator_list.size() == decorators_ptr->list.size());

			DecoratorEntryList& decorators_target = decorators;
			decorators_target.reserve(decorators_ptr->list.size());

			for (size_t i = 0; i < decorator_list.size() && i < decorators_ptr->list.size(); i++)
			{
				const SharedPtr<const Decorator>& decorator = decorator_list[i];
				if (decorator)
				{
					DecoratorEntry entry;
					entry.decorator_data = 0;
					entry.decorator = decorator;
					entry.paint_area = decorators_ptr->list[i].paint_area;
					if (entry.paint_area == BoxArea::Auto)
						entry.paint_area = BoxArea::Padding;

					RMLUI_ASSERT(entry.paint_area >= BoxArea::Border && entry.paint_area <= BoxArea::Content);
					decorators_target.push_back(std::move(entry));
				}
			}
		}
	}

	if (computed.has_filter() || computed.has_backdrop_filter())
	{
		// Fork (phase 5, P5-1): '-jfx-text-fill' is applied before the element's 'filter' list, so a filter set by another
		// rule never replaces it, and a drop-shadow() of 'filter' shadows the filled text.
		const PropertyId text_fill_id = StyleSheetSpecification::GetTextFillPropertyId();
		for (const auto id : {text_fill_id, PropertyId::Filter, PropertyId::BackdropFilter})
		{
			if (id == PropertyId::Invalid)
				continue;
			const Property* property = element->GetStyle()->GetLocalPropertyWithResolvedVariables(id);
			if (!property || property->unit != Unit::FILTER)
				continue;

			FiltersPtr filters_ptr = property->Get<FiltersPtr>();
			if (!filters_ptr)
				continue;

			FilterEntryList& list = (id == PropertyId::BackdropFilter ? backdrop_filters : filters);
			list.reserve(filters_ptr->list.size());

			for (const FilterDeclaration& declaration : filters_ptr->list)
			{
				SharedPtr<const Filter> filter = declaration.instancer->InstanceFilter(declaration.type, declaration.properties);
				if (filter)
				{
					list.push_back({std::move(filter), CompiledFilter{}});
				}
				else
				{
					const auto& source = property->source;
					Log::Message(Log::LT_WARNING, "Filter '%s' in '%s' could not be instanced, declared at %s:%d", declaration.type.c_str(),
						filters_ptr->value.c_str(), source ? source->path.c_str() : "", source ? source->line_number : -1);
				}
			}
		}
	}
}

void ElementEffects::ReloadEffectsData()
{
	if (effects_data_dirty)
	{
		effects_data_dirty = false;

		bool decorator_data_failed = false;
		for (DecoratorEntryList* list : {&decorators})
		{
			for (DecoratorEntry& decorator : *list)
			{
				const DecoratorDataHandle old_data = decorator.decorator_data;

				decorator.decorator_data = decorator.decorator->GenerateElementData(element, decorator.paint_area);
				if (!decorator.decorator_data)
					decorator_data_failed = true;

				// Release old element data after generating new data, so that the decorator can reuse any cache.
				if (old_data)
					decorator.decorator->ReleaseElementData(old_data);
			}
		}

		if (decorator_data_failed)
			Log::Message(Log::LT_WARNING, "Could not generate decorator element data: %s", element->GetAddress().c_str());

		bool filter_compile_failed = false;
		for (FilterEntryList* list : {&filters, &backdrop_filters})
		{
			for (FilterEntry& filter : *list)
			{
				filter.compiled = filter.filter->CompileFilter(element);
				if (!filter.compiled)
					filter_compile_failed = true;
			}
		}

		if (filter_compile_failed)
			Log::Message(Log::LT_WARNING, "Could not compile filter on element: %s", element->GetAddress().c_str());
	}
}

void ElementEffects::ReleaseEffects()
{
	for (DecoratorEntryList* list : {&decorators})
	{
		for (DecoratorEntry& decorator : *list)
		{
			if (decorator.decorator_data)
				decorator.decorator->ReleaseElementData(decorator.decorator_data);
		}
		list->clear();
	}

	filters.clear();
	backdrop_filters.clear();
}

static Rectanglef JoinValid(Rectanglef a, Rectanglef b)
{
	if (!a.Valid())
		return b;
	return b.Valid() ? a.Join(b) : a;
}

// The ink overflow of the element's subtree: its bounding box (with its box shadow, outline and filters) joined with
// those of its visible descendants, which a filter or a group opacity layer must not cut off (CSS: filters and opacity
// apply to the whole stacking context; 6.3 clipped them to the element's border box). Fork (P7-F2): a descendant's own
// filters spread its subtree's ink by their extent (a blur, a drop shadow), through its transform: a child's shadow
// beyond its filtered parent's box is part of the parent's filter region (it was cut off at the composite).
static void ExtendBySubtreeInkOverflow(Element* element, Rectanglef& region, int depth)
{
	const int num_children = element->GetNumChildren(true);
	for (int i = 0; i < num_children; i++)
	{
		Element* child = element->GetChild(i);
		const ComputedValues& computed = child->GetComputedValues();
		if (computed.display() == Style::Display::None)
			continue;
		Rectanglef child_region = Rectanglef::MakeInvalid();
		Rectanglef box;
		if (ElementUtilities::GetBoundingBox(box, child, BoxArea::Auto))
		{
			const float outline = computed.outline_width() + Math::Max(computed.outline_offset(), 0.f) +
				(computed.outline_style() == Style::BorderStyle::Auto ? 3.f : 0.f);
			if (outline > 0.f)
				box = box.Extend(Vector2f(outline));
			// An element without area paints nothing: a collapsed whitespace text element has no layout position (its box
			// sits at the origin), and joining it would stretch the region to the top-left of the context.
			if (box.Width() > 0.f && box.Height() > 0.f)
				child_region = box;
		}
		if (depth < 256)
			ExtendBySubtreeInkOverflow(child, child_region, depth + 1);
		if (computed.has_filter())
			ElementEffects::ExtendByFilterExtent(child, child_region);
		region = JoinValid(region, child_region);
	}
}

// The scale the element's transform applies to lengths in its plane (the larger of its axes' scales), 1 without one.
static float GetTransformScale(const Element* element)
{
	const TransformState* transform_state = element->GetTransformState();
	const Matrix4f* transform = (transform_state ? transform_state->GetTransform() : nullptr);
	if (!transform)
		return 1.f;
	const Vector4f x = (*transform) * Vector4f(1.f, 0.f, 0.f, 0.f);
	const Vector4f y = (*transform) * Vector4f(0.f, 1.f, 0.f, 0.f);
	const float scale = Math::Max(Vector2f(x.x, x.y).Magnitude(), Vector2f(y.x, y.y).Magnitude());
	return (scale > 0.f && scale < 1e6f) ? scale : 1.f;
}

float ElementEffects::ExtendByFilterExtent(Element* element, Rectanglef& region)
{
	return element->meta->effects.ExtendByFilterExtent(region);
}

float ElementEffects::ExtendByFilterExtent(Rectanglef& region)
{
	// (instanced in the update; again here when they changed since)
	InstanceEffects();
	if (filters.empty() || !region.Valid())
		return 0.f;

	// The extents the filters add on each side, in the element's own space (a filter applies before its transform).
	Rectanglef extended = region;
	for (const auto& filter : filters)
		filter.filter->ExtendInkOverflow(element, extended);
	const Vector2f top_left = Math::Max(region.p0 - extended.p0, Vector2f(0.f));
	const Vector2f bottom_right = Math::Max(extended.p1 - region.p1, Vector2f(0.f));
	float reach = Math::Max(Math::Max(top_left.x, top_left.y), Math::Max(bottom_right.x, bottom_right.y));

	const TransformState* transform_state = element->GetTransformState();
	if (transform_state && transform_state->GetTransform())
	{
		// Turned or scaled: the largest extent on every side, at the transform's scale.
		reach *= GetTransformScale(element);
		region = region.Extend(reach);
	}
	else
	{
		region = extended;
	}
	return reach;
}

void ElementEffects::RenderEffects(RenderStage render_stage)
{
	InstanceEffects();
	ReloadEffectsData();

	// Group opacity: an element with an opacity below 1 that is not drawn as a leaf (Element::GetDrawOpacity).
	const float opacity = element->GetComputedValues().opacity();
	const bool group = (opacity < 1.f && element->GetDrawOpacity() == 1.f);
	if (group && (!group_opacity || group_opacity_value != opacity) && render_stage == RenderStage::Enter)
	{
		if (RenderManager* render_manager = element->GetRenderManager())
		{
			group_opacity.Release();
			group_opacity = render_manager->CompileFilter("opacity", Dictionary{{"value", Variant(opacity)}});
			group_opacity_value = opacity;
		}
	}
	else if (!group && group_opacity)
	{
		group_opacity.Release();
	}
	// Masks: mask-image layers (ElementBackgroundLayers, phase 3 p3-paint).
	const bool has_mask = element->GetComputedValues().has_mask_image();
	const bool layer = (!filters.empty() || has_mask || (group && group_opacity));

	if (!decorators.empty())
	{
		if (render_stage == RenderStage::Decoration)
		{
			// Render the decorators attached to this element in its current state.
			// Render from back to front for correct render order.
			for (int i = (int)decorators.size() - 1; i >= 0; i--)
			{
				DecoratorEntry& decorator = decorators[i];
				if (decorator.decorator_data)
					decorator.decorator->RenderElement(element, decorator.decorator_data);
			}
		}
	}

	if (!layer && backdrop_filters.empty())
		return;

	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
		return;

	Rectanglei initial_scissor_region = render_manager->GetScissorRegion();

	// The region of the filters: the element's box with the ink overflow of its subtree and the extent of its filters.
	// 'reach': how far the filters move content (a blur's or a shadow's extent), the largest on any side.
	auto GetFilterRegion = [this](float& reach) {
		Rectanglef filter_region = Rectanglef::MakeInvalid();
		ElementUtilities::GetBoundingBox(filter_region, element, BoxArea::Auto);
		const ComputedValues& computed = element->GetComputedValues();
		const float outline = computed.outline_width() + Math::Max(computed.outline_offset(), 0.f) +
			(computed.outline_style() == Style::BorderStyle::Auto ? 3.f : 0.f);
		if (outline > 0.f)
			filter_region = filter_region.Extend(Vector2f(outline));
		ExtendBySubtreeInkOverflow(element, filter_region, 0);

		// The filter property may draw outside our normal clipping region due to ink overflow.
		reach = ExtendByFilterExtent(filter_region);

		Math::ExpandToPixelGrid(filter_region);
		return filter_region;
	};
	auto ApplyBackdropClippingRegion = [this, &render_manager]() {
		// The element's border box, clipped as the element is.
		ElementUtilities::SetClippingRegion(element, true);
		Rectanglef filter_region = Rectanglef::MakeInvalid();
		ElementUtilities::GetBoundingBox(filter_region, element, BoxArea::Border);
		Math::ExpandToPixelGrid(filter_region);
		Rectanglei scissor_region = Rectanglei(filter_region).IntersectIfValid(render_manager->GetScissorRegion());
		render_manager->SetScissorRegion(scissor_region);
	};
	// The region of the layer the element is drawn into (the viewport, or a filter's layer beyond it: fork, P7-F2).
	const Rectanglei parent_region = render_manager->GetLayerRegion();
	auto ApplyScissorRegionForBackdrop = [this, &render_manager, parent_region]() {
		// Set the scissor region for backdrop drawing: the element's border box, within the viewport (fork, P7-F2: within
		// the region of the layer holding the backdrop, the viewport at the top level). As in browsers, the filters read
		// nothing beyond it (a blur extends the content at the region's edges instead of reading the content next to the
		// element, or beyond the viewport: Chromium's backdrop is the viewport's, mirrored at its edges).
		Rectanglef filter_region = Rectanglef::MakeInvalid();
		ElementUtilities::GetBoundingBox(filter_region, element, BoxArea::Border);
		Math::ExpandToPixelGrid(filter_region);
		Rectanglei region(filter_region);
		region = region.IntersectIfValid(parent_region);
		render_manager->SetScissorRegion(region);
	};

	if (render_stage == RenderStage::Enter)
	{
		const LayerHandle backdrop_source_layer = render_manager->GetTopLayer();

		if (layer)
		{
			// Fork (P7-F1): the content is drawn into the layer unclipped by the clips established outside the element
			// (CSS Filter Effects 1: the filter applies to the element's content, the ancestors' clips to its result;
			// Chromium filters the unclipped content and clips the result when compositing it). Those clips are applied
			// to the composite (Exit). The content's window: those clips' scissor region extended by the filters'
			// reach (the content beyond a clip that a blur or a shadow moves into it), twice over: a blur's downsampled
			// passes clamp their reads at the window's edge, rounded to their coarser pixels. Without reach it is the
			// scissor region the content had before. The layer's window, which the filters read: the filter region
			// within the content's window.
			// Fork (P7-F2): the clips' scissor region within the region of the layer the result is composited into (the
			// viewport, or a filter's layer beyond it), where the result can show, also without a clip; the layer covers
			// its window wherever that lies, beyond the viewport too (RenderManager::PushLayer(region)): a blur at the
			// viewport's edge reads the content beyond it, a shadow is cast from content beyond it.
			float reach = 0.f;
			const Rectanglef filter_region = GetFilterRegion(reach);
			Rectanglei outer_scissor;
			ClipMaskGeometryList outer_clip_mask;
			const bool outer_scissored = ElementUtilities::GetRenderClippingRegion(element, *render_manager, outer_scissor, outer_clip_mask);
			const Rectanglei visible = (outer_scissored ? outer_scissor.Intersect(parent_region) : parent_region);
			const Rectanglei content_window = visible.Extend(Math::RoundUpToInteger(2.f * reach));
			layer_window = Rectanglei(filter_region);
			layer_window = (layer_window.Valid() ? layer_window.Intersect(content_window) : content_window);
			layer_clip_root = true;

			render_manager->PushLayer(layer_window);
			// (the layer may cover less: cropped by the render interface)
			layer_window = layer_window.Intersect(render_manager->GetLayerRegion());
			render_manager->PushClipRoot(element, content_window);
		}

		if (!backdrop_filters.empty())
		{
			const LayerHandle backdrop_destination_layer = render_manager->GetTopLayer();

			// @performance We strictly only need this temporary buffer when having to read from outside the element
			// boundaries, which currently only applies to blur and drop-shadow. Alternatively, we could avoid this
			// completely if we introduced a render interface API concept of different input and output clipping. That
			// is, we set a large input scissor to cover all input data, which can be used e.g. during blurring, and use
			// our small border-area-only clipping region for the composite layers output.
			ApplyScissorRegionForBackdrop();
			render_manager->PushLayer();
			const LayerHandle backdrop_temp_layer = render_manager->GetTopLayer();

			FilterHandleList filter_handles;
			for (auto& filter : backdrop_filters)
				filter.compiled.AddHandleTo(filter_handles);

			// Render the backdrop filters in the extended scissor region including any ink overflow.
			render_manager->CompositeLayers(backdrop_source_layer, backdrop_temp_layer, BlendMode::Blend, filter_handles);

			// Then composite the filter output to our destination while applying our clipping region, including any
			// border-radius (fork, P7-F1: into the element's own layer, its border box only, the clips outside the
			// element clip that layer's composite).
			ApplyBackdropClippingRegion();
			render_manager->CompositeLayers(backdrop_temp_layer, backdrop_destination_layer, BlendMode::Blend, {});
			render_manager->PopLayer();
			render_manager->SetScissorRegion(initial_scissor_region);
		}
	}
	else if (render_stage == RenderStage::Exit)
	{
		if (layer)
		{
			if (layer_clip_root)
			{
				render_manager->PopClipRoot();
				layer_clip_root = false;
			}

			// Fork (P7-F1): the filters read the layer's window; the composite is clipped by the clips established
			// outside the element: their clip mask, and their scissor region, as a rectangle of the clip mask when the
			// window extends beyond it (the filters read the content there, the composite draws nothing there).
			Rectanglei outer_scissor;
			ClipMaskGeometryList clip_mask_list;
			const bool outer_scissored = ElementUtilities::GetRenderClippingRegion(element, *render_manager, outer_scissor, clip_mask_list);
			const Rectanglei window = layer_window;
			bool force_clip_mask = false;
			if (outer_scissored && (!window.Valid() || window.Intersect(outer_scissor) != window))
			{
				const Vector2f size = Vector2f(outer_scissor.Size());
				if (clip_rect_manager != render_manager || clip_rect_size != size || !clip_rect_geometry)
				{
					Mesh mesh;
					MeshUtilities::GenerateQuad(mesh, Vector2f(0.f), size, ColourbPremultiplied(255));
					clip_rect_geometry = render_manager->MakeGeometry(std::move(mesh));
					clip_rect_manager = render_manager;
					clip_rect_size = size;
				}
				const ClipMaskOperation operation = (clip_mask_list.empty() ? ClipMaskOperation::Set : ClipMaskOperation::Intersect);
				clip_mask_list.push_back(ClipMaskGeometry{operation, &clip_rect_geometry, Vector2f(outer_scissor.p0), nullptr});
				// (the geometry may have been rebuilt in place: the render manager would take an equal list for the mask
				// last drawn into the stencil)
				force_clip_mask = true;
			}

			// The stencil of a clip mask is cleared within the scissor region: set first.
			if (window.Valid())
				render_manager->SetScissorRegion(window);
			else if (outer_scissored)
				render_manager->SetScissorRegion(outer_scissor);
			else
				render_manager->DisableScissorRegion();
			if (force_clip_mask)
				render_manager->DisableClipMask();
			render_manager->SetClipMask(std::move(clip_mask_list));

			CompiledFilter mask_image_filter;
			FilterHandleList filter_handles;
			filter_handles.reserve(filters.size() + (has_mask ? 1 : 0) + 1);

			for (auto& filter : filters)
				filter.compiled.AddHandleTo(filter_handles);

			if (has_mask)
			{
				render_manager->PushLayer();

				if (!mask_layers)
					mask_layers = MakeUnique<ElementBackgroundLayers>(ElementBackgroundLayers::Kind::Mask);
				mask_layers->Render(element);
				mask_image_filter = render_manager->SaveLayerAsMaskImage();
				mask_image_filter.AddHandleTo(filter_handles);
				render_manager->PopLayer();
			}

			// The group's opacity, in the same composite as the filters and the mask.
			if (group && group_opacity)
				group_opacity.AddHandleTo(filter_handles);

			render_manager->CompositeLayers(render_manager->GetTopLayer(), render_manager->GetNextLayer(), BlendMode::Blend, filter_handles);
			render_manager->PopLayer();
			render_manager->SetScissorRegion(initial_scissor_region);
		}
	}
}

void ElementEffects::DirtyEffects()
{
	effects_dirty = true;
	if (mask_layers)
		mask_layers->Dirty();
}

void ElementEffects::DirtyEffectsData()
{
	effects_data_dirty = true;
	if (mask_layers)
		mask_layers->Dirty();
}

} // namespace Rml
