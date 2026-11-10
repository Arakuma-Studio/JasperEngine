#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/Dictionary.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementInstancer.h"
#include "../../Include/RmlUi/Core/ElementScroll.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/PropertiesIteratorView.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/TransformPrimitive.h"
#include "CalculationResolver.h"
#include "Clock.h"
#include "ComputeProperty.h"
#include "DataModel.h"
#include "ElementAnimation.h"
#include "ElementBackgroundBorder.h"
#include "ElementCascade.h"
#include "ElementDefinition.h"
#include "ElementEffects.h"
#include "ElementMeta.h"
#include "ElementStyle.h"
#include "WillChange.h"
#include "PseudoElements.h"
#include "EventDispatcher.h"
#include "EventSpecification.h"
#include "GeometryBackgroundBorder.h"
#include "Layout/LayoutDetails.h"
#include "Layout/LayoutEngine.h"
#include "PluginRegistry.h"
#include "Pool.h"
#include "PropertiesIterator.h"
#include "PropertyParserAnimation.h"
#include "StyleSheetNode.h"
#include "StyleSheetParser.h"
#include "TransformState.h"
#include "TransformUtilities.h"
#include "XMLParseTools.h"
#include <algorithm>
#include <climits>
#include <cmath>

namespace Rml {

// Fork (P7-L): the elements that are layout boundaries (SetLayoutBoundary), over every document: while there are none,
// DirtyLayout looks for no boundary among the ancestors.
static int num_layout_boundaries = 0;

// Determines how many levels up in the hierarchy the OnChildAdd and OnChildRemove are called (starting at the child itself)
static constexpr int ChildNotifyLevels = 2;

// Helper function to select scroll offset delta
static float GetScrollOffsetDelta(ScrollAlignment alignment, float begin_offset, float end_offset)
{
	switch (alignment)
	{
	case ScrollAlignment::Start: return begin_offset;
	case ScrollAlignment::Center: return (begin_offset + end_offset) / 2.0f;
	case ScrollAlignment::End: return end_offset;
	case ScrollAlignment::Nearest:
		if (begin_offset >= 0.f && end_offset <= 0.f)
			return 0.f; // Element is already visible, don't scroll
		else if (begin_offset < 0.f && end_offset < 0.f)
			return Math::Max(begin_offset, end_offset);
		else if (begin_offset > 0.f && end_offset > 0.f)
			return Math::Min(begin_offset, end_offset);
		return 0.f; // Shouldn't happen
	case ScrollAlignment::Adaptive:
		if (begin_offset >= 0.f && end_offset <= 0.f)
			return 0.f; // Element is already visible, don't scroll
		return (begin_offset + end_offset) / 2.0f;
	}
	return 0.f;
}

RMLUI_RTTI_Define(Element)

// The pseudo-element type of a tag: pseudo-elements are the only elements whose tag starts with "::".
static PseudoElementType PseudoElementTypeOfTag(const String& tag)
{
	if (tag.size() < 3 || tag[0] != ':' || tag[1] != ':')
		return PseudoElementType::None;
	if (tag == "::before")
		return PseudoElementType::Before;
	if (tag == "::after")
		return PseudoElementType::After;
	if (tag == "::marker")
		return PseudoElementType::Marker;
	return PseudoElementType::None;
}

// Whether a descendant of 'element' is 'position: fixed', or also 'absolute' with 'with_absolute' (their containing block may
// be 'element', or a box inside it).
static bool HasPositionedDescendant(Element* element, bool with_absolute)
{
	const int num_children = element->GetNumChildren(true);
	for (int i = 0; i < num_children; i++)
	{
		Element* child = element->GetChild(i);
		const Style::Position position = child->GetComputedValues().position();
		if (position == Style::Position::Fixed || (with_absolute && position == Style::Position::Absolute))
			return true;
		if (HasPositionedDescendant(child, with_absolute))
			return true;
	}
	return false;
}

Element::Element(const String& tag) :
	local_stacking_context(false), local_stacking_context_forced(false), stacking_context_dirty(false), computed_values_are_default_initialized(true),
	visible(true), offset_fixed(false), absolute_offset_dirty(true), rounded_main_padding_size_dirty(true), dirty_definition(false),
	dirty_child_definitions(false), dirty_definition_self(false), dirty_pseudo_elements(false), pseudo_element_type(PseudoElementTypeOfTag(tag)), has_pseudo_elements(false), dirty_animation(false), dirty_transition(false),
	dirty_transform(false), dirty_perspective(false), animation_displayed(true), animations_running(false), display_contents(false), fixed_containing_block(false), absolute_containing_block(false), line_clamp_hidden(false), draws_line_clamp_ellipsis(false), layout_boundary(false), layout_boundary_dirty(false), tag(tag),
	relative_offset_base(0, 0), relative_offset_position(0, 0), absolute_offset(0, 0), scroll_offset(0, 0)
{
	RMLUI_ASSERT(tag == StringUtilities::ToLower(tag));
	parent = nullptr;
	focus = nullptr;
	instancer = nullptr;
	owner_document = nullptr;
	offset_parent = nullptr;

	clip_area = BoxArea::Padding;

	baseline = 0.0f;

	num_non_dom_children = 0;

	z_index = 0;

	meta = ElementMetaPool::element_meta_pool->pool.AllocateAndConstruct(this);
	data_model = nullptr;
}

Element::~Element()
{
	RMLUI_ASSERT(parent == nullptr);

	PluginRegistry::NotifyElementDestroy(this);

	if (layout_boundary)
		num_layout_boundaries--;

	// A simplified version of RemoveChild() for destruction.
	for (ElementPtr& child : children)
	{
		Element* child_ancestor = child.get();
		for (int i = 0; i <= ChildNotifyLevels && child_ancestor; i++, child_ancestor = child_ancestor->GetParentNode())
			child_ancestor->OnChildRemove(child.get());

		child->SetParent(nullptr);
	}

	// Destroy meta objects before clearing children. In particular, ElementScroll may still refer to some of our children.
	ElementMetaPool::element_meta_pool->pool.DestroyAndDeallocate(meta);

	children.clear();
	num_non_dom_children = 0;
}

void Element::Update(float dp_ratio, Vector2f vp_dimensions)
{
#ifdef RMLUI_TRACY_PROFILING
	auto name = GetAddress(false, false);
	RMLUI_ZoneScoped;
	RMLUI_ZoneText(name.c_str(), name.size());
#endif

	OnUpdate();

	// The flags are tested here: most elements have no animations, and the calls are not inlined (their early returns
	// would cost a function prologue per element and update).
	if (dirty_transition)
		HandleTransitionProperty();
	if (dirty_animation)
		HandleAnimationProperty();
	if (!animations.empty())
		AdvanceAnimations();
	else
		animations_running = false;

	meta->scroll.Update();

	UpdateProperties(dp_ratio, vp_dimensions);
	const bool restyle = UpdateAnimationDisplay();

	// Do en extra pass over the animations and properties if the 'animation' property was just changed.
	if (dirty_animation)
	{
		HandleAnimationProperty();
		AdvanceAnimations();
		UpdateProperties(dp_ratio, vp_dimensions);
	}
	else if (restyle)
		UpdateProperties(dp_ratio, vp_dimensions);

	// ::before, ::after, ::marker (phase 3, cascade), made before the children update so they are updated with them.
	if (dirty_pseudo_elements)
	{
		dirty_pseudo_elements = false;
		PseudoElements::Update(*this);
	}

	meta->effects.InstanceEffects();

	for (size_t i = 0; i < children.size(); i++)
		children[i]->Update(dp_ratio, vp_dimensions);

	if (animations_running && IsVisible(true))
	{
		if (Context* ctx = GetContext())
			ctx->RequestNextUpdate(0);
	}
}

void Element::UpdateProperties(const float dp_ratio, const Vector2f vp_dimensions)
{
	UpdateDefinition();

	if (meta->style.AnyPropertiesDirty())
	{
		const ComputedValues* parent_values = parent ? &parent->GetComputedValues() : nullptr;
		const ComputedValues* document_values = owner_document ? &owner_document->GetComputedValues() : nullptr;

		// Compute values and clear dirty properties
		PropertyIdSet dirty_properties = meta->style.ComputeValues(meta->computed_values, parent_values, document_values,
			computed_values_are_default_initialized, dp_ratio, vp_dimensions);

		computed_values_are_default_initialized = false;

		// Computed values are just calculated and can safely be used in OnPropertyChange.
		// However, new properties set during this call will not be available until the next update loop.
		if (!dirty_properties.Empty())
			OnPropertyChange(dirty_properties);
	}
}

bool Element::GetPaintBounds(Rectanglef& out_bounds)
{
	// (a subclass may paint more in OnRender: unknown unless it says)
	if (GetClassIdentifier() != Element::GetStaticClassIdentifier() || GetNumBoxes() != 1)
		return false;
	float reach = 0.f;
	if (!meta->background_border.GetInkReach(reach))
		return false;
	const Vector2f position = GetAbsoluteOffset(BoxArea::Border);
	out_bounds = Rectanglef::FromPositionSize(position - Vector2f(reach), main_box.GetSize(BoxArea::Border) + Vector2f(2.f * reach));
	return true;
}

void Element::Render()
{
#ifdef RMLUI_TRACY_PROFILING
	auto name = GetAddress(false, false);
	RMLUI_ZoneScoped;
	RMLUI_ZoneText(name.c_str(), name.size());
#endif

	const ComputedValues& computed = meta->computed_values;
	ElementEffects& effects = meta->effects;

	// A transparent group draws nothing (it stays in the layout and can be hit).
	if (computed.opacity() <= 0.f)
		return;

	UpdateAbsoluteOffsetAndRenderBoxData();
	// (Only an opacity below 1 makes a leaf; the test is inline, as most elements are opaque.)
	if (computed.opacity() < 1.f || opacity_leaf)
		UpdateOpacityLeaf();

	if (stacking_context_dirty)
		BuildLocalStackingContext();

	// Fork (S16-F6): an element with nothing to paint but its own (no transform, effects, stacking children or ellipsis)
	// whose paint bounds lie wholly outside the scissor its clipping region sets (kept by SetClippingRegion for the other
	// elements of its key) is passed over before any state is set: a long scrolled list's hidden rows cost a few tests
	if (!transform_state && !dirty_transform && !dirty_perspective && stacking_context.empty() && !local_stacking_context &&
		!draws_line_clamp_ellipsis && computed.clip() == Style::Clip::Type::Auto && !effects.NeedsRenderEffects(computed.opacity(), computed.has_mask_image()))
	{
		RenderManager* render_manager = GetRenderManager();
		if (render_manager && render_manager->PaintShortcuts())
		{
			const RenderManager::ClipMemo* memo = render_manager->FindClipMemo(ElementUtilities::GetClipMemoKey(GetOffsetParent()));
			Rectanglef bounds;
			if (memo && memo->scissor && GetPaintBounds(bounds))
			{
				const Rectanglei& scissor = memo->region;
				if (bounds.Right() <= (float)scissor.Left() || bounds.Left() >= (float)scissor.Right() || bounds.Bottom() <= (float)scissor.Top() ||
					bounds.Top() >= (float)scissor.Bottom())
					return;
			}
		}
	}

	UpdateTransformState();
	ElementUtilities::ApplyTransform(*this);

	// The effects are asked at each stage (rendering may change them), and skipped when they have nothing to do.
	if (effects.NeedsRenderEffects(computed.opacity(), computed.has_mask_image()))
		effects.RenderEffects(RenderStage::Enter);

	// A hidden element (CSS visibility) paints nothing of its own; its visible descendants paint (phase 3, p3-paint).
	if (computed.visibility() == Style::Visibility::Visible && ElementUtilities::SetClippingRegion(this))
	{
		// Fork (S16-F4): a background, border and box shadow wholly outside the scissor region are not painted (a long
		// scrolled list draws its visible rows only); the decorators are inside the border box
		float reach = 0.f;
		bool outside = false;
		if (GetNumBoxes() == 1 && meta->background_border.GetInkReach(reach))
		{
			if (RenderManager* render_manager = GetRenderManager())
			{
				const Vector2f position = GetAbsoluteOffset(BoxArea::Border);
				const Vector2f size = main_box.GetSize(BoxArea::Border);
				outside = render_manager->IsOutsideScissor(Rectanglef::FromPositionSize(position - Vector2f(reach), size + Vector2f(2.f * reach)));
			}
		}
		if (!outside)
		{
			meta->background_border.Render(this);
			if (effects.NeedsRenderEffects(computed.opacity(), computed.has_mask_image()))
				effects.RenderEffects(RenderStage::Decoration);
			meta->background_border.RenderBorder(this);
		}

		{
			RMLUI_ZoneScopedNC("OnRender", 0x228B22);
			OnRender();
		}
	}

	RenderStackingContextChildren();

	ElementUtilities::ApplyTransform(*this);
	if (effects.NeedsRenderEffects(computed.opacity(), computed.has_mask_image()))
		effects.RenderEffects(RenderStage::Exit);
}

// Outlines are painted as Chromium paints them (CSS UI 4 leaves their place open): per paint layer (the stacking context,
// and each positioned element), after the layer's in-flow content and before its positioned descendants.
void Element::RenderStackingContextChildren()
{
	const auto has_outline = [](const Element* element) {
		return element->meta->computed_values.outline_style() != Style::BorderStyle::None &&
			element->meta->computed_values.visibility() == Style::Visibility::Visible;
	};

	// An element that is not a stacking context has no children here, and its outline is painted by its stacking context.
	const bool own_outline = (local_stacking_context && has_outline(this));
	// (nothing to paint: most elements; an ellipsis is only painted here with the element it follows in the list, or as
	// this element's own)
	if (stacking_context.empty() && !own_outline && !draws_line_clamp_ellipsis)
		return;
	bool any_outline = own_outline;
	for (const Element* element : stacking_context)
		any_outline |= has_outline(element);
	// The line-clamp ellipses its block containers draw after an atomic inline or a <br> (fork, P6-L1), each painted
	// after the element BuildLocalStackingContext found: in the inline content, where Chromium paints the line's items.
	const Vector<std::pair<int, Element*>>* ellipses = meta->line_clamp_ellipses.get();
	size_t next_ellipsis = 0;
	const auto paint_ellipses = [&](int index) {
		while (ellipses && next_ellipsis < ellipses->size() && (*ellipses)[next_ellipsis].first == index)
			LineClamp::RenderBlockEllipsis((*ellipses)[next_ellipsis++].second);
	};
	if (ellipses)
		paint_ellipses(-1);

	if (!any_outline)
	{
		if (!ellipses)
		{
			for (Element* element : stacking_context)
				element->Render();
			return;
		}
		for (int i = 0; i < (int)stacking_context.size(); i++)
		{
			stacking_context[i]->Render();
			paint_ellipses(i);
		}
		return;
	}

	const auto is_descendant = [](const Element* element, const Element* ancestor) {
		for (const Element* e = element->parent; e; e = e->parent)
		{
			if (e == ancestor)
				return true;
		}
		return false;
	};

	// Stacks shared by the nested calls (a stacking context inside renders its own above ours, and leaves them as it
	// found them).
	struct OutlineLayer {
		const Element* owner;
		size_t begin; // The layer's outlines in 'pending', from here on.
	};
	static thread_local Vector<Element*> pending;
	static thread_local Vector<OutlineLayer> layers;
	const size_t pending_base = pending.size();
	const size_t layers_base = layers.size();

	const auto flush = [](size_t begin) {
		// (Rendering an outline does not render elements: 'pending' stays as it is meanwhile.)
		for (size_t i = begin; i < pending.size(); i++)
			pending[i]->RenderOutline();
		pending.resize(begin);
	};

	layers.push_back({this, pending_base});
	if (own_outline)
		pending.push_back(this);

	for (int index = 0; index < (int)stacking_context.size(); index++)
	{
		Element* element = stacking_context[index];
		while (layers.size() > layers_base + 1 && !is_descendant(element, layers.back().owner))
		{
			flush(layers.back().begin);
			layers.pop_back();
		}

		const bool is_layer = (element->local_stacking_context || element->GetPosition() != Style::Position::Static);
		// The outlines of the layer the element is in are painted before it, unless it is painted below the layer's content.
		if (is_layer && !(element->local_stacking_context && element->z_index < 0.f))
			flush(layers.back().begin);

		element->Render();
		paint_ellipses(index);

		// A stacking context paints its own outline (in its Render).
		if (!element->local_stacking_context)
		{
			if (is_layer)
				layers.push_back({element, pending.size()});
			if (has_outline(element))
				pending.push_back(element);
		}
	}
	flush(pending_base);
	layers.resize(layers_base);
}

void Element::RenderOutline()
{
	ElementUtilities::ApplyTransform(*this);
	// Clipped as the element is by its ancestors; the outline itself lies outside of the element's own box.
	if (ElementUtilities::SetClippingRegion(this))
		meta->background_border.RenderOutline(this);
}

ElementPtr Element::Clone() const
{
	ElementPtr clone;

	if (instancer)
	{
		clone = instancer->InstanceElement(nullptr, GetTagName(), attributes);
		if (clone)
			clone->SetInstancer(instancer);
	}
	else
		clone = Factory::InstanceElement(nullptr, GetTagName(), GetTagName(), attributes);

	if (clone)
	{
		// Copy over the attributes. The 'style' and 'class' attributes are skipped because inline styles and class names are copied manually below.
		// This is necessary in case any properties or classes have been set manually, in which case the 'style' and 'class' attributes are out of
		// sync with the used style and active classes.
		ElementAttributes clone_attributes = attributes;
		clone_attributes.erase("style");
		clone_attributes.erase("class");
		clone->SetAttributes(clone_attributes);

		const PropertyDictionary& local_properties = GetStyle()->GetLocalStyleProperties();
		for (auto& id_property : local_properties.GetProperties())
			clone->SetProperty(id_property.first, id_property.second);

		for (auto& id_property : local_properties.GetCustomProperties())
			clone->SetProperty(id_property.first, id_property.second.ToString());

		for (auto& [shorthand_id, property] : local_properties.GetVarShorthands())
			clone->SetProperty(StyleSheetSpecification::GetShorthandName(shorthand_id), property.ToString());

		clone->GetStyle()->SetClassNames(GetStyle()->GetClassNames());

		String inner_rml;
		GetInnerRML(inner_rml);

		clone->SetInnerRML(inner_rml);
	}

	return clone;
}

void Element::SetClass(const String& class_name, bool activate)
{
	if (meta->style.SetClass(class_name, activate))
	{
		DirtyDefinition(DirtyNodes::SelfAndSiblings);
		ElementCascade::OnHasDependencyChange(this, ElementCascade::Change::Class, class_name);
	}
}

bool Element::IsClassSet(const String& class_name) const
{
	return meta->style.IsClassSet(class_name);
}

void Element::SetClassNames(const String& class_names)
{
	SetAttribute("class", class_names);
}

String Element::GetClassNames() const
{
	return meta->style.GetClassNames();
}

const StyleSheet* Element::GetStyleSheet() const
{
	if (ElementDocument* document = GetOwnerDocument())
		return document->GetStyleSheet();
	return nullptr;
}

String Element::GetAddress(bool include_pseudo_classes, bool include_parents) const
{
	// Add the tag name onto the address.
	String address(tag);

	// Add the ID if we have one.
	if (!id.empty())
	{
		address += "#";
		address += id;
	}

	String classes = meta->style.GetClassNames();
	if (!classes.empty())
	{
		classes = StringUtilities::Replace(classes, ' ', '.');
		address += ".";
		address += classes;
	}

	if (include_pseudo_classes)
	{
		const PseudoClassMap& pseudo_classes = meta->style.GetActivePseudoClasses();
		for (auto& pseudo_class : pseudo_classes)
		{
			address += ":";
			address += pseudo_class.first;
		}
	}

	if (include_parents && parent)
	{
		address += " < ";
		return address + parent->GetAddress(include_pseudo_classes, true);
	}
	else
		return address;
}

void Element::SetOffset(Vector2f offset, Element* _offset_parent, bool _offset_fixed)
{
	// If our offset has definitely changed, or any of our parenting has, then these are set and
	// updated based on our left / right / top / bottom properties.
	if (relative_offset_base != offset || offset_parent != _offset_parent || offset_fixed != _offset_fixed)
	{
		relative_offset_base = offset;
		offset_fixed = _offset_fixed;
		offset_parent = _offset_parent;
		UpdateOffset();
		DirtyAbsoluteOffset();
	}

	// Otherwise, our offset is updated in case left / right / top / bottom will have an impact on
	// our final position, and our children are dirtied if they do.
	else
	{
		const Vector2f old_base = relative_offset_base;
		const Vector2f old_position = relative_offset_position;

		UpdateOffset();

		if (old_base != relative_offset_base || old_position != relative_offset_position)
			DirtyAbsoluteOffset();
	}
}

Vector2f Element::GetRelativeOffset(BoxArea area)
{
	return relative_offset_base + relative_offset_position + GetBox().GetPosition(area);
}

Vector2f Element::GetAbsoluteOffset(BoxArea area)
{
	UpdateAbsoluteOffsetAndRenderBoxData();
	return area == BoxArea::Border ? absolute_offset : absolute_offset + GetBox().GetPosition(area);
}

// The shift of a sticky box whose border box, unshifted, is at 'border_position' (absolute coordinates).
static Vector2f ComputeStickyShift(Element* element, Vector2f border_position)
{
	Element* containing_element = element->GetOffsetParent();
	if (!containing_element)
		return Vector2f(0.f);

	// The nearest ancestor scroll container, or the document.
	Element* scroller = element->GetParentNode();
	while (scroller && scroller->GetParentNode())
	{
		const ComputedValues& scroller_computed = scroller->GetComputedValues();
		if (LayoutDetails::IsScrollContainer(scroller_computed.overflow_x(), scroller_computed.overflow_y()))
			break;
		scroller = scroller->GetParentNode();
	}
	if (!scroller)
		return Vector2f(0.f);

	const ComputedValues& computed = element->GetComputedValues();
	const Box& box = element->GetBox();
	const Vector2f border_size = box.GetSize(BoxArea::Border);

	// The scrollport, and in it the sticky view rectangle: inset by the insets that are not 'auto'.
	const Vector2f scrollport_position = scroller->GetAbsoluteOffset(BoxArea::Padding);
	const Vector2f scrollport_size = {scroller->GetClientWidth(), scroller->GetClientHeight()};

	// The box stays inside its containing block: the content box of its offset parent as its content is scrolled, all
	// of its scrollable content if it is a scroll container. The margins of the box stay inside too.
	const Box& containing_box = containing_element->GetBox();
	const Vector2f containing_scroll = {containing_element->GetScrollLeft(), containing_element->GetScrollTop()};
	const Vector2f containing_position = containing_element->GetAbsoluteOffset(BoxArea::Content) - containing_scroll;
	Vector2f containing_size = containing_box.GetSize();
	const ComputedValues& containing_computed = containing_element->GetComputedValues();
	if (LayoutDetails::IsScrollContainer(containing_computed.overflow_x(), containing_computed.overflow_y()))
	{
		const Vector2f padding = {containing_box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Padding, BoxArea::Padding),
			containing_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Padding, BoxArea::Padding)};
		containing_size = Math::Max(containing_size, Vector2f(containing_element->GetScrollWidth(), containing_element->GetScrollHeight()) - padding);
	}

	using Inset = Style::LengthPercentageAuto;
	auto shift_axis = [&](int axis, Inset start_inset, Inset end_inset, BoxEdge start_edge, BoxEdge end_edge) {
		const bool has_start = (start_inset.type != Inset::Auto), has_end = (end_inset.type != Inset::Auto);
		if (!has_start && !has_end)
			return 0.f;

		const float size = border_size[axis];
		const float position = border_position[axis];
		float view_start = scrollport_position[axis] + (has_start ? ResolveValue(start_inset, scrollport_size[axis]) : 0.f);
		float view_end = scrollport_position[axis] + scrollport_size[axis] - (has_end ? ResolveValue(end_inset, scrollport_size[axis]) : 0.f);
		// A view rectangle smaller than the box gives way at its end.
		if (has_start && has_end && view_end - view_start < size)
			view_end = view_start + size;

		const float limit_start = containing_position[axis] + box.GetEdge(BoxArea::Margin, start_edge);
		const float limit_end = containing_position[axis] + containing_size[axis] - box.GetEdge(BoxArea::Margin, end_edge);

		float shifted = position;
		// Moved towards the end until its start is at the view's start, or its end margin edge at the containing block's end.
		if (has_start && shifted < view_start)
			shifted = Math::Max(Math::Min(view_start, limit_end - size), position);
		// Moved towards the start until its end is at the view's end, or its start margin edge at the containing block's start.
		if (has_end && shifted + size > view_end)
			shifted = Math::Min(Math::Max(view_end - size, limit_start), shifted);
		return shifted - position;
	};

	return Vector2f(shift_axis(0, computed.left(), computed.right(), BoxEdge::Left, BoxEdge::Right),
		shift_axis(1, computed.top(), computed.bottom(), BoxEdge::Top, BoxEdge::Bottom));
}

void Element::UpdateDirtyAbsoluteOffsetAndRenderBoxData()
{
	absolute_offset_dirty = false;
	rounded_main_padding_size_dirty = false;

	Vector2f offset_from_ancestors;
	if (offset_parent)
		offset_from_ancestors = offset_parent->GetAbsoluteOffset(BoxArea::Border);

	if (!offset_fixed)
	{
		// Add any parent scrolling onto our position as well.
		if (offset_parent)
			offset_from_ancestors -= offset_parent->scroll_offset;

		// Finally, there may be relatively positioned elements between ourself and our containing block, add their relative offsets as well.
		for (Element* ancestor = parent; ancestor && ancestor != offset_parent; ancestor = ancestor->parent)
		{
			// (the shift of a sticky one is found with its absolute offset)
			if (ancestor->absolute_offset_dirty && ancestor->GetPosition() == Style::Position::Sticky)
				ancestor->UpdateAbsoluteOffsetAndRenderBoxData();
			offset_from_ancestors += ancestor->relative_offset_position;
		}
	}

	// 'position: sticky': the shift that keeps the box in the sticky view rectangle of its scroll container, for the
	// current scroll offsets (scrolling dirties the absolute offsets below the scroll container).
	if (meta->computed_values.position() == Style::Position::Sticky)
		relative_offset_position = ComputeStickyShift(this, relative_offset_base + offset_from_ancestors);

	const Vector2f relative_offset = relative_offset_base + relative_offset_position;
	absolute_offset = relative_offset + offset_from_ancestors;

	// Next, we find the rounded size of the box so that elements can be placed border-to-border next to each other
	// without any gaps. To achieve this, we have to adjust their rounded/rendered sizes based on their position, in
	// such a way that the bottom-right of this element exactly matches the top-left of the next element. The order
	// of floating-point operations matter here, we want to replicate the operations in the layout engine as close
	// as possible to avoid any gaps.
	const Vector2f main_padding_size = main_box.GetSize(BoxArea::Padding);
	const Vector2f bottom_right_absolute_offset = (relative_offset + main_padding_size) + offset_from_ancestors;
	const Vector2f new_rounded_main_padding_size = bottom_right_absolute_offset.Round() - absolute_offset.Round();
	if (new_rounded_main_padding_size != rounded_main_padding_size)
	{
		rounded_main_padding_size = new_rounded_main_padding_size;
		meta->background_border.DirtyBackground();
		meta->background_border.DirtyBorder();
		meta->effects.DirtyEffectsData();
	}
}

void Element::SetClipArea(BoxArea _clip_area)
{
	clip_area = _clip_area;
}

BoxArea Element::GetClipArea() const
{
	return clip_area;
}

void Element::SetScrollableOverflowRectangle(Vector2f _scrollable_overflow_rectangle, bool clamp_scroll_offset)
{
	if (scrollable_overflow_rectangle != _scrollable_overflow_rectangle)
	{
		scrollable_overflow_rectangle = _scrollable_overflow_rectangle;
		if (clamp_scroll_offset)
			ClampScrollOffset();
	}
}

void Element::SetBox(const Box& box)
{
	// Placed again: an atomic inline the line-clamp ellipsis truncates is hidden once more by its line (fork, P6-L1).
	if (line_clamp_hidden)
	{
		line_clamp_hidden = false;
		DirtyStackingContext();
	}

	if (box != main_box || additional_boxes.size() > 0)
	{
#ifdef RMLUI_DEBUG
		for (const BoxEdge edge : {BoxEdge::Top, BoxEdge::Right, BoxEdge::Bottom, BoxEdge::Left})
		{
			const float border_width = box.GetEdge(BoxArea::Border, edge);
			if (border_width != Math::Round(border_width))
				Log::Message(Log::LT_WARNING, "Expected integer border width but got %g px on element: %s", border_width, GetAddress().c_str());
		}
#endif

		main_box = box;
		additional_boxes.clear();

		OnResize();
		rounded_main_padding_size_dirty = true;
		meta->background_border.DirtyBackground();
		meta->background_border.DirtyBorder();
		meta->effects.DirtyEffectsData();

		if (transform_state)
			DirtyTransformState(true, true);
	}
}

void Element::AddBox(const Box& box, Vector2f offset)
{
	additional_boxes.emplace_back(PositionedBox{box, offset});
	OnResize();
	meta->background_border.DirtyBackground();
	meta->background_border.DirtyBorder();
	meta->effects.DirtyEffectsData();
}

const Box& Element::GetBox()
{
	return main_box;
}

const Box& Element::GetBox(int index, Vector2f& offset)
{
	offset = Vector2f(0);

	const int additional_box_index = index - 1;
	if (index < 1 || additional_box_index >= (int)additional_boxes.size())
		return main_box;

	offset = additional_boxes[additional_box_index].offset;
	return additional_boxes[additional_box_index].box;
}

RenderBox Element::GetRenderBox(BoxArea fill_area, int index, bool with_border_top_cut)
{
	RMLUI_ASSERTMSG(fill_area >= BoxArea::Border && fill_area <= BoxArea::Content,
		"Render box can only be generated with fill area of border, padding or content.");

	UpdateAbsoluteOffsetAndRenderBoxData();

	struct BoxReference {
		const Box& box;
		Vector2f padding_size;
		Vector2f offset;
	};
	auto GetBoxAndOffset = [this, index]() {
		const int additional_box_index = index - 1;
		if (index < 1 || additional_box_index >= (int)additional_boxes.size())
			return BoxReference{main_box, rounded_main_padding_size, {}};
		const PositionedBox& positioned_box = additional_boxes[additional_box_index];
		return BoxReference{positioned_box.box, positioned_box.box.GetSize(BoxArea::Padding), positioned_box.offset.Round()};
	};

	BoxReference box = GetBoxAndOffset();

	EdgeSizes edge_sizes = {};
	for (int area = (int)BoxArea::Border; area < (int)fill_area; area++)
	{
		edge_sizes[0] += box.box.GetEdge(BoxArea(area), BoxEdge::Top);
		edge_sizes[1] += box.box.GetEdge(BoxArea(area), BoxEdge::Right);
		edge_sizes[2] += box.box.GetEdge(BoxArea(area), BoxEdge::Bottom);
		edge_sizes[3] += box.box.GetEdge(BoxArea(area), BoxEdge::Left);
	}
	Vector2f inner_size;
	switch (fill_area)
	{
	case BoxArea::Border: inner_size = box.padding_size + box.box.GetFrameSize(BoxArea::Border); break;
	case BoxArea::Padding: inner_size = box.padding_size; break;
	case BoxArea::Content: inner_size = box.padding_size - box.box.GetFrameSize(BoxArea::Padding); break;
	case BoxArea::Margin:
	case BoxArea::Auto: RMLUI_ERROR;
	}

	const ComputedValues& computed = meta->computed_values;

	// The radii of the border box (CSS Backgrounds 3, 5.3 and 5.5): percentages of its width (horizontal radii) and height
	// (vertical ones), then all scaled down together if the curves of two adjacent corners would overlap. The inner radii
	// of the padding and content boxes follow from them (GeometryBackgroundBorder::ComputeBorderMetrics).
	CornerSizes2 border_radius = {};
	if (computed.has_border_radius())
	{
		const Vector2f border_size = box.padding_size + box.box.GetFrameSize(BoxArea::Border);
		for (int i = 0; i < 8; i++)
		{
			const int corner = i / 2, axis = i % 2;
			const float basis = (axis == 0 ? border_size.x : border_size.y);
			// Lengths, percentages, and calc() of both (also the mixes a transition between them interpolates).
			const float value = ResolveValue(computed.border_radius(corner, axis), basis);
			border_radius[corner][axis] = Math::Max(value, 0.f);
		}
		GeometryBackgroundBorder::ScaleRadiiToFit(border_radius, border_size);
		// A corner is square unless both of its radii are positive.
		for (Vector2f& radius : border_radius)
		{
			if (radius.x <= 0.f || radius.y <= 0.f)
				radius = Vector2f(0.f);
		}
	}

	RenderBox render_box{inner_size, box.offset, edge_sizes, border_radius};
	render_box.SetBorderStyles({computed.border_top_style(), computed.border_right_style(), computed.border_bottom_style(), computed.border_left_style()});

	// A lowered and interrupted top border (a fieldset's legend): the painted main box starts `inset` px lower.
	BorderTopCut cut;
	if (with_border_top_cut && index < 1 && GetBorderTopCut(cut))
	{
		const float inset = Math::Clamp(Math::Round(cut.inset), 0.f, Math::Max(inner_size.y, 0.f));
		render_box.SetBorderOffset(box.offset + Vector2f(0.f, inset));
		render_box.SetFillSize(inner_size - Vector2f(0.f, inset));
		render_box.SetBorderTopGap(Vector2f(cut.gap_begin, cut.gap_end));
	}

	return render_box;
}

Vector2f Element::GetRenderBoxFillSize(BoxArea fill_area)
{
	RMLUI_ASSERTMSG(fill_area >= BoxArea::Border && fill_area <= BoxArea::Content,
		"Render box can only be generated with fill area of border, padding or content.");

	// As GetRenderBox computes the inner size of the main box.
	UpdateAbsoluteOffsetAndRenderBoxData();
	switch (fill_area)
	{
	case BoxArea::Border: return rounded_main_padding_size + main_box.GetFrameSize(BoxArea::Border);
	case BoxArea::Padding: return rounded_main_padding_size;
	case BoxArea::Content: return rounded_main_padding_size - main_box.GetFrameSize(BoxArea::Padding);
	case BoxArea::Margin:
	case BoxArea::Auto: RMLUI_ERROR;
	}
	return Vector2f();
}

int Element::GetNumBoxes()
{
	return 1 + (int)additional_boxes.size();
}

float Element::GetBaseline() const
{
	return baseline;
}

bool Element::GetIntrinsicDimensions(Vector2f& /*dimensions*/, float& /*ratio*/)
{
	return false;
}

bool Element::IsReplaced()
{
	Vector2f unused_dimensions;
	float unused_ratio = 0.f;
	return GetIntrinsicDimensions(unused_dimensions, unused_ratio);
}

bool Element::IsPointWithinElement(const Vector2f point)
{
	const Vector2f position = GetAbsoluteOffset(BoxArea::Border);

	for (int i = 0; i < GetNumBoxes(); ++i)
	{
		Vector2f box_offset;
		const Box& box = GetBox(i, box_offset);

		const Vector2f box_position = position + box_offset;
		const Vector2f box_dimensions = box.GetSize(BoxArea::Border);
		if (point.x >= box_position.x && point.x <= (box_position.x + box_dimensions.x) && point.y >= box_position.y &&
			point.y <= (box_position.y + box_dimensions.y))
		{
			return true;
		}
	}

	return false;
}

void Element::UpdateLocalStackingContext()
{
	using namespace Style;
	const ComputedValues& computed = meta->computed_values;

	// CSS 2.2 9.9.1 and the properties that create stacking contexts (CSS Positioned Layout 3, Compositing 1, ...): z-index
	// applies to positioned elements and to flex and grid items only (6.3 applied it to every element).
	const Position position = computed.position();
	bool is_item = false;
	if (parent)
	{
		const Display parent_display = parent->meta->computed_values.display();
		is_item = (parent_display == Display::Flex || parent_display == Display::InlineFlex || parent_display == Display::Grid ||
			parent_display == Display::InlineGrid);
	}
	const ZIndex z_index_property = computed.z_index();
	// Documents (forced stacking contexts) and the context's root element are ordered by their z-index whatever their
	// position (Context::PullDocumentToFront).
	const bool z_index_applies =
		(z_index_property.type != ZIndex::Auto && (position != Position::Static || is_item || local_stacking_context_forced || !parent));

	// 'will-change' naming a property that creates a stacking context (CSS Will Change 1 §3.1, WillChange::ParseHints).
	const bool will_change = (computed.will_change() & WillChange::StackingContext) != 0;

	const float new_z_index = (z_index_applies ? z_index_property.value : 0.f);
	const bool enable_local_stacking_context = (z_index_applies || local_stacking_context_forced || computed.has_filter() ||
		computed.has_backdrop_filter() || computed.has_mask_image() || computed.has_local_transform() || computed.has_local_perspective() ||
		computed.opacity() < 1.f || position == Position::Fixed || position == Position::Sticky || computed.isolation() == Isolation::Isolate ||
		will_change);

	if (z_index != new_z_index || local_stacking_context != enable_local_stacking_context)
	{
		z_index = new_z_index;

		if (local_stacking_context != enable_local_stacking_context)
		{
			local_stacking_context = enable_local_stacking_context;

			// If we are no longer acting as a local stacking context, then we clear the list and are all set. Otherwise, we need to rebuild our
			// local stacking context.
			stacking_context.clear();
			stacking_context_dirty = local_stacking_context;
		}

		// When our z-index or local stacking context changes, then we must dirty our parent stacking context so we are re-indexed.
		if (parent)
			parent->DirtyStackingContext();
	}
}

float Element::GetDrawOpacity() const
{
	return opacity_leaf ? meta->computed_values.opacity() : 1.f;
}

// An element with an opacity below 1 is drawn into a layer composited with its opacity (ElementEffects), unless it draws
// one thing only: then its opacity is multiplied into that geometry, which gives the same pixels without a layer.
void Element::UpdateOpacityLeaf()
{
	const ComputedValues& computed = meta->computed_values;
	const float opacity = computed.opacity();
	bool leaf = false;
	if (opacity < 1.f && opacity > 0.f && !computed.has_filter() && !computed.has_backdrop_filter() && !computed.has_mask_image())
	{
		bool has_text = false, has_element_child = false;
		for (const ElementPtr& child : children)
		{
			if (child->GetTagName() == "#text")
				has_text = true;
			else
				has_element_child = true;
		}
		if (!has_element_child)
		{
			const auto visible_border = [&computed](float width, Colourb color) { return width > 0.f && color.alpha > 0; };
			Vector2f dimensions;
			float ratio = 0.f;
			const int painted = int(computed.background_color().alpha > 0) +
				int(visible_border(computed.border_top_width(), computed.border_top_color()) ||
					visible_border(computed.border_right_width(), computed.border_right_color()) ||
					visible_border(computed.border_bottom_width(), computed.border_bottom_color()) ||
					visible_border(computed.border_left_width(), computed.border_left_color())) +
				int(computed.has_box_shadow()) + int(computed.has_decorator()) + int(computed.outline_style() != Style::BorderStyle::None) +
				int(has_text) + int(GetIntrinsicDimensions(dimensions, ratio));
			leaf = (painted <= 1);
		}
	}
	if (leaf != opacity_leaf)
	{
		opacity_leaf = leaf;
		meta->background_border.DirtyBorder();
		// Everything drawn with the opacity is generated again: as for a change of the property (elements that draw
		// content of their own, decorators, text children).
		PropertyIdSet changed;
		changed.Insert(PropertyId::Opacity);
		OnPropertyChange(changed);
		for (const ElementPtr& child : children)
		{
			if (child->GetTagName() == "#text")
				child->OnPropertyChange(changed);
		}
	}
}

bool Element::IsVisible(bool include_ancestors) const
{
	if (!include_ancestors)
		return visible;
	const Element* element = this;
	while (element)
	{
		if (!element->visible)
			return false;
		element = element->parent;
	}
	return true;
}

float Element::GetZIndex() const
{
	return z_index;
}

FontFaceHandle Element::GetFontFaceHandle() const
{
	return meta->computed_values.font_face_handle();
}

bool Element::SetProperty(const String& name, const String& value)
{
	// The name may be a shorthand giving us multiple underlying properties
	PropertyDictionary properties;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(properties, name, value))
	{
		Log::Message(Log::LT_WARNING, "Syntax error parsing inline property declaration '%s: %s;'.", name.c_str(), value.c_str());
		return false;
	}
	for (auto& property : properties.GetProperties())
	{
		if (!meta->style.SetProperty(property.first, property.second))
			return false;
	}
	for (auto& property : properties.GetCustomProperties())
	{
		meta->style.SetCustomProperty(property.first, property.second);
	}
	for (auto& [shorthand_id, property] : properties.GetVarShorthands())
	{
		meta->style.SetVarShorthand(shorthand_id, property);
	}
	return true;
}

bool Element::SetProperty(PropertyId id, const Property& property)
{
	return meta->style.SetProperty(id, property);
}

void Element::RemoveProperty(const String& name)
{
	const PropertyId property_id = StyleSheetSpecification::GetPropertyId(name);
	if (property_id != PropertyId::Invalid)
	{
		meta->style.RemoveProperty(property_id);
		return;
	}

	if (StringUtilities::StartsWith(name, "--"))
	{
		meta->style.RemoveCustomProperty(name);
		return;
	}

	const ShorthandId shorthand_id = StyleSheetSpecification::GetShorthandId(name);
	if (shorthand_id != ShorthandId::Invalid)
	{
		const PropertyIdSet property_id_set = StyleSheetSpecification::GetShorthandUnderlyingProperties(shorthand_id);
		for (const PropertyId id : property_id_set)
			meta->style.RemoveProperty(id);

		meta->style.RemoveVarShorthand(shorthand_id);
		return;
	}
}

void Element::RemoveProperty(PropertyId id)
{
	meta->style.RemoveProperty(id);
}

const Property* Element::GetProperty(const String& name)
{
	const PropertyId property_id = StyleSheetSpecification::GetPropertyId(name);
	if (property_id != PropertyId::Invalid)
		return meta->style.GetProperty(property_id);

	if (StringUtilities::StartsWith(name, "--"))
		return meta->style.GetCustomProperty(name);

	return nullptr;
}

const Property* Element::GetProperty(PropertyId id)
{
	return meta->style.GetProperty(id);
}

const Property* Element::GetLocalProperty(const String& name)
{
	const PropertyId property_id = StyleSheetSpecification::GetPropertyId(name);
	if (property_id != PropertyId::Invalid)
		return meta->style.GetLocalProperty(property_id);

	if (StringUtilities::StartsWith(name, "--"))
		return meta->style.GetLocalCustomProperty(name);

	return nullptr;
}

const Property* Element::GetLocalProperty(PropertyId id)
{
	return meta->style.GetLocalProperty(id);
}

const PropertyMap& Element::GetLocalStyleProperties()
{
	return meta->style.GetLocalStyleProperties().GetProperties();
}

float Element::ResolveLength(NumericValue value)
{
	float result = 0.f;
	if (Any(value.unit & Unit::LENGTH))
		result = meta->style.ResolveNumericValue(value, 0.f);
	return result;
}

float Element::ResolveNumericValue(NumericValue value, float base_value)
{
	float result = 0.f;
	if (Any(value.unit & Unit::NUMERIC))
		result = meta->style.ResolveNumericValue(value, base_value);
	return result;
}

const Rectanglef* Element::GetContainingBlockOverride() const
{
	const Style::Position position = meta->computed_values.position();
	if ((position == Style::Position::Absolute || position == Style::Position::Fixed) && meta->containing_block_override)
		return &*meta->containing_block_override;
	return nullptr;
}

Vector2f Element::GetContainingBlock()
{
	Vector2f containing_block;

	if (const Rectanglef* containing_block_override = GetContainingBlockOverride())
	{
		containing_block = containing_block_override->Size();
	}
	else if (offset_parent)
	{
		using namespace Style;
		Position position_property = GetPosition();
		const Box& parent_box = offset_parent->GetBox();

		if (position_property == Position::Static || position_property == Position::Relative || position_property == Position::Sticky)
		{
			containing_block = parent_box.GetSize();
			containing_block.x -= meta->scroll.GetScrollbarSize(ElementScroll::VERTICAL);
			containing_block.y -= meta->scroll.GetScrollbarSize(ElementScroll::HORIZONTAL);
		}
		else if (position_property == Position::Absolute || position_property == Position::Fixed)
		{
			containing_block = parent_box.GetSize(BoxArea::Padding);
			containing_block.x = Math::Max(containing_block.x - offset_parent->meta->scroll.GetScrollbarSize(ElementScroll::VERTICAL), 0.f);
			containing_block.y = Math::Max(containing_block.y - offset_parent->meta->scroll.GetScrollbarSize(ElementScroll::HORIZONTAL), 0.f);
		}
	}
	else if (Context* context = GetContext())
	{
		containing_block = Vector2f(context->GetDimensions());
	}

	return containing_block;
}

Style::Position Element::GetPosition()
{
	return meta->computed_values.position();
}

Style::Float Element::GetFloat()
{
	return meta->computed_values.float_();
}

Style::Display Element::GetDisplay()
{
	return meta->computed_values.display();
}

float Element::GetLineHeight()
{
	return meta->computed_values.line_height().value;
}

const TransformState* Element::GetTransformState() const noexcept
{
	return transform_state.get();
}

bool Element::Project(Vector2f& point) const noexcept
{
	if (!transform_state || !transform_state->GetTransform())
		return true;

	// The input point is in window coordinates. Need to find the projection of the point onto the current element plane,
	// taking into account the full transform applied to the element.

	if (const Matrix4f* inv_transform = transform_state->GetInverseTransform())
	{
		// Pick two points forming a line segment perpendicular to the window.
		Vector4f window_points[2] = {{point.x, point.y, -10, 1}, {point.x, point.y, 10, 1}};

		// Project them into the local element space.
		window_points[0] = *inv_transform * window_points[0];
		window_points[1] = *inv_transform * window_points[1];

		Vector3f local_points[2] = {window_points[0].PerspectiveDivide(), window_points[1].PerspectiveDivide()};

		// Construct a ray from the two projected points in the local space of the current element.
		// Find the intersection with the z=0 plane to produce our destination point.
		Vector3f ray = local_points[1] - local_points[0];

		// Only continue if we are not close to parallel with the plane.
		if (Math::Absolute(ray.z) > 1.0f)
		{
			// Solving the line equation p = p0 + t*ray for t, knowing that p.z = 0, produces the following.
			float t = -local_points[0].z / ray.z;
			Vector3f p = local_points[0] + ray * t;

			point = Vector2f(p.x, p.y);
			return true;
		}
	}

	// The transformation matrix is either singular, or the ray is parallel to the element's plane.
	return false;
}

PropertiesIteratorView Element::IterateLocalProperties(Element* filter_inherited_by) const
{
	return PropertiesIteratorView(meta->style.IterateAll(filter_inherited_by));
}

void Element::SetPseudoClass(const String& pseudo_class, bool activate)
{
	if (meta->style.SetPseudoClass(pseudo_class, activate, false))
	{
		// Include siblings in case of RCSS presence of sibling combinators '+', '~'.
		DirtyDefinition(DirtyNodes::SelfAndSiblings);
		ElementCascade::OnHasDependencyChange(this, ElementCascade::Change::PseudoClass, pseudo_class);
		OnPseudoClassChange(pseudo_class, activate);
	}
}

bool Element::IsPseudoClassSet(const String& pseudo_class) const
{
	return meta->style.IsPseudoClassSet(pseudo_class);
}

bool Element::ArePseudoClassesSet(const StringList& pseudo_classes) const
{
	for (const String& pseudo_class : pseudo_classes)
	{
		if (!IsPseudoClassSet(pseudo_class))
			return false;
	}

	return true;
}

StringList Element::GetActivePseudoClasses() const
{
	const PseudoClassMap& pseudo_classes = meta->style.GetActivePseudoClasses();
	StringList names;
	names.reserve(pseudo_classes.size());
	for (auto& pseudo_class : pseudo_classes)
	{
		names.push_back(pseudo_class.first);
	}

	return names;
}

void Element::OverridePseudoClass(Element* element, const String& pseudo_class, bool activate)
{
	RMLUI_ASSERT(element);
	element->GetStyle()->SetPseudoClass(pseudo_class, activate, true);
}

Variant* Element::GetAttribute(const String& name)
{
	return GetIf(attributes, name);
}

const Variant* Element::GetAttribute(const String& name) const
{
	return GetIf(attributes, name);
}

bool Element::HasAttribute(const String& name) const
{
	return attributes.find(name) != attributes.end();
}

void Element::RemoveAttribute(const String& name)
{
	auto it = attributes.find(name);
	if (it != attributes.end())
	{
		attributes.erase(it);

		ElementAttributes changed_attributes;
		changed_attributes.emplace(name, Variant());
		OnAttributeChange(changed_attributes);
	}
}

Element* Element::GetFocusLeafNode()
{
	// If there isn't a focus, then we are the leaf.
	if (!focus)
	{
		return this;
	}

	// Recurse down the tree until we found the leaf focus element
	Element* focus_element = focus;
	while (focus_element->focus)
		focus_element = focus_element->focus;

	return focus_element;
}

Context* Element::GetContext() const
{
	ElementDocument* document = GetOwnerDocument();
	if (document != nullptr)
		return document->GetContext();

	return nullptr;
}

RenderManager* Element::GetRenderManager() const
{
	if (Context* context = GetContext())
		return &context->GetRenderManager();
	return nullptr;
}

void Element::SetAttributes(const ElementAttributes& _attributes)
{
	attributes.reserve(attributes.size() + _attributes.size());
	for (auto& pair : _attributes)
		attributes[pair.first] = pair.second;

	OnAttributeChange(_attributes);
}

int Element::GetNumAttributes() const
{
	return (int)attributes.size();
}

const String& Element::GetTagName() const
{
	return tag;
}

const String& Element::GetId() const
{
	return id;
}

void Element::SetId(const String& _id)
{
	SetAttribute("id", _id);
}

float Element::GetAbsoluteLeft()
{
	return GetAbsoluteOffset(BoxArea::Border).x;
}

float Element::GetAbsoluteTop()
{
	return GetAbsoluteOffset(BoxArea::Border).y;
}

float Element::GetClientLeft()
{
	return GetBox().GetPosition(BoxArea::Padding).x;
}

float Element::GetClientTop()
{
	return GetBox().GetPosition(BoxArea::Padding).y;
}

float Element::GetClientWidth()
{
	return GetBox().GetSize(BoxArea::Padding).x - meta->scroll.GetScrollbarSize(ElementScroll::VERTICAL);
}

float Element::GetClientHeight()
{
	return GetBox().GetSize(BoxArea::Padding).y - meta->scroll.GetScrollbarSize(ElementScroll::HORIZONTAL);
}

Element* Element::GetOffsetParent()
{
	return offset_parent;
}

float Element::GetOffsetLeft()
{
	return relative_offset_base.x + relative_offset_position.x;
}

float Element::GetOffsetTop()
{
	return relative_offset_base.y + relative_offset_position.y;
}

float Element::GetOffsetWidth()
{
	return GetBox().GetSize(BoxArea::Border).x;
}

float Element::GetOffsetHeight()
{
	return GetBox().GetSize(BoxArea::Border).y;
}

float Element::GetScrollLeft()
{
	return scroll_offset.x;
}

void Element::SetScrollLeft(float scroll_left)
{
	// 'overflow: clip' is not a scroll container: it cannot be scrolled, not even by script.
	if (meta->computed_values.overflow_x() == Style::Overflow::Clip)
		scroll_left = 0.f;
	const float new_offset = Math::Round(Math::Clamp(scroll_left, 0.0f, GetScrollWidth() - GetClientWidth()));
	if (new_offset != scroll_offset.x)
	{
		scroll_offset.x = new_offset;
		meta->scroll.UpdateScrollbar(ElementScroll::HORIZONTAL);
		DirtyAbsoluteOffset();

		DispatchEvent(EventId::Scroll, Dictionary());
	}
}

float Element::GetScrollTop()
{
	return scroll_offset.y;
}

void Element::SetScrollTop(float scroll_top)
{
	if (meta->computed_values.overflow_y() == Style::Overflow::Clip)
		scroll_top = 0.f;
	const float new_offset = Math::Round(Math::Clamp(Math::Round(scroll_top), 0.0f, GetScrollHeight() - GetClientHeight()));
	if (new_offset != scroll_offset.y)
	{
		scroll_offset.y = new_offset;
		meta->scroll.UpdateScrollbar(ElementScroll::VERTICAL);
		DirtyAbsoluteOffset();

		DispatchEvent(EventId::Scroll, Dictionary());
	}
}

float Element::GetScrollWidth()
{
	return Math::Max(scrollable_overflow_rectangle.x, GetClientWidth());
}

float Element::GetScrollHeight()
{
	return Math::Max(scrollable_overflow_rectangle.y, GetClientHeight());
}

ElementStyle* Element::GetStyle() const
{
	return &meta->style;
}

ElementDocument* Element::GetOwnerDocument() const
{
#ifdef RMLUI_DEBUG
	if (parent && !owner_document)
	{
		// Since we have a parent but no owner_document, then we must be a 'loose' element -- that is, constructed
		// outside of a document and not attached to a child of any element in the hierarchy of a document.
		// This check ensures that we didn't just forget to set the owner document.
		RMLUI_ASSERT(!parent->GetOwnerDocument());
	}
#endif

	return owner_document;
}

Element* Element::GetPseudoElement(PseudoElementType type) const
{
	// Generated by PseudoElements::Update (phase 3, cascade).
	if (type == PseudoElementType::None)
		return nullptr;
	return meta->pseudo_elements[int(type) - 1];
}


Element* Element::GetParentNode() const
{
	return parent;
}

Element* Element::Closest(const String& selectors) const
{
	StyleSheetNode root_node;
	StyleSheetNodeListRaw leaf_nodes = StyleSheetParser::ConstructNodes(root_node, selectors);

	if (leaf_nodes.empty())
	{
		Log::Message(Log::LT_WARNING, "Query selector '%s' is empty. In element %s", selectors.c_str(), GetAddress().c_str());
		return nullptr;
	}

	Element* parent = GetParentNode();

	while (parent)
	{
		for (const StyleSheetNode* node : leaf_nodes)
		{
			if (node->IsApplicable(parent, this))
			{
				return parent;
			}
		}

		parent = parent->GetParentNode();
	}

	return nullptr;
}

Element* Element::GetNextSibling() const
{
	if (parent == nullptr)
		return nullptr;

	for (size_t i = 0; i < parent->children.size() - (parent->num_non_dom_children + 1); i++)
	{
		if (parent->children[i].get() == this)
			return parent->children[i + 1].get();
	}

	return nullptr;
}

Element* Element::GetPreviousSibling() const
{
	if (parent == nullptr)
		return nullptr;

	for (size_t i = 1; i < parent->children.size() - parent->num_non_dom_children; i++)
	{
		if (parent->children[i].get() == this)
			return parent->children[i - 1].get();
	}

	return nullptr;
}

Element* Element::GetFirstChild() const
{
	if (GetNumChildren() > 0)
		return children[0].get();

	return nullptr;
}

Element* Element::GetLastChild() const
{
	if (GetNumChildren() > 0)
		return (children.end() - (num_non_dom_children + 1))->get();

	return nullptr;
}

Element* Element::GetChild(int index) const
{
	if (index < 0 || index >= (int)children.size())
		return nullptr;

	return children[index].get();
}

int Element::GetNumChildren(bool include_non_dom_elements) const
{
	return (int)children.size() - (include_non_dom_elements ? 0 : num_non_dom_children);
}

void Element::GetInnerRML(String& content) const
{
	for (int i = 0; i < GetNumChildren(); i++)
	{
		children[i]->GetRML(content);
	}
}

String Element::GetInnerRML() const
{
	String result;
	GetInnerRML(result);
	return result;
}

void Element::SetInnerRML(const String& rml)
{
	RMLUI_ZoneScopedC(0x6495ED);

	// Remove all DOM children.
	while ((int)children.size() > num_non_dom_children)
		RemoveChild(children.front().get());

	if (!rml.empty())
		Factory::InstanceElementText(this, rml);
}

bool Element::SetPointerCapture(PointerId pointer_id)
{
	Context* context = GetContext();
	return context && context->SetPointerCapture(this, pointer_id);
}

bool Element::ReleasePointerCapture(PointerId pointer_id)
{
	Context* context = GetContext();
	return context && context->ReleasePointerCapture(this, pointer_id);
}

bool Element::HasPointerCapture(PointerId pointer_id) const
{
	Context* context = const_cast<Element*>(this)->GetContext();
	return context && context->HasPointerCapture(this, pointer_id);
}

bool Element::Focus(bool focus_visible)
{
	// Are we allowed focus? (Not 'focus: none', not a disabled form control.)
	if (ElementCascade::IsUnfocusable(this))
		return false;

	// Ask our context if we can switch focus.
	Context* context = GetContext();
	if (context == nullptr)
		return false;

	if (!context->OnFocusChange(this, focus_visible))
		return false;

	// Set this as the end of the focus chain.
	focus = nullptr;

	// Update the focus chain up the hierarchy.
	Element* element = this;
	while (Element* parent = element->GetParentNode())
	{
		parent->focus = element;
		element = parent;
	}

	return true;
}

void Element::Blur()
{
	if (parent)
	{
		Context* context = GetContext();
		if (context == nullptr)
			return;

		if (context->GetFocusElement() == this)
		{
			parent->Focus();
		}
		else if (parent->focus == this)
		{
			parent->focus = nullptr;
		}
	}
}

void Element::Click()
{
	Context* context = GetContext();
	if (context == nullptr)
		return;

	context->GenerateClickEvent(this);
}

void Element::AddEventListener(const String& event, EventListener* listener, const bool in_capture_phase)
{
	const EventId id = EventSpecificationInterface::GetIdOrInsert(event);
	meta->event_dispatcher.AttachEvent(id, listener, in_capture_phase);
}

void Element::AddEventListener(const EventId id, EventListener* listener, const bool in_capture_phase)
{
	meta->event_dispatcher.AttachEvent(id, listener, in_capture_phase);
}

void Element::RemoveEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	EventId id = EventSpecificationInterface::GetIdOrInsert(event);
	meta->event_dispatcher.DetachEvent(id, listener, in_capture_phase);
}

void Element::RemoveEventListener(EventId id, EventListener* listener, bool in_capture_phase)
{
	meta->event_dispatcher.DetachEvent(id, listener, in_capture_phase);
}

bool Element::DispatchEvent(const String& type, const Dictionary& parameters)
{
	const EventSpecification& specification = EventSpecificationInterface::GetOrInsert(type);
	return EventDispatcher::DispatchEvent(this, specification.id, type, parameters, specification.interruptible, specification.bubbles,
		specification.default_action_phase);
}

bool Element::DispatchEvent(const String& type, const Dictionary& parameters, bool interruptible, bool bubbles)
{
	const EventSpecification& specification = EventSpecificationInterface::GetOrInsert(type);
	return EventDispatcher::DispatchEvent(this, specification.id, type, parameters, interruptible, bubbles, specification.default_action_phase);
}

bool Element::DispatchEvent(EventId id, const Dictionary& parameters)
{
	const EventSpecification& specification = EventSpecificationInterface::Get(id);
	return EventDispatcher::DispatchEvent(this, specification.id, specification.type, parameters, specification.interruptible, specification.bubbles,
		specification.default_action_phase);
}

bool Element::DispatchEvent(EventId id, const Dictionary& parameters, bool* default_prevented)
{
	const EventSpecification& specification = EventSpecificationInterface::Get(id);
	return EventDispatcher::DispatchEvent(this, specification.id, specification.type, parameters, specification.interruptible, specification.bubbles,
		specification.default_action_phase, default_prevented);
}

void Element::ScrollIntoView(const ScrollIntoViewOptions options)
{
	const Vector2f size = main_box.GetSize(BoxArea::Border);
	ScrollBehavior scroll_behavior = options.behavior;

	for (Element* scroll_parent = parent; scroll_parent; scroll_parent = scroll_parent->GetParentNode())
	{
		using Style::Overflow;
		const ComputedValues& computed = scroll_parent->GetComputedValues();
		const bool scrollable_box_x = (computed.overflow_x() == Overflow::Auto || computed.overflow_x() == Overflow::Scroll);
		const bool scrollable_box_y = (computed.overflow_y() == Overflow::Auto || computed.overflow_y() == Overflow::Scroll);

		const Vector2f parent_scroll_size = {scroll_parent->GetScrollWidth(), scroll_parent->GetScrollHeight()};
		const Vector2f parent_client_size = {scroll_parent->GetClientWidth(), scroll_parent->GetClientHeight()};

		if ((scrollable_box_x && parent_scroll_size.x > parent_client_size.x) || (scrollable_box_y && parent_scroll_size.y > parent_client_size.y))
		{
			const Vector2f relative_offset = scroll_parent->GetAbsoluteOffset(BoxArea::Border) - GetAbsoluteOffset(BoxArea::Border);

			const Vector2f old_scroll_offset = {scroll_parent->GetScrollLeft(), scroll_parent->GetScrollTop()};
			const Vector2f parent_client_offset = {scroll_parent->GetClientLeft(), scroll_parent->GetClientTop()};

			const Vector2f delta_scroll_offset_start = parent_client_offset - relative_offset;
			const Vector2f delta_scroll_offset_end = delta_scroll_offset_start + size - parent_client_size;

			Vector2f scroll_delta = {
				scrollable_box_x ? GetScrollOffsetDelta(options.horizontal, delta_scroll_offset_start.x, delta_scroll_offset_end.x) : 0.f,
				scrollable_box_y ? GetScrollOffsetDelta(options.vertical, delta_scroll_offset_start.y, delta_scroll_offset_end.y) : 0.f,
			};

			scroll_parent->ScrollTo(old_scroll_offset + scroll_delta, scroll_behavior);

			// Currently, only a single scrollable parent can be smooth scrolled at a time, so any other parents must be instant scrolled.
			scroll_behavior = ScrollBehavior::Instant;
		}

		if ((scrollable_box_x || scrollable_box_y) && options.parentage == ScrollParentage::Closest)
			break;
	}
}

void Element::ScrollIntoView(bool align_with_top)
{
	ScrollIntoViewOptions options;
	options.vertical = (align_with_top ? ScrollAlignment::Start : ScrollAlignment::End);
	options.horizontal = ScrollAlignment::Nearest;
	ScrollIntoView(options);
}

void Element::ScrollTo(Vector2f offset, ScrollBehavior behavior)
{
	if (behavior != ScrollBehavior::Instant)
	{
		if (Context* context = GetContext())
		{
			context->PerformSmoothscrollOnTarget(this, offset - scroll_offset, behavior);
			return;
		}
	}

	SetScrollLeft(offset.x);
	SetScrollTop(offset.y);
}

Element* Element::AppendChild(ElementPtr child, bool dom_element)
{
	RMLUI_ASSERT(child);
	Element* child_ptr = child.get();
	if (dom_element)
		children.insert(children.end() - num_non_dom_children, std::move(child));
	else
	{
		children.push_back(std::move(child));
		num_non_dom_children++;
	}
	// Set parent just after inserting into children. This allows us to eg. get our previous sibling in SetParent.
	child_ptr->SetParent(this);

	Element* ancestor = child_ptr;
	for (int i = 0; i <= ChildNotifyLevels && ancestor; i++, ancestor = ancestor->GetParentNode())
		ancestor->OnChildAdd(child_ptr);

	DirtyStackingContext();

	// Not only does the element definition of the newly inserted element need to be dirtied, but also our own definition and implicitly all of our
	// children's. This ensures correct styles being applied in the presence of tree-structural selectors such as ':first-child'.
	DirtyDefinition(DirtyNodes::Self);

	if (dom_element)
	{
		DirtyLayout();
		ElementCascade::OnHasDependencyChange(this, ElementCascade::Change::Structure, String());
		ElementCascade::DirtyCountersIfUsed(*this);
	}

	return child_ptr;
}

Element* Element::InsertBefore(ElementPtr child, Element* adjacent_element)
{
	RMLUI_ASSERT(child);
	// Find the position in the list of children of the adjacent element. If
	// it's nullptr or we can't find it, then we insert it at the end of the dom
	// children, as a dom element.
	size_t child_index = 0;
	bool found_child = false;
	if (adjacent_element)
	{
		for (child_index = 0; child_index < children.size(); child_index++)
		{
			if (children[child_index].get() == adjacent_element)
			{
				found_child = true;
				break;
			}
		}
	}

	Element* child_ptr = nullptr;

	if (found_child)
	{
		child_ptr = child.get();

		if ((int)child_index >= GetNumChildren())
			num_non_dom_children++;
		else
			DirtyLayout();

		children.insert(children.begin() + child_index, std::move(child));
		child_ptr->SetParent(this);

		Element* ancestor = child_ptr;
		for (int i = 0; i <= ChildNotifyLevels && ancestor; i++, ancestor = ancestor->GetParentNode())
			ancestor->OnChildAdd(child_ptr);

		DirtyStackingContext();
		DirtyDefinition(DirtyNodes::Self);
		ElementCascade::OnHasDependencyChange(this, ElementCascade::Change::Structure, String());
		ElementCascade::DirtyCountersIfUsed(*this);
	}
	else
	{
		child_ptr = AppendChild(std::move(child));
	}

	return child_ptr;
}

ElementPtr Element::ReplaceChild(ElementPtr inserted_element, Element* replaced_element)
{
	RMLUI_ASSERT(inserted_element);
	auto insertion_point = children.begin();
	while (insertion_point != children.end() && insertion_point->get() != replaced_element)
	{
		++insertion_point;
	}

	Element* inserted_element_ptr = inserted_element.get();

	if (insertion_point == children.end())
	{
		AppendChild(std::move(inserted_element));
		return nullptr;
	}

	children.insert(insertion_point, std::move(inserted_element));
	inserted_element_ptr->SetParent(this);

	ElementPtr result = RemoveChild(replaced_element);

	Element* ancestor = inserted_element_ptr;
	for (int i = 0; i <= ChildNotifyLevels && ancestor; i++, ancestor = ancestor->GetParentNode())
		ancestor->OnChildAdd(inserted_element_ptr);

	return result;
}

ElementPtr Element::RemoveChild(Element* child)
{
	size_t child_index = 0;

	for (auto itr = children.begin(); itr != children.end(); ++itr)
	{
		// Add the element to the delete list
		if (itr->get() == child)
		{
			Element* ancestor = child;
			for (int i = 0; i <= ChildNotifyLevels && ancestor; i++, ancestor = ancestor->GetParentNode())
				ancestor->OnChildRemove(child);

			if (child_index >= children.size() - num_non_dom_children)
				num_non_dom_children--;

			ElementPtr detached_child = std::move(*itr);
			children.erase(itr);

			// Remove the child element as the focused child of this element.
			if (child == focus)
			{
				focus = nullptr;

				// If this child (or a descendant of this child) is the context's currently
				// focused element, set the focus to us instead.
				if (Context* context = GetContext())
				{
					Element* focus_element = context->GetFocusElement();
					while (focus_element)
					{
						if (focus_element == child)
						{
							Focus();
							break;
						}

						focus_element = focus_element->GetParentNode();
					}
				}
			}

			detached_child->SetParent(nullptr);

			DirtyLayout();
			DirtyStackingContext();
			DirtyDefinition(DirtyNodes::Self);
			ElementCascade::OnHasDependencyChange(this, ElementCascade::Change::Structure, String());
			ElementCascade::DirtyCountersIfUsed(*this);

			return detached_child;
		}

		child_index++;
	}

	return nullptr;
}

bool Element::HasChildNodes() const
{
	return (int)children.size() > num_non_dom_children;
}

Element* Element::GetElementById(const String& id)
{
	// Check for special-case tokens.
	if (id == "#self")
		return this;
	else if (id == "#document")
		return GetOwnerDocument();
	else if (id == "#parent")
		return this->parent;
	else
	{
		Element* search_root = GetOwnerDocument();
		if (search_root == nullptr)
			search_root = this;
		return ElementUtilities::GetElementById(search_root, id);
	}
}

void Element::GetElementsByTagName(ElementList& elements, const String& tag)
{
	return ElementUtilities::GetElementsByTagName(elements, this, tag);
}

void Element::GetElementsByClassName(ElementList& elements, const String& class_name)
{
	return ElementUtilities::GetElementsByClassName(elements, this, class_name);
}

static Element* QuerySelectorMatchRecursive(const StyleSheetNodeListRaw& nodes, Element* element, Element* scope)
{
	const int num_children = element->GetNumChildren();

	for (int i = 0; i < num_children; i++)
	{
		Element* child = element->GetChild(i);
		if (child->GetTagName() == "#text")
			continue;

		for (const StyleSheetNode* node : nodes)
		{
			if (node->IsApplicable(child, scope))
				return child;
		}

		Element* matching_element = QuerySelectorMatchRecursive(nodes, child, scope);
		if (matching_element)
			return matching_element;
	}

	return nullptr;
}

static void QuerySelectorAllMatchRecursive(ElementList& matching_elements, const StyleSheetNodeListRaw& nodes, Element* element, Element* scope)
{
	const int num_children = element->GetNumChildren();

	for (int i = 0; i < num_children; i++)
	{
		Element* child = element->GetChild(i);
		if (child->GetTagName() == "#text")
			continue;

		for (const StyleSheetNode* node : nodes)
		{
			if (node->IsApplicable(child, scope))
			{
				matching_elements.push_back(child);
				break;
			}
		}

		QuerySelectorAllMatchRecursive(matching_elements, nodes, child, scope);
	}
}

Element* Element::QuerySelector(const String& selectors)
{
	StyleSheetNode root_node;
	StyleSheetNodeListRaw leaf_nodes = StyleSheetParser::ConstructNodes(root_node, selectors);

	if (leaf_nodes.empty())
	{
		Log::Message(Log::LT_WARNING, "Query selector '%s' is empty. In element %s", selectors.c_str(), GetAddress().c_str());
		return nullptr;
	}

	return QuerySelectorMatchRecursive(leaf_nodes, this, this);
}

void Element::QuerySelectorAll(ElementList& elements, const String& selectors)
{
	StyleSheetNode root_node;
	StyleSheetNodeListRaw leaf_nodes = StyleSheetParser::ConstructNodes(root_node, selectors);

	if (leaf_nodes.empty())
	{
		Log::Message(Log::LT_WARNING, "Query selector '%s' is empty. In element %s", selectors.c_str(), GetAddress().c_str());
		return;
	}

	QuerySelectorAllMatchRecursive(elements, leaf_nodes, this, this);
}

bool Element::Matches(const String& selectors)
{
	StyleSheetNode root_node;
	StyleSheetNodeListRaw leaf_nodes = StyleSheetParser::ConstructNodes(root_node, selectors);

	if (leaf_nodes.empty())
	{
		Log::Message(Log::LT_WARNING, "Query selector '%s' is empty. In element %s", selectors.c_str(), GetAddress().c_str());
		return false;
	}

	for (const StyleSheetNode* node : leaf_nodes)
	{
		if (node->IsApplicable(this, this))
		{
			return true;
		}
	}

	return false;
}

bool Element::Contains(Element* element) const
{
	while (element)
	{
		if (element == this)
			return true;
		element = element->GetParentNode();
	}
	return false;
}

EventDispatcher* Element::GetEventDispatcher() const
{
	return &meta->event_dispatcher;
}

String Element::GetEventDispatcherSummary() const
{
	return meta->event_dispatcher.ToString();
}

ElementBackgroundBorder* Element::GetElementBackgroundBorder() const
{
	return &meta->background_border;
}

bool Element::IsPaintedAsPrimitives(const char** reason) const
{
	if (reason)
		*reason = meta->background_border.GetPaintReason();
	return meta->background_border.IsPaintedAsPrimitives();
}

ElementScroll* Element::GetElementScroll() const
{
	return &meta->scroll;
}

DataModel* Element::GetDataModel() const
{
	return data_model;
}

void Element::SetInstancer(ElementInstancer* _instancer)
{
	// Only record the first instancer being set as some instancers call other instancers to do their dirty work, in
	// which case we don't want to update the lowest level instancer.
	if (!instancer)
	{
		instancer = _instancer;
	}
}

void Element::ForceLocalStackingContext()
{
	local_stacking_context_forced = true;
	local_stacking_context = true;

	DirtyStackingContext();
}

void Element::OnUpdate() {}

void Element::OnRender() {}

void Element::OnResize() {}

void Element::OnLayout() {}

bool Element::GetBorderTopCut(BorderTopCut& /*cut*/)
{
	return false;
}

bool Element::HasButtonLayout() const
{
	return false;
}

bool Element::HasGeneratedContentBoxes()
{
	return !IsReplaced();
}

bool Element::HasNativeAppearance() const
{
	return false;
}

bool Element::HasThemedBox() const
{
	return false;
}

void Element::DirtyBorderTopCut()
{
	meta->background_border.DirtyBackground();
	meta->background_border.DirtyBorder();
	meta->effects.DirtyEffectsData();
}

void Element::OnDpRatioChange() {}

void Element::OnStyleSheetChange() {}

void Element::OnAttributeChange(const ElementAttributes& changed_attributes)
{
	for (const auto& element_attribute : changed_attributes)
	{
		const auto& attribute = element_attribute.first;
		const auto& value = element_attribute.second;
		if (attribute == "id")
		{
			id = value.Get<String>();
		}
		else if (attribute == "class")
		{
			meta->style.SetClassNames(value.Get<String>());
		}
		else if (((attribute == "colspan" || attribute == "rowspan") && meta->computed_values.display() == Style::Display::TableCell) ||
			(attribute == "span" &&
				(meta->computed_values.display() == Style::Display::TableColumn ||
					meta->computed_values.display() == Style::Display::TableColumnGroup)))
		{
			DirtyLayout();
		}
		else if (attribute.size() > 2 && attribute[0] == 'o' && attribute[1] == 'n')
		{
			static constexpr size_t on_length = 2;
			static constexpr size_t capture_length = 7;
			const bool in_capture_phase = StringUtilities::EndsWith(attribute, "capture");
			auto& attribute_event_listeners = meta->attribute_event_listeners;
			auto& event_dispatcher = meta->event_dispatcher;
			const size_t event_name_length = attribute.size() - on_length - (in_capture_phase ? capture_length : 0);
			const auto event_id = EventSpecificationInterface::GetIdOrInsert(attribute.substr(on_length, event_name_length));
			const auto remove_event_listener_if_exists = [&attribute_event_listeners, &event_dispatcher, event_id, in_capture_phase]() {
				const auto listener_it = attribute_event_listeners.find(event_id);
				if (listener_it != attribute_event_listeners.cend())
				{
					event_dispatcher.DetachEvent(event_id, listener_it->second, in_capture_phase);
					attribute_event_listeners.erase(listener_it);
				}
			};

			if (value.GetType() == Variant::Type::STRING)
			{
				remove_event_listener_if_exists();

				const auto value_as_string = value.Get<String>();
				auto insertion_result = attribute_event_listeners.emplace(event_id, Factory::InstanceEventListener(value_as_string, this));
				if (auto* listener = insertion_result.first->second)
					event_dispatcher.AttachEvent(event_id, listener, in_capture_phase);
			}
			else if (value.GetType() == Variant::Type::NONE)
				remove_event_listener_if_exists();
		}
		else if (attribute == "style")
		{
			// The attribute's declarations replace those of its previous value (phase 3, cascade); removing it removes them.
			if (value.GetType() == Variant::STRING)
			{
				PropertyDictionary properties;
				StyleSheetParser parser;
				parser.ParseProperties(properties, value.GetReference<String>());
				meta->style.SetStyleAttribute(properties);
			}
			else if (value.GetType() == Variant::NONE)
				meta->style.SetStyleAttribute(PropertyDictionary());
			else
				Log::Message(Log::LT_WARNING, "Invalid 'style' attribute, string type required. In element: %s", GetAddress().c_str());
		}
		else if (attribute == "lang")
		{
			if (value.GetType() == Variant::STRING)
				meta->style.SetProperty(PropertyId::RmlUi_Language, Property(value.GetReference<String>(), Unit::STRING));
			else if (value.GetType() != Variant::NONE)
				Log::Message(Log::LT_WARNING, "Invalid 'lang' attribute, string type required. In element: %s", GetAddress().c_str());
		}
		else if (attribute == "dir")
		{
			if (value.GetType() == Variant::STRING)
			{
				const String& dir_value = value.GetReference<String>();

				if (dir_value == "auto")
					meta->style.SetProperty(PropertyId::RmlUi_Direction, Property(Style::Direction::Auto));
				else if (dir_value == "ltr")
					meta->style.SetProperty(PropertyId::RmlUi_Direction, Property(Style::Direction::Ltr));
				else if (dir_value == "rtl")
					meta->style.SetProperty(PropertyId::RmlUi_Direction, Property(Style::Direction::Rtl));
				else
					Log::Message(Log::LT_WARNING, "Invalid 'dir' attribute '%s', value must be 'auto', 'ltr', or 'rtl'. In element: %s",
						dir_value.c_str(), GetAddress().c_str());
			}
			else if (value.GetType() != Variant::NONE)
				Log::Message(Log::LT_WARNING, "Invalid 'dir' attribute, string type required. In element: %s", GetAddress().c_str());
		}
	}

	// Any change to the attributes may affect which styles apply to the current element, in particular due to attribute selectors, ID selectors, and
	// class selectors. This can further affect all siblings or descendants due to sibling or descendant combinators.
	DirtyDefinition(DirtyNodes::SelfAndSiblings);
	for (const auto& element_attribute : changed_attributes)
		ElementCascade::OnHasDependencyChange(this, ElementCascade::Change::Attribute, element_attribute.first);
	// attr() in the pseudo-elements, <ol start>, <li value> (phase 3).
	PseudoElements::OnAttributeChange(*this, changed_attributes);
}

void Element::OnPropertyChange(const PropertyIdSet& changed_properties)
{
	RMLUI_ZoneScoped;
	const bool top_right_bottom_left_changed = (           //
		changed_properties.Contains(PropertyId::Top) ||    //
		changed_properties.Contains(PropertyId::Right) ||  //
		changed_properties.Contains(PropertyId::Bottom) || //
		changed_properties.Contains(PropertyId::Left)      //
	);

	// See if the document layout needs to be updated.
	if (!IsLayoutDirty())
	{
		// Force a relayout if any of the changed properties require it.
		if (changed_properties.Intersects(StyleSheetSpecification::GetRegisteredPropertiesForcingLayout()))
		{
			DirtyLayout();
		}
		else if (top_right_bottom_left_changed)
		{
			// Normally, the position properties only affect the position of the element and not the layout. Thus, these properties are not registered
			// as affecting layout. However, when absolutely positioned elements with both left & right, or top & bottom are set to definite values,
			// they affect the size of the element and thereby also the layout. This layout-dirtying condition needs to be registered manually.
			using namespace Style;
			const ComputedValues& computed = GetComputedValues();
			const bool absolutely_positioned = (computed.position() == Position::Absolute || computed.position() == Position::Fixed);
			const bool sized_width =
				(computed.width().type == Width::Auto && computed.left().type != Left::Auto && computed.right().type != Right::Auto);
			const bool sized_height =
				(computed.height().type == Height::Auto && computed.top().type != Top::Auto && computed.bottom().type != Bottom::Auto);

			if (absolutely_positioned && (sized_width || sized_height))
				DirtyLayout();
		}
	}

	// Update the position.
	if (top_right_bottom_left_changed)
	{
		UpdateOffset();
		DirtyAbsoluteOffset();
	}

	// Update the visibility.
	if (changed_properties.Contains(PropertyId::Visibility) || changed_properties.Contains(PropertyId::Display))
	{
		bool new_visibility =
			(meta->computed_values.display() != Style::Display::None && meta->computed_values.visibility() == Style::Visibility::Visible);

		if (visible != new_visibility)
		{
			visible = new_visibility;

			if (parent != nullptr)
				parent->DirtyStackingContext();

			if (!visible)
				Blur();
		}
	}

	// Each corner has a horizontal and a vertical radius (phase 3, p3-paint).
	bool border_radius_changed = false;
	for (int i = 0; i < 8; i++)
		border_radius_changed |= changed_properties.Contains(PropertyId(int(PropertyId::RmlUi_BorderTopLeftRadiusX) + i));
	// Fork (phase 5, P5-1): '-jfx-text-fill' is part of the element's filters.
	const PropertyId text_fill_id = StyleSheetSpecification::GetTextFillPropertyId();
	const bool filter_or_mask_changed = (changed_properties.Contains(PropertyId::Filter) || changed_properties.Contains(PropertyId::BackdropFilter) ||
		(text_fill_id != PropertyId::Invalid && changed_properties.Contains(text_fill_id)) ||
		changed_properties.Contains(PropertyId::MaskImage) || changed_properties.Contains(PropertyId::MaskSize) ||
		changed_properties.Contains(PropertyId::MaskPositionX) || changed_properties.Contains(PropertyId::MaskPositionY) ||
		changed_properties.Contains(PropertyId::MaskRepeat));
	const bool perspective_changed = changed_properties.Contains(PropertyId::Perspective) ||
		changed_properties.Contains(PropertyId::PerspectiveOriginX) || changed_properties.Contains(PropertyId::PerspectiveOriginY);
	const bool transform_changed = changed_properties.Contains(PropertyId::Transform) || changed_properties.Contains(PropertyId::TransformOriginX) ||
		changed_properties.Contains(PropertyId::TransformOriginY) || changed_properties.Contains(PropertyId::TransformOriginZ) ||
		changed_properties.Contains(PropertyId::Translate) || changed_properties.Contains(PropertyId::Rotate) ||
		changed_properties.Contains(PropertyId::Scale);

	// A transform, a filter, ... make the element the containing block of its fixed and absolutely positioned descendants:
	// their layout changes when that starts or stops (not when only their values change, as in a transition), if it has
	// such descendants (a 'position' change dirties the layout anyway).
	if (transform_changed || perspective_changed || filter_or_mask_changed || changed_properties.Contains(PropertyId::WillChange) ||
		changed_properties.Contains(PropertyId::Position))
	{
		const bool new_fixed_containing_block = WillChange::IsFixedPositioningContainingBlock(meta->computed_values);
		const bool new_absolute_containing_block = WillChange::IsAbsolutePositioningContainingBlock(meta->computed_values);
		const bool fixed_flip = (new_fixed_containing_block != fixed_containing_block);
		const bool absolute_flip = (new_absolute_containing_block != absolute_containing_block);
		fixed_containing_block = new_fixed_containing_block;
		absolute_containing_block = new_absolute_containing_block;
		if ((fixed_flip || absolute_flip) && HasPositionedDescendant(this, absolute_flip))
			DirtyLayout();
	}

	// Update the z-index and stacking context.
	if (changed_properties.Contains(PropertyId::ZIndex) || filter_or_mask_changed || perspective_changed || transform_changed ||
		changed_properties.Contains(PropertyId::Opacity) || changed_properties.Contains(PropertyId::Position) ||
		changed_properties.Contains(PropertyId::Isolation) || changed_properties.Contains(PropertyId::WillChange))
	{
		UpdateLocalStackingContext();
	}
	// 'z-index' applies to flex and grid items whatever their position.
	if (changed_properties.Contains(PropertyId::Display))
	{
		for (int i = 0; i < GetNumChildren(true); i++)
			GetChild(i)->UpdateLocalStackingContext();
	}

	// Dirty the background if it's changed (with the background layers, phase 3 p3-paint).
	bool background_layers_changed = false;
	for (PropertyId id : {PropertyId::BackgroundImage, PropertyId::BackgroundPositionX, PropertyId::BackgroundPositionY, PropertyId::BackgroundSize,
			 PropertyId::BackgroundRepeat, PropertyId::BackgroundOrigin, PropertyId::BackgroundClip, PropertyId::BackgroundAttachment})
		background_layers_changed |= changed_properties.Contains(id);
	// (A colour change only where gradient layers may take 'currentcolor'.)
	if (changed_properties.Contains(PropertyId::Color) && meta->computed_values.has_background_image())
		background_layers_changed = true;
	if (border_radius_changed || background_layers_changed ||       //
		changed_properties.Contains(PropertyId::BackgroundColor) || //
		changed_properties.Contains(PropertyId::Opacity) ||         //
		changed_properties.Contains(PropertyId::ImageColor) ||      //
		changed_properties.Contains(PropertyId::BoxShadow))         //
	{
		meta->background_border.DirtyBackground();
	}

	// Dirty the border if it's changed.
	if (border_radius_changed ||                                      //
		changed_properties.Contains(PropertyId::BorderTopWidth) ||    //
		changed_properties.Contains(PropertyId::BorderRightWidth) ||  //
		changed_properties.Contains(PropertyId::BorderBottomWidth) || //
		changed_properties.Contains(PropertyId::BorderLeftWidth) ||   //
		changed_properties.Contains(PropertyId::BorderTopColor) ||    //
		changed_properties.Contains(PropertyId::BorderRightColor) ||  //
		changed_properties.Contains(PropertyId::BorderBottomColor) || //
		changed_properties.Contains(PropertyId::BorderLeftColor) ||   //
		changed_properties.Contains(PropertyId::Opacity))
	{
		meta->background_border.DirtyBorder();
	}

	// Dirty the effects if they've changed.
	if (border_radius_changed || filter_or_mask_changed || changed_properties.Contains(PropertyId::Decorator))
	{
		meta->effects.DirtyEffects();
	}

	const bool font_changed = (changed_properties.Contains(PropertyId::FontFamily) || changed_properties.Contains(PropertyId::FontStyle) ||
		changed_properties.Contains(PropertyId::FontWeight) || changed_properties.Contains(PropertyId::FontSize) ||
		changed_properties.Contains(PropertyId::FontKerning) || changed_properties.Contains(PropertyId::LetterSpacing));

	// Dirty the effects data when their visual looks may have changed.
	if (border_radius_changed ||                            //
		font_changed ||                                     //
		changed_properties.Contains(PropertyId::Opacity) || //
		changed_properties.Contains(PropertyId::Color) ||   //
		changed_properties.Contains(PropertyId::ImageColor))
	{
		meta->effects.DirtyEffectsData();
	}

	if (perspective_changed)
		DirtyTransformState(true, false);

	if (transform_changed)
		DirtyTransformState(false, true);

	/*
	  Phase 3 sections (docs/html-ui/phase-3-fork-patches.md 3.0, 3.1.3): each package reacts to its own properties in
	  its section below, and only there (a call into the package's own source file where it needs more than a line).
	*/

	// -- Groundwork (p3-base) --
	// Children are painted in layout order: flex items by 'order', the children of a 'display: contents' element in its
	// parent's place (Element::AddChildrenToStackingContext).
	{
		bool paint_order_changed = changed_properties.Contains(PropertyId::Order);
		if (changed_properties.Contains(PropertyId::Display))
		{
			const bool new_display_contents = (meta->computed_values.display() == Style::Display::Contents);
			if (new_display_contents != display_contents)
			{
				display_contents = new_display_contents;
				paint_order_changed = true;
				// It generates no box from now on: the one of its last layout is not kept.
				if (display_contents)
					SetBox(Box(Vector2f(0.f)));
			}
		}
		if (paint_order_changed && parent)
			parent->DirtyStackingContext();
	}

	// -- Values (p3-values) --

	// -- Cascade (p3-cascade) --
	// The ::marker depends on the item's display and list styles; counters on the counter properties and the boxes.
	if (changed_properties.Contains(PropertyId::Display) || changed_properties.Contains(PropertyId::ListStyleType) ||
		changed_properties.Contains(PropertyId::ListStyleImage) || changed_properties.Contains(PropertyId::ListStylePosition))
	{
		ElementCascade::DirtyPseudoElements(*this);
		ElementCascade::DirtyCountersIfUsed(*this);
	}
	if (changed_properties.Contains(PropertyId::CounterReset) || changed_properties.Contains(PropertyId::CounterIncrement) ||
		changed_properties.Contains(PropertyId::CounterSet))
	{
		// A counter property set to something else than 'none' makes the document resolve counters; one back to 'none'
		// dirties them only when the document already does.
		auto declares = [this](PropertyId id) {
			const Property* property = GetProperty(id);
			return property && property->unit == Unit::STYLE_VALUE;
		};
		if (declares(PropertyId::CounterReset) || declares(PropertyId::CounterIncrement) || declares(PropertyId::CounterSet))
			PseudoElements::DirtyCounters(*this);
		else
			ElementCascade::DirtyCountersIfUsed(*this);
	}

	// -- Paint (p3-paint) --
	if (changed_properties.Contains(PropertyId::OutlineWidth) || changed_properties.Contains(PropertyId::OutlineStyle) ||
		changed_properties.Contains(PropertyId::OutlineColor) || changed_properties.Contains(PropertyId::OutlineOffset) ||
		changed_properties.Contains(PropertyId::Opacity) ||
		(changed_properties.Contains(PropertyId::Color) && meta->computed_values.outline_color_is_current()))
	{
		meta->background_border.DirtyOutline();
	}
	if (changed_properties.Contains(PropertyId::BorderTopStyle) || changed_properties.Contains(PropertyId::BorderRightStyle) ||
		changed_properties.Contains(PropertyId::BorderBottomStyle) || changed_properties.Contains(PropertyId::BorderLeftStyle))
	{
		meta->background_border.DirtyBackground();
		meta->background_border.DirtyBorder();
	}

	// -- Text (p3-text) --

	// -- Layout (p3-layout) --

	// -- Animation (p3-animation) --
	// The animation and transition lists are built from their longhands (PropertyParserAnimation).
	if (changed_properties.Intersects(GetAnimationLonghandIds()))
		dirty_animation = true;
	if (changed_properties.Intersects(GetTransitionLonghandIds()))
		dirty_transition = true;

	// -- Grid (p3-grid) --
	// The items of a grid container are painted as flex items are (AddChildrenToStackingContext): a change of 'display' can
	// change how this element's children are painted.
	if (changed_properties.Contains(PropertyId::Display))
		DirtyStackingContext();

	// -- Line clamp (fork, P6-L1) --
	// The ellipsis a block container draws is placed by its lines: a box of another kind has none.
	if (changed_properties.Contains(PropertyId::Display) && draws_line_clamp_ellipsis)
		LineClampAccess::SetBlockEllipsis(this, nullptr);
	// It has the block container's colour and text shadows.
	if (draws_line_clamp_ellipsis &&
		(changed_properties.Contains(PropertyId::Color) || changed_properties.Contains(PropertyId::TextShadow) ||
			changed_properties.Contains(PropertyId::Opacity)))
		LineClampAccess::DirtyBlockEllipsis(this);
}

void Element::OnPseudoClassChange(const String& /*pseudo_class*/, bool /*activate*/) {}

void Element::OnChildAdd(Element* /*child*/) {}

void Element::OnChildRemove(Element* /*child*/) {}

void Element::DirtyLayout()
{
	ElementDocument* document = GetOwnerDocument();
	if (!document)
		return;
	// Fork (P7-L): a change inside a layout boundary lays out the boundary alone (the nearest one); the elements
	// that are no boundary pay one counter test while the document has none.
	if (num_layout_boundaries > 0)
	{
		for (Element* ancestor = parent; ancestor && ancestor != document; ancestor = ancestor->parent)
		{
			if (ancestor->layout_boundary)
			{
				document->DirtyLayoutBoundary(ancestor);
				return;
			}
		}
	}
	document->DirtyLayout();
}

void Element::SetLayoutBoundary(bool boundary)
{
	if (layout_boundary == boundary)
		return;
	layout_boundary = boundary;
	num_layout_boundaries += boundary ? 1 : -1;
	// The next layout is the document's: the boundary's box comes from its parent's formatting.
	if (ElementDocument* document = GetOwnerDocument())
		document->DirtyLayout();
}

bool Element::IsLayoutBoundary() const
{
	return layout_boundary;
}

bool Element::IsLayoutDirty()
{
	if (Element* document = GetOwnerDocument())
		return document->IsLayoutDirty();
	return false;
}

Element* Element::GetClosestScrollableContainer()
{
	using namespace Style;

	Overflow overflow_x = meta->computed_values.overflow_x();
	Overflow overflow_y = meta->computed_values.overflow_y();
	bool scrollable_x = (overflow_x == Overflow::Auto || overflow_x == Overflow::Scroll);
	bool scrollable_y = (overflow_y == Overflow::Auto || overflow_y == Overflow::Scroll);

	scrollable_x = (scrollable_x && GetScrollWidth() > GetClientWidth());
	scrollable_y = (scrollable_y && GetScrollHeight() > GetClientHeight());

	if (scrollable_x || scrollable_y || meta->computed_values.overscroll_behavior() == OverscrollBehavior::Contain)
		return this;
	else if (parent)
		return parent->GetClosestScrollableContainer();

	return nullptr;
}

void Element::ProcessDefaultAction(Event& /*event*/)
{
	// ':hover' and ':active' are set by the context, counted over its pointers (fork, engine-gaps G5-1: a lifted finger
	// no longer clears another finger's); the focus pseudo-classes by Context::OnFocusChange (phase 3).
}

const Style::ComputedValues& Element::GetComputedValues() const
{
	return meta->computed_values;
}

void Element::GetRML(String& content)
{
	// First we start the open tag, add the attributes then close the open tag.
	// Then comes the children in order, then we add our close tag.
	content += "<";
	content += tag;

	for (auto& pair : attributes)
	{
		const String& name = pair.first;
		if (name == "style")
			continue;

		const Variant& variant = pair.second;
		String value;
		if (variant.GetInto(value))
		{
			content += ' ';
			content += name;
			content += "=\"";
			content += value;
			content += "\"";
		}
	}

	const PropertyDictionary& local_properties = meta->style.GetLocalStyleProperties();
	if (!local_properties.Empty())
		content += " style=\"";

	for (const auto& [id, property] : local_properties.GetProperties())
	{
		// Skip placeholders for shorthands that are pending substitution, the shorthand itself is serialized below.
		if (property.unit == Unit::SHORTHAND_PLACEHOLDER)
			continue;
		content += StyleSheetSpecification::GetPropertyName(id);
		content += ": ";
		content += StringUtilities::EncodeRml(property.ToString());
		content += "; ";
	}
	for (const auto& [name, property] : local_properties.GetCustomProperties())
	{
		content += name;
		content += ": ";
		content += StringUtilities::EncodeRml(property.ToString());
		content += "; ";
	}
	for (const auto& [shorthand_id, property] : local_properties.GetVarShorthands())
	{
		content += StyleSheetSpecification::GetShorthandName(shorthand_id);
		content += ": ";
		content += StringUtilities::EncodeRml(property.ToString());
		content += "; ";
	}

	if (!local_properties.Empty())
		content.back() = '\"';

	if (HasChildNodes())
	{
		content += ">";

		GetInnerRML(content);

		content += "</";
		content += tag;
		content += ">";
	}
	else
	{
		content += " />";
	}
}

void Element::SetOwnerDocument(ElementDocument* document, bool force_set)
{
	if (owner_document == document)
		return;

	if (owner_document && !document)
	{
		// We are detaching from the document and thereby also the context.
		if (Context* context = owner_document->GetContext())
			context->OnElementDetach(this);
	}

	// If this element is a document, then never change owner_document, except if forced.
	if (owner_document != this || force_set)
	{
		owner_document = document;
		for (ElementPtr& child : children)
			child->SetOwnerDocument(document, false);
	}
}

void Element::SetDataModel(DataModel* new_data_model)
{
	RMLUI_ASSERTMSG(!data_model || !new_data_model, "We must either attach a new data model, or detach the old one.");

	if (data_model == new_data_model)
		return;

	// stop descent if a nested data model is encountered
	if (data_model && new_data_model && data_model != new_data_model)
		return;

	if (data_model)
		data_model->OnElementRemove(this);

	data_model = new_data_model;

	if (data_model)
		ElementUtilities::ApplyDataViewsControllers(this);

	for (ElementPtr& child : children)
		child->SetDataModel(new_data_model);
}

void Element::Release()
{
	if (instancer)
		instancer->ReleaseElement(this);
	else
		Log::Message(Log::LT_WARNING, "Leak detected: element %s not instanced via RmlUi Factory. Unable to release.", GetAddress().c_str());
}

void Element::SetParent(Element* _parent)
{
	// Assumes we are already detached from the hierarchy or we are detaching now.
	RMLUI_ASSERT(!parent || !_parent);

	parent = _parent;

	if (parent)
	{
		// We need to update our definition and make sure we inherit the properties of our new parent.
		DirtyDefinition(DirtyNodes::Self);
		meta->style.DirtyInheritedProperties();
	}

	// The transform state may require recalculation.
	if (transform_state || (parent && parent->transform_state))
		DirtyTransformState(true, true);

	SetOwnerDocument(parent ? parent->GetOwnerDocument() : nullptr, false);

	if (!parent)
	{
		if (data_model)
			SetDataModel(nullptr);
	}
	else
	{
		auto it = attributes.find("data-model");
		if (it == attributes.end())
		{
			SetDataModel(parent->data_model);
		}
		else if (Context* context = GetContext())
		{
			String name = it->second.Get<String>();

			if (DataModel* model = context->GetDataModelPtr(name))
			{
				model->AttachModelRootElement(this);
				SetDataModel(model);
			}
			else
				Log::Message(Log::LT_ERROR, "Could not locate data model '%s' in element %s.", name.c_str(), GetAddress().c_str());
		}
	}
}

void Element::DirtyAbsoluteOffset()
{
	if (!absolute_offset_dirty)
		DirtyAbsoluteOffsetRecursive();
}

void Element::DirtyAbsoluteOffsetRecursive()
{
	if (!absolute_offset_dirty)
	{
		absolute_offset_dirty = true;

		if (transform_state)
			DirtyTransformState(true, true);
	}

	for (size_t i = 0; i < children.size(); i++)
		children[i]->DirtyAbsoluteOffsetRecursive();
}

void Element::UpdateOffset()
{
	using namespace Style;
	const auto& computed = meta->computed_values;
	Position position_property = computed.position();

	if (position_property == Position::Absolute || position_property == Position::Fixed)
	{
		if (offset_parent != nullptr)
		{
			// The containing block: the offset parent's padding box, or the override (a grid area, the viewport), its
			// origin relative to the offset parent's border box.
			const Box& parent_box = offset_parent->GetBox();
			Vector2f containing_block = parent_box.GetSize(BoxArea::Padding);
			// (without the offset parent's scrollbars, as the layout sized this box: LayoutDetails::GetContainingBlock)
			containing_block.x = Math::Max(containing_block.x - offset_parent->meta->scroll.GetScrollbarSize(ElementScroll::VERTICAL), 0.f);
			containing_block.y = Math::Max(containing_block.y - offset_parent->meta->scroll.GetScrollbarSize(ElementScroll::HORIZONTAL), 0.f);
			Vector2f containing_block_origin = {parent_box.GetEdge(BoxArea::Border, BoxEdge::Left),
				parent_box.GetEdge(BoxArea::Border, BoxEdge::Top)};
			if (const Rectanglef* containing_block_override = GetContainingBlockOverride())
			{
				containing_block = containing_block_override->Size();
				containing_block_origin = containing_block_override->Position();
			}

			// If the element is anchored left, then the position is offset by that resolved value.
			if (computed.left().type != Left::Auto)
				relative_offset_base.x = containing_block_origin.x +
					(ResolveValue(computed.left(), containing_block.x) + GetBox().GetEdge(BoxArea::Margin, BoxEdge::Left));

			// If the element is anchored right, then the position is set first so the element's right-most edge
			// (including margins) will render up against the containing box's right-most content edge, and then
			// offset by the resolved value.
			else if (computed.right().type != Right::Auto)
			{
				relative_offset_base.x = containing_block.x + containing_block_origin.x -
					(ResolveValue(computed.right(), containing_block.x) + GetBox().GetSize(BoxArea::Border).x +
						GetBox().GetEdge(BoxArea::Margin, BoxEdge::Right));
			}

			// If the element is anchored top, then the position is offset by that resolved value.
			if (computed.top().type != Top::Auto)
			{
				relative_offset_base.y =
					containing_block_origin.y + (ResolveValue(computed.top(), containing_block.y) + GetBox().GetEdge(BoxArea::Margin, BoxEdge::Top));
			}

			// If the element is anchored bottom, then the position is set first so the element's right-most edge
			// (including margins) will render up against the containing box's right-most content edge, and then
			// offset by the resolved value.
			else if (computed.bottom().type != Bottom::Auto)
			{
				relative_offset_base.y = containing_block.y + containing_block_origin.y -
					(ResolveValue(computed.bottom(), containing_block.y) + GetBox().GetSize(BoxArea::Border).y +
						GetBox().GetEdge(BoxArea::Margin, BoxEdge::Bottom));
			}
		}
	}
	else if (position_property == Position::Sticky)
	{
		// Laid out as a relatively positioned box without a shift; its insets constrain it against the scrollport of its
		// scroll container instead (CSS Positioned Layout 3 §3.4). The shift depends on the scroll offsets: it is found with
		// the absolute offset (UpdateAbsoluteOffsetAndRenderBoxData).
		DirtyAbsoluteOffset();
	}
	else if (position_property == Position::Relative)
	{
		if (offset_parent != nullptr)
		{
			const Box& parent_box = offset_parent->GetBox();
			Vector2f containing_block = parent_box.GetSize();
			// Percentages of a containing block height that depends on its content resolve as 'auto' (CSS 2.1 §9.4.3).
			if (!LayoutDetails::HasDefiniteHeight(offset_parent))
				containing_block.y = -1.f;

			if (computed.left().type != Left::Auto)
				relative_offset_position.x = ResolveValue(computed.left(), containing_block.x);
			else if (computed.right().type != Right::Auto)
				relative_offset_position.x = -1 * ResolveValue(computed.right(), containing_block.x);
			else
				relative_offset_position.x = 0;

			auto resolve_vertical = [&](Style::LengthPercentageAuto value) {
				return (value.type == Style::LengthPercentageAuto::Length || containing_block.y >= 0.f) ? ResolveValue(value, containing_block.y)
																										: 0.f;
			};
			if (computed.top().type != Top::Auto)
				relative_offset_position.y = resolve_vertical(computed.top());
			else if (computed.bottom().type != Bottom::Auto)
				relative_offset_position.y = -1 * resolve_vertical(computed.bottom());
			else
				relative_offset_position.y = 0;
		}
	}
	else
	{
		relative_offset_position.x = 0;
		relative_offset_position.y = 0;
	}
}

void Element::SetBaseline(float in_baseline)
{
	baseline = in_baseline;
}

enum class RenderOrder {
	StackNegative, // Local stacking context with z < 0.
	Block,
	TableColumnGroup,
	TableColumn,
	TableRowGroup,
	TableRow,
	TableCell,
	Floating,
	Inline,
	Positioned,    // Positioned element, or local stacking context with z == 0.
	StackPositive, // Local stacking context with z > 0.
};
struct StackingContextChild {
	Element* element = nullptr;
	RenderOrder order = {};
	int tree_index = 0; // the position in tree order (before sorting)
};
static bool operator<(const StackingContextChild& lhs, const StackingContextChild& rhs)
{
	if (int(lhs.order) == int(rhs.order))
		return lhs.element->GetZIndex() < rhs.element->GetZIndex();
	return int(lhs.order) < int(rhs.order);
}

// Where the stacking context paints the line-clamp ellipses the block containers of a unit draw after an atomic inline or
// a <br> (fork, P6-L1): 'owner' and the blocks among the entries from 'index_begin' on, sorted in paint order, not placed
// yet (the blocks of an atomic unit inside are placed when it is made one). Chromium paints one with the line's items, in
// the inline content after the in-flow inline content before it in tree order: after the atomic inline it follows and
// its descendants, or, when that one is positioned or a stacking context (painted later, over it), before it. The
// block-level backgrounds, floats and negative stacking contexts are under it, positioned elements over it. The ellipsis
// is painted after the last entry under it ('paint_after'): 'owner' when none in an atomic unit (painted first), before
// every entry of a stacking context. An atomic unit's negative stacking contexts are painted outside it: never that entry.
static void StackingContext_PlaceLineClampEllipses(Vector<StackingContextChild>& stacking_children, size_t index_begin, Element* owner,
	bool is_stacking_context)
{
	const auto is_layered = [](RenderOrder order) {
		return order == RenderOrder::Positioned || order == RenderOrder::StackPositive || order == RenderOrder::StackNegative;
	};
	const auto is_descendant_or_self = [](const Element* element, const Element* ancestor) {
		for (const Element* e = element; e; e = e->GetParentNode())
		{
			if (e == ancestor)
				return true;
		}
		return false;
	};
	const auto place = [&](Element* block) {
		LineClampBlockEllipsis& ellipsis = *LineClampAccess::GetBlockEllipsis(block);
		ellipsis.placed = true;
		// the tree order the ellipsis follows: past the subtree of the element it follows when that one is painted in the
		// inline content, before it otherwise
		int limit = INT_MAX;
		for (size_t i = index_begin; i < stacking_children.size(); i++)
		{
			if (stacking_children[i].element != ellipsis.after)
				continue;
			limit = stacking_children[i].tree_index;
			if (!is_layered(stacking_children[i].order))
			{
				for (size_t j = index_begin; j < stacking_children.size(); j++)
				{
					if (stacking_children[j].tree_index >= limit && is_descendant_or_self(stacking_children[j].element, ellipsis.after))
						limit = stacking_children[j].tree_index + 1;
				}
			}
			break;
		}
		ellipsis.paint_after = (is_stacking_context ? nullptr : owner);
		for (size_t i = index_begin; i < stacking_children.size(); i++)
		{
			const RenderOrder order = stacking_children[i].order;
			if (order == RenderOrder::Positioned || order == RenderOrder::StackPositive)
				break;
			if (order == RenderOrder::StackNegative && !is_stacking_context)
				continue;
			if (order != RenderOrder::Inline || stacking_children[i].tree_index < limit)
				ellipsis.paint_after = stacking_children[i].element;
		}
	};
	if (LineClampBlockEllipsis* ellipsis = LineClampAccess::GetBlockEllipsis(owner))
	{
		if (!ellipsis->placed)
			place(owner);
	}
	for (size_t i = index_begin; i < stacking_children.size(); i++)
	{
		// (a block that is a stacking context of its own places its ellipsis in its own)
		Element* element = stacking_children[i].element;
		LineClampBlockEllipsis* ellipsis = LineClampAccess::GetBlockEllipsis(element);
		if (ellipsis && !ellipsis->placed && !LineClampAccess::IsStackingContext(element))
			place(element);
	}
}

// Treat all children in the range [index_begin, end) as if the parent created a new stacking context: they are sorted
// separately (their tree order kept in 'tree_index' to place the line-clamp ellipses 'element' and its block containers
// draw), then they all take their parent's paint order. However, positioned and descendants which create a new stacking
// context should be considered part of the parent stacking context. See CSS 2, Appendix E.
static void StackingContext_MakeAtomicRange(Vector<StackingContextChild>& stacking_children, size_t index_begin, RenderOrder parent_render_order,
	Element* element)
{
	bool any_line_clamp_ellipsis = LineClampAccess::HasBlockEllipsis(element);
	for (size_t i = index_begin; i < stacking_children.size(); i++)
	{
		stacking_children[i].tree_index = (int)i;
		any_line_clamp_ellipsis |= LineClampAccess::HasBlockEllipsis(stacking_children[i].element);
	}

	std::stable_sort(stacking_children.begin() + index_begin, stacking_children.end());

	if (any_line_clamp_ellipsis)
		StackingContext_PlaceLineClampEllipses(stacking_children, index_begin, element, false);

	for (auto it = stacking_children.begin() + index_begin; it != stacking_children.end(); ++it)
	{
		auto order = it->order;
		if (order != RenderOrder::StackNegative && order != RenderOrder::Positioned && order != RenderOrder::StackPositive)
			it->order = parent_render_order;
	}
}

void Element::BuildLocalStackingContext()
{
	stacking_context_dirty = false;

	Vector<StackingContextChild> stacking_children;
	if (draws_line_clamp_ellipsis)
		meta->line_clamp_ellipsis->placed = false;
	AddChildrenToStackingContext(stacking_children);
	bool any_line_clamp_ellipsis = draws_line_clamp_ellipsis;
	for (size_t i = 0; i < stacking_children.size(); i++)
	{
		stacking_children[i].tree_index = (int)i;
		any_line_clamp_ellipsis |= stacking_children[i].element->draws_line_clamp_ellipsis;
	}
	std::stable_sort(stacking_children.begin(), stacking_children.end());

	stacking_context.resize(stacking_children.size());
	for (size_t i = 0; i < stacking_children.size(); i++)
		stacking_context[i] = stacking_children[i].element;

	// The line-clamp ellipses the block containers of this stacking context draw (fork, P6-L1), painted after the entry
	// StackingContext_PlaceLineClampEllipses found (here, or when it made an atomic unit of the entries).
	meta->line_clamp_ellipses.reset();
	if (any_line_clamp_ellipsis)
	{
		StackingContext_PlaceLineClampEllipses(stacking_children, 0, this, true);
		Vector<std::pair<int, Element*>> ellipses;
		const auto add = [&](Element* block) {
			const Element* paint_after = block->meta->line_clamp_ellipsis->paint_after;
			int index = -1;
			if (paint_after)
			{
				const auto it = std::find(stacking_context.begin(), stacking_context.end(), paint_after);
				if (it != stacking_context.end())
					index = int(it - stacking_context.begin());
			}
			ellipses.emplace_back(index, block);
		};
		if (draws_line_clamp_ellipsis)
			add(this);
		for (Element* element : stacking_context)
		{
			if (element->draws_line_clamp_ellipsis && !element->local_stacking_context)
				add(element);
		}
		std::stable_sort(ellipses.begin(), ellipses.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		meta->line_clamp_ellipses = MakeUnique<Vector<std::pair<int, Element*>>>(std::move(ellipses));
	}

	// If the stacking context changed, any previously hidden elements may not have had their transforms updated. Ensure they will be updated now.
	if (transform_state)
	{
		for (Element* child : stacking_context)
			child->DirtyTransformState(false, true);
	}
}

void Element::AddChildrenToStackingContext(Vector<StackingContextChild>& stacking_children)
{
	// Inline SVG descendants are editable DOM nodes, painted together by the SVG rasterizer.
	if (tag == "svg")
		return;
	// Grid items paint like flex items (p3-grid): atomically, in 'order' order.
	const Style::Display display = GetDisplay();
	bool is_flex_container = (display == Style::Display::Flex || display == Style::Display::Grid || display == Style::Display::InlineGrid);

	// The children in the order they are laid out, which is CSS paint order: ::marker, ::before, the DOM children, ::after;
	// the children of a 'display: contents' child in its place; the items of a flex or grid container in 'order' order.
	ScopedLayoutChildList layout_children;
	LayoutDetails::GetLayoutChildren(this, *layout_children, is_flex_container);
	for (Element* child : *layout_children)
		child->AddToStackingContext(stacking_children, is_flex_container, false);

	// Then the other non-DOM children (scrollbars and the parts of widgets), each painted as a unit.
	const Element* pseudo_elements[] = {GetPseudoElement(PseudoElementType::Marker), GetPseudoElement(PseudoElementType::Before),
		GetPseudoElement(PseudoElementType::After)};
	const int num_children = (int)children.size();
	for (int i = num_children - num_non_dom_children; i < num_children; ++i)
	{
		Element* child = children[i].get();
		if (std::find(std::begin(pseudo_elements), std::end(pseudo_elements), child) == std::end(pseudo_elements))
			child->AddToStackingContext(stacking_children, is_flex_container, true);
	}
}

void Element::AddToStackingContext(Vector<StackingContextChild>& stacking_children, bool is_flex_item, bool is_non_dom_element)
{
	using Style::Display;

	// 'display: none' removes the subtree. A hidden element (CSS visibility) is skipped when painting it, but its
	// descendants that are visible paint: it stays in the list. Documents and the parts of widgets hide their whole subtree
	// with it (ElementDocument::Hide, scrollbars, select boxes), as in 6.3.
	if (meta->computed_values.display() == Display::None)
		return;
	if (meta->computed_values.visibility() != Style::Visibility::Visible && (local_stacking_context_forced || is_non_dom_element || !parent))
		return;
	// Truncated by a line-clamp ellipsis (fork, P6-L1): not painted, not hit, with its subtree.
	if (line_clamp_hidden)
		return;

	const Display display = GetDisplay();

	RenderOrder order = RenderOrder::Inline;
	bool include_children = true;
	bool render_as_atomic_unit = false;

	if (local_stacking_context)
	{
		if (z_index > 0.f)
			order = RenderOrder::StackPositive;
		else if (z_index < 0.f)
			order = RenderOrder::StackNegative;
		else
			order = RenderOrder::Positioned;

		include_children = false;
	}
	else if (display == Display::TableRow || display == Display::TableRowGroup || display == Display::TableColumn ||
		display == Display::TableColumnGroup)
	{
		// Handle internal display values taking priority over position and float.
		switch (display)
		{
		case Display::TableRow: order = RenderOrder::TableRow; break;
		case Display::TableRowGroup: order = RenderOrder::TableRowGroup; break;
		case Display::TableColumn: order = RenderOrder::TableColumn; break;
		case Display::TableColumnGroup: order = RenderOrder::TableColumnGroup; break;
		default: break;
		}
	}
	else if (GetPosition() != Style::Position::Static)
	{
		order = RenderOrder::Positioned;
		render_as_atomic_unit = true;
	}
	else if (GetFloat() != Style::Float::None)
	{
		order = RenderOrder::Floating;
		render_as_atomic_unit = true;
	}
	else
	{
		switch (display)
		{
		case Display::Block:
		case Display::FlowRoot:
		case Display::Table:
		case Display::Flex:
		case Display::Grid:
		case Display::ListItem:
			order = RenderOrder::Block;
			render_as_atomic_unit = (display == Display::Table || is_flex_item);
			break;

		case Display::Inline:
		case Display::InlineBlock:
		case Display::InlineFlex:
		case Display::InlineTable:
		case Display::InlineGrid:
			order = RenderOrder::Inline;
			render_as_atomic_unit = (display != Display::Inline || is_flex_item);
			break;

		case Display::TableCell:
			order = RenderOrder::TableCell;
			render_as_atomic_unit = true;
			break;

		case Display::TableRow:
		case Display::TableRowGroup:
		case Display::TableColumn:
		case Display::TableColumnGroup:
		case Display::Contents:                 // Never added: its children are added in its place.
		case Display::WebkitBox:                // (resolved by ElementStyle::ComputeValues, fork P6-L1)
		case Display::WebkitInlineBox:
		case Display::None: RMLUI_ERROR; break; // Handled above.
		}
	}

	if (is_non_dom_element)
		render_as_atomic_unit = true;

	stacking_children.push_back(StackingContextChild{this, order});
	if (draws_line_clamp_ellipsis)
		meta->line_clamp_ellipsis->placed = false;

	if (include_children && !children.empty())
	{
		const size_t index_child_begin = stacking_children.size();

		AddChildrenToStackingContext(stacking_children);

		if (render_as_atomic_unit)
			StackingContext_MakeAtomicRange(stacking_children, index_child_begin, order, this);
	}
}

void Element::DirtyStackingContext()
{
	if (Element* stacking_context_parent = ClosestStackingContextContainer())
		stacking_context_parent->stacking_context_dirty = true;
}

Element* Element::ClosestStackingContextContainer()
{
	// Find the first ancestor, or this, that has a local stacking context. That is our stacking context container.
	Element* stacking_context_parent = this;
	while (stacking_context_parent && !stacking_context_parent->local_stacking_context)
	{
		stacking_context_parent = stacking_context_parent->GetParentNode();
	}

	return stacking_context_parent;
}

void Element::DirtyDefinition(DirtyNodes dirty_nodes)
{
	switch (dirty_nodes)
	{
	case DirtyNodes::Self: dirty_definition = true; break;
	case DirtyNodes::SelfAndSiblings:
		dirty_definition = true;
		if (parent)
			parent->dirty_child_definitions = true;
		break;
	}
}

void Element::UpdateDefinition()
{
	if (dirty_definition)
	{
		dirty_definition = false;
		dirty_definition_self = false;

		// Dirty definition implies all our descendent elements. Anything that can change the definition of this element can also change the
		// definition of any descendants due to the presence of RCSS descendant or child combinators. In principle this also applies to sibling
		// combinators, but those are handled during the DirtyDefinition call.
		dirty_child_definitions = true;

		GetStyle()->UpdateDefinition();
	}
	else if (dirty_definition_self)
	{
		// The element alone may match differently (:has(), phase 3); its descendants are not affected.
		dirty_definition_self = false;
		GetStyle()->UpdateDefinition();
	}

	if (dirty_child_definitions)
	{
		dirty_child_definitions = false;
		for (const ElementPtr& child : children)
			child->dirty_definition = true;
	}
}

bool Element::Animate(const String& property_name, const Property& target_value, float duration, Tween tween, int num_iterations,
	bool alternate_direction, float delay, const Property* start_value)
{
	return Animate(StyleSheetSpecification::GetPropertyId(property_name), target_value, duration, tween, num_iterations, alternate_direction, delay,
		start_value);
}

bool Element::Animate(PropertyId id, const Property& target_value, float duration, Tween tween, int num_iterations, bool alternate_direction,
	float delay, const Property* start_value)
{
	bool result = false;
	auto it_animation = StartAnimation(id, start_value, num_iterations, alternate_direction, delay);
	if (it_animation != animations.end())
	{
		result = it_animation->AddKey(duration, target_value, *this, tween, true);
		if (!result)
			animations.erase(it_animation);
	}

	return result;
}

bool Element::AddAnimationKey(const String& property_name, const Property& target_value, float duration, Tween tween)
{
	return AddAnimationKey(StyleSheetSpecification::GetPropertyId(property_name), target_value, duration, tween);
}

bool Element::AddAnimationKey(PropertyId id, const Property& target_value, float duration, Tween tween)
{
	for (auto& existing_animation : animations)
	{
		if (existing_animation.GetOrigin() == ElementAnimationOrigin::User && existing_animation.GetPropertyId() == id)
			return existing_animation.AddKey(existing_animation.GetDuration() + duration, target_value, *this, tween, true);
	}
	return false;
}

ElementAnimationList::iterator Element::StartAnimation(PropertyId property_id, const Property* start_value, int num_iterations,
	bool alternate_direction, float delay)
{
	// A user animation replaces the user animation or the transition of the same property.
	for (auto it = animations.begin(); it != animations.end(); ++it)
	{
		if (it->GetOrigin() != ElementAnimationOrigin::Animation && it->GetPropertyId() == property_id)
		{
			if (it->IsTransition())
				meta->style.RemoveTransitionProperty(property_id);
			animations.erase(it);
			break;
		}
	}

	Property value;
	if (start_value)
	{
		value = *start_value;
		if (!value.definition)
			if (auto default_value = GetProperty(property_id))
				value.definition = default_value->definition;
	}
	else if (auto default_value = GetProperty(property_id))
	{
		value = *default_value;
	}

	if (!value.definition)
		return animations.end();

	const double start_time = Clock::GetElapsedTime() + (double)delay;
	animations.push_back(
		ElementAnimation{property_id, ElementAnimationOrigin::User, value, *this, start_time, 0.0f, num_iterations, alternate_direction});
	auto it = animations.end() - 1;
	if (!it->IsInitalized())
	{
		animations.erase(it);
		return animations.end();
	}
	animations_running = true;
	return it;
}

const ElementAnimation* Element::GetTransition(PropertyId id) const
{
	for (const ElementAnimation& animation : animations)
		if (animation.IsTransition() && !animation.IsCancelled() && animation.GetPropertyId() == id)
			return &animation;
	return nullptr;
}

namespace {
	struct PendingAnimationEvent {
		EventId id;
		Dictionary parameters;
	};

	void AddTransitionEvent(Vector<PendingAnimationEvent>& events, EventId id, PropertyId property_id, float elapsed_time)
	{
		Dictionary parameters;
		const String& name = StyleSheetSpecification::GetPropertyName(property_id);
		parameters.emplace("propertyName", Variant(name));
		parameters.emplace("elapsedTime", Variant(elapsed_time));
		parameters.emplace("pseudoElement", Variant(String()));
		// RmlUi 6.3's parameter.
		parameters.emplace("property", Variant(name));
		events.push_back(PendingAnimationEvent{id, std::move(parameters)});
	}

	void AddAnimationEvent(Vector<PendingAnimationEvent>& events, EventId id, const String& name, float elapsed_time)
	{
		Dictionary parameters;
		parameters.emplace("animationName", Variant(name));
		parameters.emplace("elapsedTime", Variant(elapsed_time));
		parameters.emplace("pseudoElement", Variant(String()));
		events.push_back(PendingAnimationEvent{id, std::move(parameters)});
	}

	// The elapsed time of a start event (CSS Animations 1, CSS Transitions 1): min(max(-delay, 0), active duration).
	float StartElapsedTime(const ElementAnimation& animation)
	{
		return Math::Min(Math::Max(-animation.GetDelay(), 0.f), animation.GetActiveDuration());
	}

	// Queues the events of a CSS animation or transition for the phase change since its last update.
	void QueueEvents(Vector<PendingAnimationEvent>& events, ElementAnimation& animation, const ElementAnimation::Sample& sample)
	{
		const AnimationPhase previous = animation.GetEventPhase();
		const AnimationPhase phase = sample.phase;
		const bool was_running = (previous == AnimationPhase::Active);
		const bool was_waiting = (previous == AnimationPhase::Idle || previous == AnimationPhase::Before);

		if (animation.IsTransition())
		{
			const PropertyId id = animation.GetPropertyId();
			if (previous == AnimationPhase::Idle)
				AddTransitionEvent(events, EventId::Transitionrun, id, StartElapsedTime(animation));
			if (was_waiting && (phase == AnimationPhase::Active || phase == AnimationPhase::After))
				AddTransitionEvent(events, EventId::Transitionstart, id, StartElapsedTime(animation));
			if ((was_waiting || was_running) && phase == AnimationPhase::After)
				AddTransitionEvent(events, EventId::Transitionend, id, animation.GetActiveDuration());
		}
		else if (animation.GetOrigin() == ElementAnimationOrigin::Animation)
		{
			const String& name = animation.GetName();
			if (was_waiting && (phase == AnimationPhase::Active || phase == AnimationPhase::After))
				AddAnimationEvent(events, EventId::Animationstart, name, StartElapsedTime(animation));
			if (was_running && phase == AnimationPhase::Active && sample.iteration != animation.GetEventIteration())
				AddAnimationEvent(events, EventId::Animationiteration, name, float(sample.iteration * animation.GetDuration()));
			if ((was_waiting || was_running) && phase == AnimationPhase::After)
				AddAnimationEvent(events, EventId::Animationend, name, animation.GetActiveDuration());
		}

		if (phase == AnimationPhase::After && previous == AnimationPhase::After)
			return;
		animation.SetEventState(phase, sample.iteration);
	}

	// The cancel event of a CSS animation or transition removed before its end.
	void QueueCancelEvent(Vector<PendingAnimationEvent>& events, const ElementAnimation& animation, double world_time)
	{
		const AnimationPhase previous = animation.GetEventPhase();
		if (previous == AnimationPhase::After)
			return;
		const float elapsed_time = animation.GetSample(world_time).elapsed_time;
		if (animation.IsTransition())
		{
			if (previous != AnimationPhase::Idle)
				AddTransitionEvent(events, EventId::Transitioncancel, animation.GetPropertyId(), elapsed_time);
		}
		else if (animation.GetOrigin() == ElementAnimationOrigin::Animation && previous != AnimationPhase::Idle)
			AddAnimationEvent(events, EventId::Animationcancel, animation.GetName(), elapsed_time);
	}
} // namespace

bool Element::StartTransition(const Transition& transition, const Property& start_value, const Property& target_value,
	const Property& reversing_adjusted_start, float reversing_shortening_factor)
{
	// A user animation of the property takes precedence.
	for (const ElementAnimation& animation : animations)
		if (animation.GetOrigin() == ElementAnimationOrigin::User && animation.GetPropertyId() == transition.id)
			return false;

	CancelTransition(transition.id);

	ElementAnimation animation{transition.id, ElementAnimationOrigin::Transition, start_value, *this, Clock::GetElapsedTime(), 0.0f, 1, false};
	if (!animation.IsInitalized() || !animation.AddKey(transition.duration, target_value, *this, transition.tween, true))
		return false;
	animation.SetTransitionData(transition, reversing_adjusted_start, reversing_shortening_factor);
	animations.push_back(std::move(animation));
	animations_running = true;

	// The start value applies at once, also during the delay.
	meta->style.SetTransitionProperty(transition.id, start_value);
	return true;
}

void Element::CancelTransition(PropertyId id)
{
	// The cancel event is dispatched by the next AdvanceAnimations: this may run while the element's style is computed.
	for (ElementAnimation& animation : animations)
	{
		if (animation.IsTransition() && !animation.IsCancelled() && animation.GetPropertyId() == id)
		{
			animation.SetCancelled();
			meta->style.RemoveTransitionProperty(id);
			animations_running = true;
			return;
		}
	}
}

void Element::HandleTransitionProperty()
{
	if (!dirty_transition)
		return;
	dirty_transition = false;

	// Cancel the transitions of the properties the element's transition list no longer covers.
	const TransitionList* transition_list = GetComputedValues().transition();
	const bool displayed = animation_displayed;
	Vector<PropertyId> cancel;
	for (const ElementAnimation& animation : animations)
	{
		if (animation.IsTransition() && !animation.IsCancelled() &&
			(!displayed || !transition_list || !transition_list->Find(animation.GetPropertyId())))
			cancel.push_back(animation.GetPropertyId());
	}
	for (PropertyId id : cancel)
		CancelTransition(id);
}

void Element::HandleAnimationProperty()
{
	if (!dirty_animation)
		return;
	dirty_animation = false;

	const double now = Clock::GetElapsedTime();
	const AnimationList* animation_list = (animation_displayed ? meta->computed_values.animation() : nullptr);
	const StyleSheet* stylesheet = (animation_list ? GetStyleSheet() : nullptr);

	// The CSS animations as the list wants them: matched to the running ones by name and occurrence of the name, in the order of the list.
	ElementAnimationList css_animations;
	for (auto it = animations.begin(); it != animations.end();)
	{
		if (it->GetOrigin() == ElementAnimationOrigin::Animation)
		{
			css_animations.push_back(std::move(*it));
			it = animations.erase(it);
		}
		else
			++it;
	}

	ElementAnimationList result;
	if (animation_list && stylesheet)
	{
		Property property_storage;
		SmallUnorderedMap<String, int> occurrences;
		for (const Animation& animation : *animation_list)
		{
			const int occurrence = occurrences[animation.name]++;
			auto existing = std::find_if(css_animations.begin(), css_animations.end(),
				[&](const ElementAnimation& running) { return running.GetName() == animation.name && running.GetOccurrence() == occurrence; });
			if (existing != css_animations.end())
			{
				// A change of the play state, duration, ... updates the running animation.
				if (existing->GetAnimation() != animation)
					existing->UpdateTiming(animation, now);
				result.push_back(std::move(*existing));
				css_animations.erase(existing);
				continue;
			}

			const Keyframes* keyframes = stylesheet->GetKeyframes(animation.name);
			if (!keyframes || keyframes->blocks.empty())
				continue;

			ElementAnimation css_animation(animation, occurrence, now);
			const auto& blocks = keyframes->blocks;
			SmallUnorderedSet<String> variable_dependencies;
			std::optional<PropertyDictionary> substituted_shorthands_cache;
			int previous_block_index = -1;
			auto ResolveProperty = [&](PropertyId id, const Property* property, int block_index) {
				if (block_index != previous_block_index)
				{
					substituted_shorthands_cache.reset();
					previous_block_index = block_index;
				}
				return meta->style.ResolveKeyFrameProperty(id, property, blocks[block_index].properties, substituted_shorthands_cache,
					variable_dependencies, property_storage);
			};

			// One track per property, with the keyframes that set it; a missing 0% or 100% keyframe takes the underlying value.
			for (PropertyId id : keyframes->property_ids)
			{
				AnimationTrack track(id);
				bool valid = true;
				auto AddUnderlying = [&](float offset) {
					const Property* underlying = meta->style.GetUnderlyingProperty(id);
					if (!underlying || !track.AddKey(offset, *underlying, *this, Tween{}, false))
						valid = false;
				};
				for (int i = 0; i < (int)blocks.size() && valid; i++)
				{
					const Property* property = blocks[i].properties.GetProperty(id);
					if (!property)
						continue;
					if (track.GetKeys().empty() && blocks[i].normalized_time > 0.f)
						AddUnderlying(0.f);
					property = ResolveProperty(id, property, i);
					if (!property || !track.AddKey(blocks[i].normalized_time, *property, *this, blocks[i].tween, blocks[i].has_tween))
						valid = false;
				}
				if (valid && (track.GetKeys().empty() || track.GetKeys().back().time < 1.f))
					AddUnderlying(1.f);
				if (valid)
					css_animation.AddTrack(std::move(track));
			}
			result.push_back(std::move(css_animation));
		}
	}

	// The animations no longer in the list are cancelled.
	Vector<PendingAnimationEvent> events;
	for (const ElementAnimation& removed : css_animations)
	{
		QueueCancelEvent(events, removed, now);
		for (size_t i = 0; i < removed.GetNumTracks(); i++)
			meta->style.RemoveAnimatedProperty(removed.GetTrack(i).GetPropertyId());
	}

	for (ElementAnimation& animation : result)
		animations.push_back(std::move(animation));
	if (!result.empty())
		animations_running = true;

	for (PendingAnimationEvent& event : events)
		DispatchEvent(event.id, event.parameters);
}

void Element::AdvanceAnimations()
{
	if (animations.empty())
	{
		animations_running = false;
		return;
	}

	const double time = Clock::GetElapsedTime();
	Vector<PendingAnimationEvent> events;
	bool running = false;

	// The values of the CSS animations, the later animations of the list over the earlier ones.
	SmallUnorderedMap<PropertyId, Property> animated_values;
	PropertyIdSet animated_ids;

	for (ElementAnimation& animation : animations)
	{
		if (animation.IsCancelled())
		{
			QueueCancelEvent(events, animation, time);
			continue;
		}
		if (animation.GetOrigin() == ElementAnimationOrigin::User)
		{
			Property property = animation.UpdateAndGetProperty(time, *this);
			if (property.unit != Unit::UNKNOWN)
				SetProperty(animation.GetPropertyId(), property);
			if (!animation.IsComplete())
				running = true;
			continue;
		}

		const ElementAnimation::Sample sample = animation.GetSample(time);
		QueueEvents(events, animation, sample);
		if (!animation.IsPaused() && sample.phase != AnimationPhase::After)
			running = true;

		if (animation.IsTransition())
		{
			animation.RecordProgress(sample);
			if (sample.phase == AnimationPhase::After)
			{
				animation.SetComplete(true);
				continue;
			}
			Property property = animation.GetValue(0, sample, *this);
			if (property.unit != Unit::UNKNOWN)
			{
				property.definition = StyleSheetSpecification::GetProperty(animation.GetPropertyId());
				const PropertyDictionary* layer = meta->style.GetTransitionProperties();
				const Property* current = (layer ? layer->GetProperty(animation.GetPropertyId()) : nullptr);
				if (!current || *current != property)
					meta->style.SetTransitionProperty(animation.GetPropertyId(), property);
			}
			continue;
		}

		for (size_t i = 0; i < animation.GetNumTracks(); i++)
		{
			const PropertyId id = animation.GetTrack(i).GetPropertyId();
			animated_ids.Insert(id);
			Property property = animation.GetValue(i, sample, *this);
			if (property.unit != Unit::UNKNOWN)
			{
				property.definition = StyleSheetSpecification::GetProperty(id);
				animated_values[id] = std::move(property);
			}
		}
	}

	// Apply the CSS animations' values to the animation layer, and remove those no animation applies now.
	{
		const PropertyDictionary* layer = meta->style.GetAnimatedProperties();
		for (const PropertyId id : animated_ids)
		{
			const Property* current = (layer ? layer->GetProperty(id) : nullptr);
			auto it = animated_values.find(id);
			if (it == animated_values.end())
			{
				if (current)
					meta->style.RemoveAnimatedProperty(id);
			}
			else if (!current || *current != it->second)
				meta->style.SetAnimatedProperty(id, it->second);
		}
	}

	// Remove the user animations and transitions that reached their end.
	Vector<Dictionary> user_end_events;
	for (auto it = animations.begin(); it != animations.end();)
	{
		if (it->IsCancelled())
		{
			it = animations.erase(it);
			continue;
		}
		if (!it->IsComplete() || it->GetOrigin() == ElementAnimationOrigin::Animation)
		{
			++it;
			continue;
		}
		if (it->IsTransition())
			meta->style.RemoveTransitionProperty(it->GetPropertyId());
		else
		{
			// RmlUi 6.3's event of a user animation.
			Dictionary parameters;
			parameters.emplace("property", Variant(StyleSheetSpecification::GetPropertyName(it->GetPropertyId())));
			user_end_events.push_back(std::move(parameters));
		}
		it = animations.erase(it);
	}

	animations_running = running;

	// Events are dispatched last, as their listeners may change the element.
	for (Dictionary& parameters : user_end_events)
		DispatchEvent(EventId::Animationend, parameters);
	for (PendingAnimationEvent& event : events)
		DispatchEvent(event.id, event.parameters);
}

bool Element::UpdateAnimationDisplay()
{
	const bool displayed = (meta->computed_values.display() != Style::Display::None) && (!parent || parent->animation_displayed);
	if (displayed == animation_displayed)
		return false;
	animation_displayed = displayed;

	// CSS: an element (or an ancestor) set to 'display: none' cancels its animations and transitions; shown again, its animations start
	// from their beginning, and its transitions from its @starting-style.
	if (meta->computed_values.animation() || !animations.empty())
		dirty_animation = true;
	if (!animations.empty())
		dirty_transition = true;
	return displayed && meta->style.RestartTransitionsFromStartingStyle();
}

void Element::UpdateTransformStates()
{
	// Parents first: an element combines its transform with its parent's state. Everything is marked dirty, since a
	// new layout moves boxes without dirtying the transforms that depend on their position.
	DirtyTransformState(true, true);
	UpdateTransformState();
	const int num_children = GetNumChildren(true);
	for (int i = 0; i < num_children; i++)
		GetChild(i)->UpdateTransformStates();
}

void Element::DirtyTransformState(bool perspective_dirty, bool transform_dirty)
{
	dirty_perspective |= perspective_dirty;
	dirty_transform |= transform_dirty;
}

void Element::UpdateTransformState()
{
	if (!dirty_perspective && !dirty_transform)
		return;

	const ComputedValues& computed = meta->computed_values;

	const Vector2f pos = GetAbsoluteOffset(BoxArea::Border);
	const Vector2f size = GetBox().GetSize(BoxArea::Border);

	bool perspective_or_transform_changed = false;

	if (dirty_perspective)
	{
		// If perspective is set on this element, then it applies to our children. We just calculate it here,
		// and let the children's transform update merge it with their transform.
		bool had_perspective = (transform_state && transform_state->GetLocalPerspective());

		float distance = computed.perspective();
		Vector2f vanish = Vector2f(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
		bool have_perspective = false;

		if (distance > 0.0f)
		{
			have_perspective = true;

			// Compute the vanishing point from the perspective origin
			vanish.x = pos.x + ResolveValue(computed.perspective_origin_x(), size.x);
			vanish.y = pos.y + ResolveValue(computed.perspective_origin_y(), size.y);
		}

		if (have_perspective)
		{
			// Equivalent to: Translate(x,y,0) * Perspective(distance) * Translate(-x,-y,0)
			Matrix4f perspective = Matrix4f::FromRows( //
				{1, 0, -vanish.x / distance, 0},       //
				{0, 1, -vanish.y / distance, 0},       //
				{0, 0, 1, 0},                          //
				{0, 0, -1 / distance, 1}               //
			);

			if (!transform_state)
				transform_state = MakeUnique<TransformState>();

			perspective_or_transform_changed |= transform_state->SetLocalPerspective(&perspective);
		}
		else if (transform_state)
			transform_state->SetLocalPerspective(nullptr);

		perspective_or_transform_changed |= (have_perspective != had_perspective);

		dirty_perspective = false;
	}

	if (dirty_transform)
	{
		// We want to find the accumulated transform given all our ancestors. It is assumed here that the parent transform is already updated,
		// so that we only need to consider our local transform and combine it with our parent's transform and perspective matrices.
		bool had_transform = (transform_state && transform_state->GetTransform());

		bool have_transform = false;
		Matrix4f transform = Matrix4f::Identity();

		// The individual transform properties, then 'transform' (CSS Transforms 2): translate * rotate * scale * transform.
		const TransformPtr transform_parts[] = {computed.translate(), computed.rotate(), computed.scale(), computed.transform()};
		for (const TransformPtr& transform_ptr : transform_parts)
		{
			if (!transform_ptr)
				continue;
			// First find the current element's transform
			const int n = transform_ptr->GetNumPrimitives();
			for (int i = 0; i < n; ++i)
			{
#ifdef RMLUI_MATH_EXPRESSIONS
				const TransformPrimitive primitive = transform_ptr->ResolvePrimitive(i, *this);
#else
				const TransformPrimitive& primitive = transform_ptr->GetPrimitive(i);
#endif
				Matrix4f matrix = TransformUtilities::ResolveTransform(primitive, *this);
				transform *= matrix;
				have_transform = true;
			}
		}

		if (have_transform)
		{
			// Compute the transform origin
			Vector3f transform_origin(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f, 0);

			transform_origin.x = pos.x + ResolveValue(computed.transform_origin_x(), size.x);
			transform_origin.y = pos.y + ResolveValue(computed.transform_origin_y(), size.y);

			transform_origin.z = computed.transform_origin_z();

			// Make the transformation apply relative to the transform origin
			transform = Matrix4f::Translate(transform_origin) * transform * Matrix4f::Translate(-transform_origin);
		}

		// We may want to include the local offsets here, as suggested by the CSS specs, so that the local transform is applied after the offset I
		// believe the motivation is. Then we would need to subtract the absolute zero-offsets during geometry submit whenever we have transforms.

		Element* stacking_parent = parent ? parent->ClosestStackingContextContainer() : nullptr;
		if (stacking_parent && stacking_parent->transform_state)
		{
			// Apply the parent's local perspective and transform.
			// @performance: If we have no local transform and no parent perspective, we can effectively just point to the parent transform instead of
			// copying it.
			const TransformState& parent_state = *stacking_parent->transform_state;

			if (auto parent_perspective = parent_state.GetLocalPerspective())
			{
				transform = *parent_perspective * transform;
				have_transform = true;
			}

			if (auto parent_transform = parent_state.GetTransform())
			{
				transform = *parent_transform * transform;
				have_transform = true;
			}
		}

		if (have_transform)
		{
			if (!transform_state)
				transform_state = MakeUnique<TransformState>();

			perspective_or_transform_changed |= transform_state->SetTransform(&transform);
		}
		else if (transform_state)
			transform_state->SetTransform(nullptr);

		perspective_or_transform_changed |= (had_transform != have_transform);

		// Fork (S16-F6): done until something dirties it again (RmlUi 6.3 kept the flag set: once dirtied, an element
		// computed its transform anew at every render, and it is dirtied again whenever an input of it changes:
		// a transform property, its offset or box while it has a transform state, its stacking parent's transform)
		if (RenderManager::LastPaintShortcuts())
			dirty_transform = false;
	}

	// A change in perspective or transform will require an update to children transforms as well.
	if (perspective_or_transform_changed)
	{
		// Fork (S16-F6): and changes the clipping regions of the elements it clips
		if (RenderManager* render_manager = GetRenderManager())
			render_manager->ResetClipMemo();

		for (Element* stacking_child : stacking_context)
			stacking_child->DirtyTransformState(false, true);
	}

	// No reason to keep the transform state around if transform and perspective have been removed.
	if (transform_state && !transform_state->GetTransform() && !transform_state->GetLocalPerspective())
	{
		transform_state.reset();
	}
}

void Element::OnStyleSheetChangeRecursive()
{
	meta->effects.DirtyEffects();

	OnStyleSheetChange();

	// Now dirty all of our descendants.
	const int num_children = GetNumChildren(true);
	for (int i = 0; i < num_children; ++i)
		GetChild(i)->OnStyleSheetChangeRecursive();
}

void Element::OnDpRatioChangeRecursive()
{
	meta->effects.DirtyEffects();
	GetStyle()->DirtyPropertiesWithUnits(Unit::DP_SCALABLE_LENGTH);

	OnDpRatioChange();

	// Now dirty all of our descendants.
	const int num_children = GetNumChildren(true);
	for (int i = 0; i < num_children; ++i)
		GetChild(i)->OnDpRatioChangeRecursive();
}

void Element::DirtyFontFaceRecursive()
{
	// Dirty the font size to force the element to update the face handle during the next Update(), and update any existing text geometry.
	meta->style.DirtyProperty(PropertyId::FontSize);
	meta->computed_values.font_face_handle(0);

	const int num_children = GetNumChildren(true);
	for (int i = 0; i < num_children; ++i)
		GetChild(i)->DirtyFontFaceRecursive();
}

void Element::ClampScrollOffset()
{
	const Vector2f new_scroll_offset = {
		Math::Round(Math::Min(scroll_offset.x, GetScrollWidth() - GetClientWidth())),
		Math::Round(Math::Min(scroll_offset.y, GetScrollHeight() - GetClientHeight())),
	};

	if (new_scroll_offset != scroll_offset)
	{
		scroll_offset = new_scroll_offset;
		DirtyAbsoluteOffset();
	}

	// At this point the scrollbars have been resolved, both in terms of size and visibility. Update their properties
	// now so that any visibility changes in particular are reflected immediately on the next render. Otherwise we risk
	// that the scrollbars renders a frame late, since changes to scrollbars can happen during layouting.
	meta->scroll.UpdateProperties();
}

void Element::ClampScrollOffsetRecursive()
{
	ClampScrollOffset();
	const int num_children = GetNumChildren();
	for (int i = 0; i < num_children; ++i)
		GetChild(i)->ClampScrollOffsetRecursive();
}

} // namespace Rml
