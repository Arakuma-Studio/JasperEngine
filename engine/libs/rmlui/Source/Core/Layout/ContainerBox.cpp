#include "ContainerBox.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Context.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/ElementScroll.h"
#include "../../../Include/RmlUi/Core/Profiling.h"
#include "../ElementMeta.h"
#include "../WillChange.h"
#include "FlexFormattingContext.h"
#include "FormattingContext.h"
#include "GridFormattingContext.h"
#include "LayoutDetails.h"
#include <algorithm>
#include <cmath>

namespace Rml {

void ContainerBox::ResetScrollbars(const Box& box)
{
	RMLUI_ASSERT(element);
	if (overflow_x == Style::Overflow::Scroll)
		element->GetElementScroll()->EnableScrollbar(ElementScroll::HORIZONTAL, box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Padding));
	else
		element->GetElementScroll()->DisableScrollbar(ElementScroll::HORIZONTAL);

	// 'scrollbar-gutter: stable' keeps the space of the vertical scrollbar of a scroll container whether or not it overflows
	// (CSS Overflow 3 §4; 'both-edges' is not mirrored). The scrollbar is shown when the content overflows (CatchOverflow).
	const bool stable_gutter = (element->GetComputedValues().scrollbar_gutter() != Style::ScrollbarGutter::Auto &&
		(overflow_y == Style::Overflow::Auto || overflow_y == Style::Overflow::Hidden));
	if (overflow_y == Style::Overflow::Scroll || stable_gutter)
		element->GetElementScroll()->EnableScrollbar(ElementScroll::VERTICAL, box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Padding),
			stable_gutter);
	else
		element->GetElementScroll()->DisableScrollbar(ElementScroll::VERTICAL);
}

void ContainerBox::AddAbsoluteElement(Element* element, Vector2f static_position, Element* static_relative_offset_parent,
	std::optional<Rectanglef> containing_block, std::optional<StaticPositionArea> static_position_area)
{
	// 'position: fixed' with no ancestor that is a containing block for fixed boxes (LayoutDetails::GetContainingBlock then
	// gives the outermost box, the document): its containing block is the viewport, the context's dimensions at the origin
	// of the document's border box (the canvas origin for a document that fills the context).
	bool viewport_fixed = false;
	if (!containing_block && !is_fixed_positioning_containing_block && element->GetPosition() == Style::Position::Fixed)
	{
		if (Context* context = element->GetContext())
		{
			containing_block = Rectanglef::FromPositionSize(Vector2f(0.f), Vector2f(context->GetDimensions()));
			viewport_fixed = true;
		}
	}

	// We may possibly be adding the same element from a previous layout iteration. If so, this ensures it is updated with the latest static position.
	absolute_elements[element] =
		AbsoluteElement{static_position, static_relative_offset_parent, containing_block, viewport_fixed, static_position_area};
}

void ContainerBox::AddRelativeElement(Element* element)
{
	// The same relative element may be added multiple times during repeated layout iterations, avoid any duplicates.
	if (std::find(relative_elements.begin(), relative_elements.end(), element) == relative_elements.end())
		relative_elements.push_back(element);
}

void ContainerBox::ShiftStaticPositions(Element* static_relative_offset_parent, Vector2f delta)
{
	for (auto& absolute_element : absolute_elements)
	{
		if (absolute_element.second.static_position_offset_parent == static_relative_offset_parent)
			absolute_element.second.static_position += delta;
	}
}

bool ContainerBox::IsScrollContainer() const
{
	return LayoutDetails::IsScrollContainer(overflow_x, overflow_y);
}

void ContainerBox::ClosePositionedElements()
{
	// Any relatively positioned elements that we act as containing block for may need to be have their positions
	// updated to reflect changes to the size of this block box. Update relative offsets before handling absolute
	// elements, as this may affect the resolved static position of the absolute elements.
	for (Element* child : relative_elements)
		child->UpdateOffset();

	relative_elements.clear();

	while (!absolute_elements.empty())
	{
		// New absolute elements may be added to this box during formatting below. To avoid invalidated iterators and
		// references, move the list to a local copy to iterate over, and repeat if new elements are added.
		AbsoluteElementMap absolute_elements_iterate = std::move(absolute_elements);
		absolute_elements.clear();

		for (const auto& absolute_element_pair : absolute_elements_iterate)
		{
			Element* absolute_element = absolute_element_pair.first;
			const Vector2f static_position = absolute_element_pair.second.static_position;
			Element* static_position_offset_parent = absolute_element_pair.second.static_position_offset_parent;

			// Find the static position relative to this containing block. First, calculate the offset from ourself to
			// the static position's offset parent. Assumes (1) that this container box is part of the containing block
			// chain of the static position offset parent, and (2) that all offsets in this chain has been set already.
			Vector2f relative_position;
			for (Element* ancestor = static_position_offset_parent; ancestor && ancestor != element; ancestor = ancestor->GetOffsetParent())
				relative_position += ancestor->GetRelativeOffset(BoxArea::Border);

			// Now simply add the result to the stored static position to get the static position in our local space.
			Vector2f offset = relative_position + static_position;

			// Lay out the element, against its overriding containing block if it has one (also for Element::UpdateOffset and
			// Element::GetContainingBlock).
			std::optional<Rectanglef> containing_block = absolute_element_pair.second.containing_block;
			if (!containing_block)
				containing_block = GetAbsoluteContainingBlock(absolute_element);
			absolute_element->meta->containing_block_override = containing_block;
			if (containing_block)
			{
				Box box;
				LayoutDetails::BuildBox(box, containing_block->Size(), absolute_element);
				FormattingContext::FormatIndependent(this, absolute_element, &box, FormattingContextType::Block);
			}
			else
			{
				FormattingContext::FormatIndependent(this, absolute_element, nullptr, FormattingContextType::Block);
			}

			// Now that the element's box has been built, we can offset the position we determined was appropriate for
			// it by the element's margin. This is necessary because the coordinate system for the box begins at the
			// border, not the margin.
			const Box& absolute_box = absolute_element->GetBox();
			offset.x += absolute_box.GetEdge(BoxArea::Margin, BoxEdge::Left);
			offset.y += absolute_box.GetEdge(BoxArea::Margin, BoxEdge::Top);

			// Aligned in the area of its static position, now that its size is known.
			if (const auto& area = absolute_element_pair.second.static_position_area)
			{
				const Vector2f margin_size = absolute_box.GetSize(BoxArea::Margin);
				const Vector2f free_space = area->rectangle.Size() - margin_size;
				for (int axis = 0; axis < 2; axis++)
				{
					float shift = 0.f;
					if (area->alignment[axis] == StaticAlignment::Center)
						shift = 0.5f * free_space[axis];
					else if (area->alignment[axis] == StaticAlignment::End)
						shift = free_space[axis];
					if (area->safe[axis] && free_space[axis] < 0.f)
						shift = 0.f;
					offset[axis] += shift;
				}
			}

			// Set the offset of the element; the element itself will take care of any RCSS-defined positional offsets.
			absolute_element->SetOffset(offset, element, absolute_element_pair.second.viewport_fixed);

			// Its box is scrollable overflow of the nearest scroll container from us (a box fixed to the viewport scrolls with
			// nothing).
			if (!absolute_element_pair.second.viewport_fixed)
			{
				for (ContainerBox* container = this; container; container = container->parent_container)
				{
					if (container->element && container->IsScrollContainer())
					{
						container->absolute_overflow_elements.push_back(absolute_element);
						break;
					}
				}
			}
		}
	}
}

bool ContainerBox::CatchAbsoluteOverflow()
{
	if (absolute_overflow_elements.empty())
		return true;
	ElementList overflow_elements = std::move(absolute_overflow_elements);
	absolute_overflow_elements.clear();
	if (!element || !IsScrollContainer())
		return true;

	// The extent of the boxes from our padding box's top-left corner (unscrolled): their border boxes, with their own
	// overflow when they do not clip it. The offset parents up to us are the boxes they were laid out in, none of which
	// scrolls (we are the nearest scroll container).
	const Box& box = element->GetBox();
	const Vector2f border_top_left = {box.GetEdge(BoxArea::Border, BoxEdge::Left), box.GetEdge(BoxArea::Border, BoxEdge::Top)};
	Vector2f extent;
	bool found = false;
	for (Element* absolute_element : overflow_elements)
	{
		Vector2f position;
		Element* ancestor = absolute_element;
		for (; ancestor && ancestor != element; ancestor = ancestor->GetOffsetParent())
			position += ancestor->GetRelativeOffset(BoxArea::Border);
		if (ancestor != element || absolute_element->GetDisplay() == Style::Display::None)
			continue;

		const Box& absolute_box = absolute_element->GetBox();
		Vector2f size = absolute_box.GetSize(BoxArea::Border);
		const auto& computed = absolute_element->GetComputedValues();
		if (!LayoutDetails::IsScrollContainer(computed.overflow_x(), computed.overflow_y()) && computed.overflow_x() != Style::Overflow::Clip &&
			computed.overflow_y() != Style::Overflow::Clip)
		{
			const Vector2f absolute_border_top_left = {absolute_box.GetEdge(BoxArea::Border, BoxEdge::Left),
				absolute_box.GetEdge(BoxArea::Border, BoxEdge::Top)};
			size = Math::Max(size, absolute_border_top_left + Vector2f(absolute_element->GetScrollWidth(), absolute_element->GetScrollHeight()));
		}
		extent = Math::Max(extent, position - border_top_left + size);
		found = true;
	}
	if (!found)
		return true;

	const Vector2f scrollable_overflow = {element->GetScrollWidth(), element->GetScrollHeight()};
	if (extent.x <= scrollable_overflow.x + 0.5f && extent.y <= scrollable_overflow.y + 0.5f)
		return true;
	element->SetScrollableOverflowRectangle(Math::Max(scrollable_overflow, extent), false);

	// 'overflow: auto' shows a scrollbar for them as for in-flow overflow (CatchOverflow).
	ElementScroll* element_scroll = element->GetElementScroll();
	const float padding_width = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Padding);
	bool scrollbar_enabled = false;
	if (overflow_x == Style::Overflow::Auto && extent.x > element->GetClientWidth() + 0.5f &&
		element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL) == 0.f)
	{
		element_scroll->EnableScrollbar(ElementScroll::HORIZONTAL, padding_width);
		scrollbar_enabled |= (element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL) != 0.f);
	}
	if (overflow_y == Style::Overflow::Auto && extent.y > element->GetClientHeight() + 0.5f)
	{
		if (element_scroll->GetScrollbarSize(ElementScroll::VERTICAL) == 0.f)
		{
			element_scroll->EnableScrollbar(ElementScroll::VERTICAL, padding_width);
			scrollbar_enabled |= (element_scroll->GetScrollbarSize(ElementScroll::VERTICAL) != 0.f);
		}
		else if (element_scroll->IsScrollbarGutterOnly(ElementScroll::VERTICAL))
		{
			element_scroll->EnableScrollbar(ElementScroll::VERTICAL, padding_width);
		}
	}
	if (scrollbar_enabled)
		return false;

	// The scrollbars' thumbs follow the larger overflow.
	element_scroll->FormatScrollbars();
	return true;
}

std::optional<Rectanglef> ContainerBox::GetAbsoluteContainingBlock(Element* /*element*/) const
{
	return std::nullopt;
}

void ContainerBox::SetElementBaseline(float element_baseline)
{
	element->SetBaseline(element_baseline);
}

void ContainerBox::SubmitElementLayout()
{
	element->OnLayout();
}

ContainerBox::ContainerBox(Type type, Element* element, ContainerBox* parent_container) :
	LayoutBox(type), element(element), parent_container(parent_container)
{
	if (element)
	{
		const auto& computed = element->GetComputedValues();
		overflow_x = computed.overflow_x();
		overflow_y = computed.overflow_y();
		is_fixed_positioning_containing_block = WillChange::IsFixedPositioningContainingBlock(computed);
		is_absolute_positioning_containing_block = WillChange::IsAbsolutePositioningContainingBlock(computed);
	}
}

Vector2f ContainerBox::GetScrollableOverflowContentSize(Vector2f visible_overflow_size, Vector2f scrollable_content_size,
	Vector2f padding_bottom_right) const
{
	// The scrollable overflow content size is the largest of:
	//     1. Visible overflow: Descendant boxes whose border box overflow outside the padding area of this box.
	//     2. Scrollable content: Margin boxes of our direct descendant overflowing onto the padding box of this box.
	return Math::Max(visible_overflow_size, scrollable_content_size + padding_bottom_right);
}

bool ContainerBox::CatchOverflow(const Vector2f visible_overflow_size, const Vector2f scrollable_content_size, const Box& box,
	const float max_height) const
{
	if (!IsScrollContainer())
		return true;

	const Vector2f padding_bottom_right = {box.GetEdge(BoxArea::Padding, BoxEdge::Right), box.GetEdge(BoxArea::Padding, BoxEdge::Bottom)};
	const float padding_width = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Padding);

	const Vector2f overflow_content_size = GetScrollableOverflowContentSize(visible_overflow_size, scrollable_content_size, padding_bottom_right);

	Vector2f available_space = box.GetSize();
	if (available_space.y < 0.f)
		available_space.y = max_height;
	if (available_space.y < 0.f)
		available_space.y = HUGE_VALF;

	RMLUI_ASSERT(available_space.x >= 0.f && available_space.y >= 0.f);

	// Allow overflow onto the padding area.
	available_space += padding_bottom_right;

	ElementScroll* element_scroll = element->GetElementScroll();
	bool scrollbar_size_changed = false;

	// @performance If we have auto-height sizing and the horizontal scrollbar is enabled, then we can in principle
	// simply add the scrollbar size to the height instead of formatting the element all over again.
	if (overflow_x == Style::Overflow::Auto && overflow_content_size.x > available_space.x + 0.5f)
	{
		if (element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL) == 0.f)
		{
			element_scroll->EnableScrollbar(ElementScroll::HORIZONTAL, padding_width);
			const float new_size = element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL);
			scrollbar_size_changed = (new_size != 0.f);
			available_space.y -= new_size;
		}
	}

	// If we're auto-scrolling and our height is fixed, we have to check if this box has exceeded our client height.
	if (overflow_y == Style::Overflow::Auto && overflow_content_size.y > available_space.y + 0.5f)
	{
		if (element_scroll->GetScrollbarSize(ElementScroll::VERTICAL) == 0.f)
		{
			element_scroll->EnableScrollbar(ElementScroll::VERTICAL, padding_width);
			const float new_size = element_scroll->GetScrollbarSize(ElementScroll::VERTICAL);
			scrollbar_size_changed |= (new_size != 0.f);
		}
		else if (element_scroll->IsScrollbarGutterOnly(ElementScroll::VERTICAL))
		{
			// Its space is already there ('scrollbar-gutter: stable'): show it.
			element_scroll->EnableScrollbar(ElementScroll::VERTICAL, padding_width);
		}
	}

	return !scrollbar_size_changed;
}

bool ContainerBox::SubmitBox(const Vector2f visible_overflow_size, const Vector2f scrollable_content_size, const Box& box, const float max_height)
{
	Vector2f propagate_visible_overflow_size;

	// Set the computed box on the element.
	if (element)
	{
		// Calculate the dimensions of the box's scrollable overflow rectangle. This is the union of the tightest-
		// fitting box around all of the internal elements, and this element's padding box. We really only care about
		// overflow on the bottom-right sides, as these are the only ones allowed to be scrolled to in CSS.
		//
		// If we are a scroll container (use any other value than 'overflow: visible'), then any overflow outside our
		// padding box should be caught here. Otherwise, our overflow should be included in the overflow calculations of
		// our nearest scroll container ancestor.

		// If our content is larger than our padding box, we can add scrollbars if we're set to auto-scrollbars. If
		// we're set to always use scrollbars, then the scrollbars have already been enabled.
		if (!CatchOverflow(visible_overflow_size, scrollable_content_size, box, max_height))
			return false;

		const Vector2f padding_top_left = {box.GetEdge(BoxArea::Padding, BoxEdge::Left), box.GetEdge(BoxArea::Padding, BoxEdge::Top)};
		const Vector2f padding_bottom_right = {box.GetEdge(BoxArea::Padding, BoxEdge::Right), box.GetEdge(BoxArea::Padding, BoxEdge::Bottom)};
		const Vector2f padding_size = box.GetSize() + padding_top_left + padding_bottom_right;

		const bool is_scroll_container = IsScrollContainer();
		const Vector2f scrollbar_size = {
			is_scroll_container ? element->GetElementScroll()->GetScrollbarSize(ElementScroll::VERTICAL) : 0.f,
			is_scroll_container ? element->GetElementScroll()->GetScrollbarSize(ElementScroll::HORIZONTAL) : 0.f,
		};

		element->SetBox(box);

		// Scrollable overflow is the set of things extending our padding area, for which scrolling could be provided.
		const Vector2f scrollable_overflow_size = Math::Max(padding_size - scrollbar_size,
			padding_top_left + GetScrollableOverflowContentSize(visible_overflow_size, scrollable_content_size, padding_bottom_right));

		// Set the overflow size but defer clamping of the scroll offset, see `LayoutEngine::FormatElement`.
		element->SetScrollableOverflowRectangle(scrollable_overflow_size, false);

		const Vector2f border_size = padding_size + box.GetFrameSize(BoxArea::Border);

		// Set the visible overflow size so that ancestors can catch any overflow produced by us. That is, hiding it or
		// providing a scrolling mechanism. If this box is a scroll container we catch our own overflow here. Thus, in
		// this case, only our border box is visible from our ancestor's perspective.
		if (is_scroll_container)
		{
			propagate_visible_overflow_size = border_size;

			// Format any scrollbars in case they were enabled on this element.
			element->GetElementScroll()->FormatScrollbars();
		}
		else
		{
			const Vector2f border_top_left = {box.GetEdge(BoxArea::Border, BoxEdge::Left), box.GetEdge(BoxArea::Border, BoxEdge::Top)};
			propagate_visible_overflow_size = Math::Max(border_size, visible_overflow_size + border_top_left + padding_top_left);

			// An axis with 'overflow: clip' shows nothing beyond its overflow clip edge: the padding box extended by
			// 'overflow-clip-margin'.
			if (overflow_x == Style::Overflow::Clip || overflow_y == Style::Overflow::Clip)
			{
				const Vector2f clip_edge = border_top_left + padding_size + Vector2f(element->GetComputedValues().overflow_clip_margin());
				if (overflow_x == Style::Overflow::Clip)
					propagate_visible_overflow_size.x = Math::Max(border_size.x, Math::Min(propagate_visible_overflow_size.x, clip_edge.x));
				if (overflow_y == Style::Overflow::Clip)
					propagate_visible_overflow_size.y = Math::Max(border_size.y, Math::Min(propagate_visible_overflow_size.y, clip_edge.y));
			}
		}
	}

	SetVisibleOverflowSize(propagate_visible_overflow_size);

	return true;
}

String RootBox::DebugDumpTree(int depth) const
{
	return String(depth * 2, ' ') + "RootBox";
}

FlexContainer::FlexContainer(Element* element, ContainerBox* parent_container) : ContainerBox(Type::FlexContainer, element, parent_container)
{
	RMLUI_ASSERT(element);
}

bool FlexContainer::Close(const Vector2f visible_overflow_size, const Vector2f scrollable_content_size, const Box& box, float element_baseline)
{
	if (!SubmitBox(visible_overflow_size, scrollable_content_size, box, -1.f))
		return false;

	ClosePositionedElements();
	if (!CatchAbsoluteOverflow())
		return false;

	SubmitElementLayout();
	SetElementBaseline(element_baseline);
	return true;
}

bool FlexContainer::GetBaselineOfFirstLine(float& out_baseline) const
{
	if (has_first_baseline)
		out_baseline = first_baseline;
	return has_first_baseline;
}

float FlexContainer::GetShrinkToFitWidth() const
{
	// For the trivial case of a fixed width, we simply return that.
	if (element->GetComputedValues().width().type == Style::Width::Type::Length && !LayoutDetails::IsMeasuringContentOf(element))
		return box.GetSize().x;

	// Infer shrink-to-fit width from the intrinsic width of the element.
	return FlexFormattingContext::GetMaxContentSize(element, definite_content_height).x;
}

float FlexContainer::GetMinContentWidth() const
{
	// For the trivial case of a fixed width, we simply return that.
	if (element->GetComputedValues().width().type == Style::Width::Type::Length && !LayoutDetails::IsMeasuringContentOf(element))
		return box.GetSize().x;

	return FlexFormattingContext::GetMinContentSize(element, definite_content_height).x;
}

String FlexContainer::DebugDumpTree(int depth) const
{
	return String(depth * 2, ' ') + "FlexContainer" + " | " + LayoutDetails::GetDebugElementName(element);
}

GridContainer::GridContainer(Element* element, ContainerBox* parent_container) : ContainerBox(Type::GridContainer, element, parent_container)
{
	RMLUI_ASSERT(element);
	containing_block = LayoutDetails::GetContainingBlock(parent_container, element).size;
}

GridContainer::~GridContainer() {}

bool GridContainer::Close(const Vector2f visible_overflow_size, const Vector2f scrollable_content_size, const Box& box, float element_baseline)
{
	if (!SubmitBox(visible_overflow_size, scrollable_content_size, box, -1.f))
		return false;

	ClosePositionedElements();
	absolute_area_resolver.reset();
	if (!CatchAbsoluteOverflow())
		return false;

	SubmitElementLayout();
	SetElementBaseline(element_baseline);
	return true;
}

float GridContainer::GetShrinkToFitWidth() const
{
	// For the trivial case of a fixed width, we simply return that (unless the content is measured, LayoutDetails::GetContentWidth).
	if (element->GetComputedValues().width().type == Style::Width::Type::Length && !LayoutDetails::IsMeasuringContentOf(element))
		return box.GetSize().x;

	return GridFormattingContext::GetMaxContentWidth(element, containing_block);
}

float GridContainer::GetMinContentWidth() const
{
	if (element->GetComputedValues().width().type == Style::Width::Type::Length && !LayoutDetails::IsMeasuringContentOf(element))
		return box.GetSize().x;

	return GridFormattingContext::GetMinContentWidth(element, containing_block);
}

String GridContainer::DebugDumpTree(int depth) const
{
	return String(depth * 2, ' ') + "GridContainer" + " | " + LayoutDetails::GetDebugElementName(element);
}

void GridContainer::SetAbsoluteAreaResolver(UniquePtr<GridLayout::AbsoluteAreaResolver> resolver)
{
	absolute_area_resolver = std::move(resolver);
}

std::optional<Rectanglef> GridContainer::GetAbsoluteContainingBlock(Element* absolute_element) const
{
	// CSS Grid 2, 10.1: the grid area from the box's grid-placement properties ('auto' lines at our padding edges).
	if (!absolute_area_resolver)
		return std::nullopt;
	return absolute_area_resolver->Resolve(absolute_element);
}

TableWrapper::TableWrapper(Element* element, ContainerBox* parent_container) : ContainerBox(Type::TableWrapper, element, parent_container)
{
	RMLUI_ASSERT(element);
}

void TableWrapper::Close(const Vector2f visible_overflow_size, const Box& box, float element_baseline)
{
	bool result = SubmitBox(visible_overflow_size, Vector2f(0.f), box, -1.f);

	// Since the table wrapper cannot generate scrollbars, this should always pass.
	RMLUI_ASSERT(result);
	(void)result;

	ClosePositionedElements();

	SubmitElementLayout();
	SetElementBaseline(element_baseline);
}

float TableWrapper::GetShrinkToFitWidth() const
{
	// We don't currently support shrink-to-fit layout of tables. However, for the trivial case of a fixed width, we
	// simply return that.
	if (element->GetComputedValues().width().type == Style::Width::Type::Length)
		return box.GetSize().x;

	return 0.0f;
}

String TableWrapper::DebugDumpTree(int depth) const
{
	return String(depth * 2, ' ') + "TableWrapper" + " | " + LayoutDetails::GetDebugElementName(element);
}

} // namespace Rml
