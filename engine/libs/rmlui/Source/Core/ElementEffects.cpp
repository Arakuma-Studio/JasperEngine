#include "ElementEffects.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Decorator.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Filter.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "ElementBackgroundLayers.h"
#include "ElementStyle.h"

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

// The ink overflow of the element's subtree: its bounding box (with its box shadow, outline and filters) joined with
// those of its visible descendants, which a filter or a group opacity layer must not cut off (CSS: filters and opacity
// apply to the whole stacking context; 6.3 clipped them to the element's border box).
static void ExtendBySubtreeInkOverflow(Element* element, Rectanglef& region, int depth)
{
	const int num_children = element->GetNumChildren(true);
	for (int i = 0; i < num_children; i++)
	{
		Element* child = element->GetChild(i);
		if (child->GetComputedValues().display() == Style::Display::None)
			continue;
		Rectanglef child_region;
		if (ElementUtilities::GetBoundingBox(child_region, child, BoxArea::Auto))
		{
			const ComputedValues& computed = child->GetComputedValues();
			const float outline = computed.outline_width() + Math::Max(computed.outline_offset(), 0.f) +
				(computed.outline_style() == Style::BorderStyle::Auto ? 3.f : 0.f);
			if (outline > 0.f)
				child_region = child_region.Extend(Vector2f(outline));
			// An element without area paints nothing: a collapsed whitespace text element has no layout position (its box
			// sits at the origin), and joining it would stretch the region to the top-left of the context.
			if (child_region.Width() > 0.f && child_region.Height() > 0.f)
				region = region.Join(child_region);
		}
		if (depth < 256)
			ExtendBySubtreeInkOverflow(child, region, depth + 1);
	}
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

	auto ApplyClippingRegion = [this, &render_manager](PropertyId filter_id) {
		RMLUI_ASSERT(filter_id == PropertyId::Filter || filter_id == PropertyId::BackdropFilter);

		const bool force_clip_to_self_border_box = (filter_id == PropertyId::BackdropFilter);
		ElementUtilities::SetClippingRegion(element, force_clip_to_self_border_box);

		// Find the region being affected by the active filters and apply it as a scissor.
		Rectanglef filter_region = Rectanglef::MakeInvalid();
		ElementUtilities::GetBoundingBox(filter_region, element, force_clip_to_self_border_box ? BoxArea::Border : BoxArea::Auto);
		if (!force_clip_to_self_border_box)
		{
			const ComputedValues& computed = element->GetComputedValues();
			const float outline = computed.outline_width() + Math::Max(computed.outline_offset(), 0.f) +
				(computed.outline_style() == Style::BorderStyle::Auto ? 3.f : 0.f);
			if (outline > 0.f)
				filter_region = filter_region.Extend(Vector2f(outline));
			ExtendBySubtreeInkOverflow(element, filter_region, 0);
		}

		// The filter property may draw outside our normal clipping region due to ink overflow.
		if (filter_id == PropertyId::Filter)
		{
			for (const auto& filter : filters)
				filter.filter->ExtendInkOverflow(element, filter_region);
		}

		Math::ExpandToPixelGrid(filter_region);

		Rectanglei scissor_region = Rectanglei(filter_region).IntersectIfValid(render_manager->GetScissorRegion());
		render_manager->SetScissorRegion(scissor_region);
	};
	auto ApplyScissorRegionForBackdrop = [this, &render_manager]() {
		// Set the scissor region for backdrop drawing: the element's border box, within the viewport. As in browsers,
		// the filters read nothing beyond it (a blur extends the content at the region's edges instead of reading the
		// content next to the element, or the transparent target beyond the viewport).
		Rectanglef filter_region = Rectanglef::MakeInvalid();
		ElementUtilities::GetBoundingBox(filter_region, element, BoxArea::Border);
		Math::ExpandToPixelGrid(filter_region);
		Rectanglei region(filter_region);
		if (Context* context = element->GetContext())
			region = region.IntersectIfValid(Rectanglei::FromSize(context->GetDimensions()));
		render_manager->SetScissorRegion(region);
	};

	if (render_stage == RenderStage::Enter)
	{
		const LayerHandle backdrop_source_layer = render_manager->GetTopLayer();

		if (layer)
		{
			render_manager->PushLayer();
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

			// Then composite the filter output to our destination while applying our clipping region, including any border-radius.
			ApplyClippingRegion(PropertyId::BackdropFilter);
			render_manager->CompositeLayers(backdrop_temp_layer, backdrop_destination_layer, BlendMode::Blend, {});
			render_manager->PopLayer();
			render_manager->SetScissorRegion(initial_scissor_region);
		}
	}
	else if (render_stage == RenderStage::Exit)
	{
		if (layer)
		{
			ApplyClippingRegion(PropertyId::Filter);

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
