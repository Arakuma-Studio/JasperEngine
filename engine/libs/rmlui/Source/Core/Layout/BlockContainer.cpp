#include "BlockContainer.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/ElementScroll.h"
#include "../../../Include/RmlUi/Core/Profiling.h"
#include "../LineClampEllipsis.h"
#include "FloatedBoxSpace.h"
#include "InlineContainer.h"
#include "LayoutDetails.h"
#include "LineBox.h"
#include "LineClamp.h"
#include <float.h>

namespace Rml {

BlockContainer::BlockContainer(ContainerBox* _parent_container, FloatedBoxSpace* _space, Element* _element, const Box& _box, float _min_height,
	float _max_height) :
	ContainerBox(Type::BlockContainer, _element, _parent_container), box(_box), min_height(_min_height), max_height(_max_height), space(_space)
{
	RMLUI_ASSERT(element);
	RMLUI_ASSERT(box.GetSize().x >= 0.f);

	if (!space)
	{
		// We are the root of the formatting context, establish a new space for floated boxes.
		root_space = MakeUnique<FloatedBoxSpace>();
		space = root_space.get();

		// A line-clamp container clamps its block formatting context (fork, P6-L1).
		if (const int lines = element->GetComputedValues().line_clamp())
		{
			own_line_clamp.lines_until_clamp = lines;
			owns_line_clamp = true;
			line_clamp = &own_line_clamp;
		}
	}
	// Our lines place a new ellipsis, if any (fork, P6-L1).
	if (LineClampAccess::HasBlockEllipsis(element))
		LineClampAccess::SetBlockEllipsis(element, nullptr);

	// Our top margin collapses with our first in-flow child's if nothing separates them (CSS 2.1 §8.3.1): we are not the
	// root of a block formatting context, and have no top border or padding.
	top_margin_open = (!root_space && box.GetEdge(BoxArea::Border, BoxEdge::Top) == 0.f && box.GetEdge(BoxArea::Padding, BoxEdge::Top) == 0.f);
}

BlockContainer::~BlockContainer() {}

bool BlockContainer::Close(BlockContainer* parent_block_container)
{
	// If the last child of this block box is an inline box, then we haven't closed it; close it now!
	if (!CloseOpenInlineContainer())
		return false;

	// A line-clamp container whose count ends at exactly 0 has nothing after its clamp line: it is laid out again without
	// clamping, so it shows no ellipsis (fork, P6-L1; Chromium's ShouldRelayoutWithNoForcedTruncate).
	// Clamped inside us: our automatic height ends at our clamp position, without the margins after it (fork, P6-L1).
	bool clamped = false;
	if (line_clamp)
	{
		if (owns_line_clamp && clamp_cursor >= 0.f && line_clamp->lines_until_clamp == 0)
		{
			ignore_line_clamp = true;
			return false;
		}
		clamped = (clamp_cursor >= 0.f);
	}

	// Our bottom margin collapses with our last in-flow child's if nothing separates them (CSS 2.1 §8.3.1): we are not the
	// root of a block formatting context, our height is 'auto' and we have no bottom border or padding.
	const bool auto_height = (box.GetSize().y < 0);
	const bool no_bottom_edges = (box.GetEdge(BoxArea::Border, BoxEdge::Bottom) == 0.f && box.GetEdge(BoxArea::Padding, BoxEdge::Bottom) == 0.f);
	bool bottom_margin_collapses = (!root_space && auto_height && no_bottom_edges && !clamped);

	// Set this box's height, if necessary.
	if (auto_height)
	{
		float content_height = box_cursor;
		if ((!bottom_margin_collapses || trailing_after_clearance) && !top_margin_open)
			content_height += trailing_margin_set.Resolve();
		// (Chromium's legacy line clamp: the intrinsic block size at the clamp point, no margins or floats after it)
		if (clamped)
			content_height = clamp_cursor;

		if (!parent_block_container)
		{
			if (!clamped)
				content_height = Math::Max(content_height, space->GetDimensions(FloatedBoxEdge::Margin).y - (position.y + box.GetPosition().y));
			content_height += element->GetElementScroll()->GetScrollbarSize(ElementScroll::HORIZONTAL);
		}

		const float clamped_height = Math::Clamp(content_height, min_height, max_height);
		// When the min-height determines our height, our last child's bottom margin no longer adjoins our bottom.
		if (min_height > 0.f && clamped_height > content_height)
			bottom_margin_collapses = false;
		box.SetContent({box.GetSize().x, clamped_height});
	}

	else if (content_max_height >= 0.f)
	{
		// A height from an aspect ratio is the automatic minimum height: taller content extends us.
		float content_height = (clamped ? clamp_cursor : box_cursor + (top_margin_open ? 0.f : trailing_margin_set.Resolve()));
		if (!parent_block_container && !clamped)
			content_height = Math::Max(content_height, space->GetDimensions(FloatedBoxEdge::Margin).y - (position.y + box.GetPosition().y));
		if (content_height > box.GetSize().y)
			box.SetContent({box.GetSize().x, Math::Max(Math::Min(content_height, content_max_height), box.GetSize().y)});
	}

	// 'align-content' (CSS Box Alignment 3 §5.1, block containers; it makes us the root of a block formatting context): the
	// content moves as one in the free space. A button centers it (fork, P4-G2).
	if (root_space && (element->GetComputedValues().align_content() != Style::AlignContent::Normal || element->HasButtonLayout()))
		AlignContent();

	// The margins below our border bottom, for our parent. An empty box (no in-flow content, no height, no vertical padding or
	// borders) lets its top and bottom margins collapse through it.
	const float margin_bottom = box.GetEdge(BoxArea::Margin, BoxEdge::Bottom);
	collapsed_through = (top_margin_open && no_bottom_edges && box.GetSize().y == 0.f && parent_block_container);
	bottom_margin_set = CollapsibleMarginSet();
	if (collapsed_through && clearance)
	{
		// Clearance fixes our border top: of our collapsed margins, what exceeds our top margin is below it (CSS 2.1 §9.5.2).
		CollapsibleMarginSet own_margin_set = top_margin_set;
		own_margin_set.Add(trailing_margin_set);
		own_margin_set.Add(margin_bottom);
		bottom_margin_set.Add(own_margin_set.Resolve() - box.GetEdge(BoxArea::Margin, BoxEdge::Top));
	}
	else
	{
		if (collapsed_through)
		{
			bottom_margin_set = top_margin_set;
			bottom_margin_set.Add(trailing_margin_set);
		}
		else if (bottom_margin_collapses && !top_margin_open && !trailing_after_clearance)
			bottom_margin_set = trailing_margin_set;
		bottom_margin_set.Add(margin_bottom);
	}

	// Find the size of our content.
	const Vector2f space_box = space->GetDimensions(FloatedBoxEdge::Overflow) - (position + box.GetPosition());
	Vector2f content_box = Math::Max(inner_content_size, space_box);
	content_box.y = Math::Max(content_box.y, box_cursor);

	if (!SubmitBox(content_box, inner_content_margin_size, box, max_height))
		return false;

	// If we are the root of our block formatting context, this will be null. Otherwise increment our parent's cursor to account for this box.
	if (parent_block_container)
	{
		RMLUI_ASSERTMSG(GetParent() == parent_block_container, "Mismatched parent box.");

		// If this close fails, it means this block box has caused our parent box to generate an automatic vertical scrollbar.
		if (!parent_block_container->EncloseChildBox(this, position, box.GetSize(BoxArea::Border),
				{box.GetEdge(BoxArea::Margin, BoxEdge::Right), box.GetEdge(BoxArea::Margin, BoxEdge::Bottom)}, bottom_margin_set, collapsed_through,
				joined_parent_top_margin, clearance))
			return false;
		if (line_clamp)
			parent_block_container->UpdateLineClampAfterChild(lines_at_open, true);
	}

	// Now that we have been sized, we can proceed with formatting and placing positioned elements that this container
	// acts as a containing block for.
	ClosePositionedElements();
	// (A scroll container is the root of its formatting context: formatted again when this fails, as when SubmitBox does.)
	if (!CatchAbsoluteOverflow())
		return false;

	// Find the element baseline which is the distance from the margin bottom of the element to its baseline.
	float element_baseline = 0;

	// For inline-blocks with visible overflow, this is the baseline of the last line of the element (see CSS2 10.8.1).
	if (element->GetDisplay() == Style::Display::InlineBlock && !IsScrollContainer())
	{
		float baseline = 0;
		bool found_baseline = GetBaselineOfLastLine(baseline);

		// The retrieved baseline is the vertical distance from the top of our root space (the coordinate system of our
		// local block formatting context), convert it to the element's local coordinates.
		if (found_baseline)
		{
			const float bottom_position =
				position.y + box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border) + box.GetEdge(BoxArea::Margin, BoxEdge::Bottom);
			element_baseline = bottom_position - baseline;
		}
		// A button without a line has its baseline at its content box's bottom (Chromium; fork, P4-G2).
		else if (element->HasButtonLayout())
		{
			element_baseline = box.GetEdge(BoxArea::Margin, BoxEdge::Bottom) + box.GetEdge(BoxArea::Border, BoxEdge::Bottom) +
				box.GetEdge(BoxArea::Padding, BoxEdge::Bottom);
		}
	}

	SetElementBaseline(element_baseline);

	EnsureEmptyInterruptedLineBox();

	SubmitElementLayout();

	return true;
}

bool BlockContainer::EncloseChildBox(LayoutBox* child, Vector2f child_position, Vector2f child_border_size, Vector2f child_margin_bottom_right,
	const CollapsibleMarginSet& child_bottom_margin_set, bool child_collapsed_through, bool child_joined_top_margin, bool child_clearance)
{
	child_position -= (box.GetPosition() + position);

	if (child_collapsed_through && child_clearance)
	{
		// An empty child moved below floats: the cursor moves to it. Its margins below its border top collapse with what
		// follows, but not with our bottom margin (CSS 2.1 §8.3.1).
		box_cursor = child_position.y;
		trailing_margin_set = child_bottom_margin_set;
		trailing_after_clearance = true;
		top_margin_open = false;
	}
	else if (child_collapsed_through)
	{
		// An empty child: its margins collapse with those around it, our cursor stays.
		if (child_joined_top_margin && top_margin_open)
			CollapseTopMargin(child_bottom_margin_set);
		else
			trailing_margin_set = child_bottom_margin_set;
	}
	else
	{
		box_cursor = child_position.y + child_border_size.y;
		trailing_margin_set = child_bottom_margin_set;
		trailing_after_clearance = false;
		top_margin_open = false;
	}

	// Extend the inner content size. The vertical size can be larger than the box_cursor due to overflow.
	inner_content_size = Math::Max(inner_content_size, child_position + child->GetVisibleOverflowSize());

	// Separately extend the margin-box size of the child's margin box, since this does not propagate to parents unlike visible overflow.
	// Below the child: its bottom margin as collapsed with the margins of its last children (they end the scrollable
	// overflow of a scroll container as in browsers, also through a wrapper).
	Vector2f margin_bottom_right = child_margin_bottom_right;
	if (!child_collapsed_through)
		margin_bottom_right.y = Math::Max(margin_bottom_right.y, child_bottom_margin_set.Resolve());
	inner_content_margin_size = Math::Max(inner_content_margin_size, child_position + child_border_size + margin_bottom_right);

	const Vector2f content_size =
		Math::Max(Vector2f{box.GetSize().x, box_cursor + (top_margin_open ? 0.f : trailing_margin_set.Resolve())}, inner_content_size);

	const bool result = CatchOverflow(content_size, inner_content_margin_size, box, max_height);

	return result;
}

BlockContainer* BlockContainer::OpenBlockBox(Element* child_element, const Box& child_box, float min_height, float max_height)
{
	if (!CloseOpenInlineContainer())
		return nullptr;

	auto child_container_ptr = MakeUnique<BlockContainer>(this, space, child_element, child_box, min_height, max_height);
	BlockContainer* child_container = child_container_ptr.get();
	// (it shares our formatting context, and our line-clamp context: fork, P6-L1)
	if (line_clamp)
	{
		NoteLineClampChild();
		child_container->line_clamp = line_clamp;
		child_container->lines_at_open = line_clamp->lines_until_clamp;
	}

	bool joins_top_margin = false;
	child_container->position = PlaceBlockLevelChild(child_element, child_box, child_element->GetComputedValues().clear(),
		child_container->top_margin_set, joins_top_margin, child_container->clearance, false);
	if (child_container->clearance)
	{
		// Clearance separates its top margin from the margins before it, and fixes its position.
		child_container->top_margin_set = CollapsibleMarginSet();
		child_container->top_margin_set.Add(child_box.GetEdge(BoxArea::Margin, BoxEdge::Top));
	}
	child_container->joined_parent_top_margin = joins_top_margin;
	child_container->top_margin_parent = (joins_top_margin ? this : nullptr);
	child_element->SetOffset(child_container->position - position, element);

	child_container->ResetScrollbars(child_box);

	// Store relatively positioned elements with their containing block so that their offset can be updated after
	// their containing block has been sized.
	if (child_element->GetPosition() == Style::Position::Relative || child_element->GetPosition() == Style::Position::Sticky)
		AddRelativeElement(child_element);

	child_boxes.push_back(std::move(child_container_ptr));

	return child_container;
}

LayoutBox* BlockContainer::AddBlockLevelBox(UniquePtr<LayoutBox> block_level_box_ptr, Element* child_element, const Box& child_box,
	const FloatFit* float_fit)
{
	RMLUI_ASSERT(child_box.GetSize().y >= 0.f); // Assumes child element already formatted and sized.

	if (!CloseOpenInlineContainer())
		return nullptr;

	// The box must not overlap floats: it was fitted among them (FitBesideFloats), or else it clears them.
	CollapsibleMarginSet child_top_margin_set;
	bool joins_top_margin = false, child_clearance = false;
	Vector2f child_position = PlaceBlockLevelChild(child_element, child_box, float_fit ? Style::Clear::None : Style::Clear::Both,
		child_top_margin_set, joins_top_margin, child_clearance, true);
	if (float_fit)
	{
		// Moved below floats it did not fit beside: as with clearance, its margins no longer collapse with ours.
		if (float_fit->top > child_position.y + 0.01f)
		{
			child_position.y = float_fit->top;
			child_clearance = true;
			joins_top_margin = false;
			top_margin_open = false;
		}
		if (float_fit->beside_floats)
			child_position.x = float_fit->left;
	}

	child_element->SetOffset(child_position - position, element);

	if (child_element->GetPosition() == Style::Position::Relative || child_element->GetPosition() == Style::Position::Sticky)
		AddRelativeElement(child_element);

	LayoutBox* block_level_box = block_level_box_ptr.get();
	if (block_level_box->GetType() != Type::Replaced)
		static_cast<ContainerBox*>(block_level_box)->SetFlowPositionTop(child_position.y);
	if (line_clamp)
		NoteLineClampChild();
	child_boxes.push_back(std::move(block_level_box_ptr));

	// A box establishing an independent formatting context does not collapse margins with its content, and is never
	// collapsed through.
	CollapsibleMarginSet child_bottom_margin_set;
	child_bottom_margin_set.Add(child_box.GetEdge(BoxArea::Margin, BoxEdge::Bottom));
	if (!EncloseChildBox(block_level_box, child_position, child_box.GetSize(BoxArea::Border),
			{child_box.GetEdge(BoxArea::Margin, BoxEdge::Right), child_box.GetEdge(BoxArea::Margin, BoxEdge::Bottom)}, child_bottom_margin_set, false,
			joins_top_margin, child_clearance))
		return nullptr;
	// (its lines are its own formatting context's: the count is unchanged)
	if (line_clamp)
		UpdateLineClampAfterChild(line_clamp->lines_until_clamp, true);

	return block_level_box;
}

bool BlockContainer::FitBesideFloats(Element* child_element, const Box& child_box, FloatFit& out_fit, float& out_content_width)
{
	out_content_width = child_box.GetSize().x;
	if (space->GetDimensions(FloatedBoxEdge::Margin) == Vector2f(0.f))
		return false;
	if (!CloseOpenInlineContainer())
		return false;

	const ComputedValues& computed = child_element->GetComputedValues();
	const float margin_left = child_box.GetEdge(BoxArea::Margin, BoxEdge::Left);
	const float margin_right = child_box.GetEdge(BoxArea::Margin, BoxEdge::Right);
	const float margin_top = child_box.GetEdge(BoxArea::Margin, BoxEdge::Top);
	const float edges = child_box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
	const Vector2f border_size = child_box.GetSize(BoxArea::Border);

	// An automatic width fills the line (LayoutDetails::BuildBoxWidth).
	const Style::Display display = computed.display();
	const bool auto_width = (computed.width().type == Style::Width::Auto && computed.width_keyword() == Style::SizingKeyword::None &&
		display != Style::Display::Table && !child_element->IsReplaced() && !LayoutDetails::IsJustifiedFitContent(child_element));
	float min_width = 0.f, max_width = FLT_MAX;
	LayoutDetails::GetMinMaxWidth(min_width, max_width, computed, child_box, box.GetSize().x);

	// Its border top after the margins before it, as PlaceBlockLevelChild places it.
	float top = position.y + box.GetPosition().y + box_cursor;
	if (top_margin_open && !adjoining_floats)
	{
		CollapsibleMarginSet margin_set = top_margin_set;
		margin_set.Add(margin_top);
		top += margin_set.Resolve() - top_margin_set.Resolve();
	}
	else
	{
		CollapsibleMarginSet margin_set = trailing_margin_set;
		margin_set.Add(margin_top);
		top += margin_set.Resolve();
	}

	const float scrollbar_width = element->GetElementScroll()->GetScrollbarSize(ElementScroll::VERTICAL);
	const float line_left = position.x + box.GetPosition().x;
	const float line_width = box.GetSize().x - scrollbar_width;
	const float height = Math::Max(border_size.y, 0.01f);

	// Take the first place, there or lower, where its border box fits between the floats.
	float required_width = (auto_width ? Math::Min(min_width + edges, border_size.x) : border_size.x);
	for (int i = 0; i < 16; i++)
	{
		float slot_width = 0.f;
		const Vector2f slot = space->NextBoxPosition(this, slot_width, top, Vector2f(required_width, height), false);
		out_fit.top = slot.y;

		// A line that no float narrows: the box is placed as without floats (its negative margins may extend it outside).
		if (slot.x <= line_left && slot_width >= line_width)
		{
			out_fit.beside_floats = false;
			if (auto_width)
				out_content_width = Math::Clamp(Math::Max(line_width - margin_left - margin_right - edges, 0.f), min_width, max_width);
			return true;
		}

		// Floats narrow the line. They absorb the margins of the box: its border box starts after the float or its margin,
		// whichever is further in, and an automatic width fills what is left.
		const float left = Math::Max(slot.x, line_left + Math::Max(margin_left, 0.f));
		const float right = Math::Min(slot.x + slot_width, line_left + line_width - Math::Max(margin_right, 0.f));
		float border_width = border_size.x;
		if (auto_width)
		{
			const float fill_width = Math::Max(Math::Max(right - left, 0.f) + margin_left + margin_right, 0.f) - margin_left - margin_right;
			border_width = Math::Clamp(Math::Max(fill_width - edges, 0.f), min_width, max_width) + edges;
		}

		if (border_width <= slot_width + 0.01f)
		{
			out_fit.beside_floats = true;
			out_fit.left = left;
			out_content_width = border_width - edges;
			return true;
		}

		// Too wide for this line: look for the next one that is wide enough for it.
		required_width = Math::Max(border_width, required_width + 0.01f);
	}

	out_fit.beside_floats = false;
	out_fit.top = space->DetermineClearPosition(top, Style::Clear::Both);
	out_content_width = child_box.GetSize().x;
	return true;
}

InlineBoxHandle BlockContainer::AddInlineElement(Element* element, const Box& child_box)
{
	RMLUI_ZoneScoped;

	// Inline-level elements need to be added to an inline container, open one if needed.
	InlineContainer* inline_container = EnsureOpenInlineContainer();

	InlineBox* inline_box = inline_container->AddInlineElement(element, child_box);

	if (element->GetPosition() == Style::Position::Relative || element->GetPosition() == Style::Position::Sticky)
		AddRelativeElement(element);

	return {inline_box};
}

void BlockContainer::CloseInlineElement(InlineBoxHandle handle)
{
	// If the inline-level element did not generate an inline box, then there is no need to close anything.
	if (!handle.inline_box)
		return;

	// Usually the inline container the box was placed in is still the open box, and we can just close the inline
	// element in it. However, it is possible that an intermediary block-level element was placed, thereby splitting the
	// inline element into multiple inline containers around the block-level box. If we don't have an open inline
	// container at all, open a new one, even if the sole purpose of the new line is to close this inline element.
	EnsureOpenInlineContainer()->CloseInlineElement(handle.inline_box);
}

void BlockContainer::AddBreak()
{
	const float line_height = element->GetLineHeight();

	// With line clamping the <br> makes a line of an inline container, which counts it (fork, P6-L1).
	if (line_clamp)
	{
		EnsureOpenInlineContainer()->AddBreak(line_height);
		return;
	}

	// Check for an inline box as our last child; if so, we can simply end its line and bail.
	if (InlineContainer* inline_container = GetOpenInlineContainer())
	{
		inline_container->AddBreak(line_height);
		return;
	}

	// No inline box as our last child; no problem, just increment the cursor by the line height of this element (an empty
	// line, after the margins pending before it).
	if (!top_margin_open)
		box_cursor += trailing_margin_set.Resolve();
	trailing_margin_set = CollapsibleMarginSet();
	top_margin_open = false;
	box_cursor += line_height;
}

void BlockContainer::AddFloatElement(Element* element, Vector2f visible_overflow_size)
{
	if (InlineContainer* inline_container = GetOpenInlineContainer())
	{
		// Try to add the float to our inline container, placing it next to any open line if possible. Otherwise, queue it for later.
		bool float_placed = false;
		float line_position_top = 0.f;
		Vector2f line_size;
		if (queued_float_elements.empty() && inline_container->GetOpenLineBoxDimensions(line_position_top, line_size))
		{
			const Vector2f margin_size = element->GetBox().GetSize(BoxArea::Margin);
			const Style::Float float_property = element->GetComputedValues().float_();
			const Style::Clear clear_property = element->GetComputedValues().clear();

			float available_width = 0.f;
			const Vector2f float_position =
				space->NextFloatPosition(this, available_width, line_position_top, margin_size, float_property, clear_property);

			const float line_position_bottom = line_position_top + line_size.y;
			const float line_and_element_width = margin_size.x + line_size.x;

			// If the float can be positioned on the open line, and it can fit next to the line's contents, place it now.
			if (float_position.y < line_position_bottom && line_and_element_width <= available_width)
			{
				PlaceFloat(element, line_position_top, visible_overflow_size);
				inline_container->UpdateOpenLineBoxPlacement();
				float_placed = true;
			}
		}

		if (!float_placed)
			queued_float_elements.push_back({element, visible_overflow_size});
	}
	else
	{
		// There is no inline container, so just place it! While our top margin is open, the float adjoins the margins that
		// collapse with it.
		if (top_margin_open)
		{
			for (BlockContainer* container = this; container; container = container->top_margin_parent)
				container->adjoining_floats = true;
		}
		const Vector2f box_position = NextBoxPosition();
		PlaceFloat(element, box_position.y, visible_overflow_size);
	}

	if (element->GetPosition() == Style::Position::Relative || element->GetPosition() == Style::Position::Sticky)
		AddRelativeElement(element);
}

Vector2f BlockContainer::GetOpenStaticPosition(Style::Display display) const
{
	// Estimate the next box as if it had static position (10.6.4). If the element is inline-level, position it on the
	// open line if we have one. Otherwise, block-level elements are positioned on a hypothetical next line.
	Vector2f static_position = NextBoxPosition();

	if (const InlineContainer* inline_container = GetOpenInlineContainer())
	{
		const bool inline_level_element = (display == Style::Display::Inline || display == Style::Display::InlineBlock);
		static_position += inline_container->GetStaticPositionEstimate(inline_level_element);
	}

	return static_position;
}

Vector2f BlockContainer::NextBoxPosition() const
{
	Vector2f box_position = position + box.GetPosition();
	box_position.y += box_cursor;
	if (!top_margin_open)
		box_position.y += trailing_margin_set.Resolve();
	return box_position;
}

Vector2f BlockContainer::PlaceBlockLevelChild(Element* child_element, const Box& child_box, Style::Clear clear_property,
	CollapsibleMarginSet& out_top_margin_set, bool& out_joins_top_margin, bool& out_clearance, bool new_formatting_context)
{
	const float child_top_margin = child_box.GetEdge(BoxArea::Margin, BoxEdge::Top);
	out_joins_top_margin = false;
	out_clearance = false;

	// Floats placed while our top margin was open are positioned at our content top. A box establishing a new formatting
	// context, or clearing floats, after them separates from them: the margins before it are resolved there, without it.
	bool forced_clearance = false;
	if (top_margin_open && adjoining_floats && (new_formatting_context || clear_property != Style::Clear::None))
	{
		top_margin_open = false;

		// Its margin would have moved those floats with us, so the box clears them whatever its margin (the clearance may
		// be negative, CSS 2.1 §9.5.2): its border top goes right below them.
		const float content_top = position.y + box.GetPosition().y + box_cursor;
		forced_clearance = (clear_property != Style::Clear::None && space->DetermineClearPosition(content_top, clear_property) > content_top);
	}

	if (top_margin_open)
	{
		// Our first in-flow content: its top margin collapses with ours, so it starts at our content top.
		CollapsibleMarginSet margin_set = top_margin_set;
		margin_set.Add(child_top_margin);
		CollapseTopMargin(margin_set);
		out_top_margin_set = margin_set;
		out_joins_top_margin = true;
	}
	else
	{
		out_top_margin_set = trailing_margin_set;
		out_top_margin_set.Add(child_top_margin);
	}

	Vector2f box_position = position + box.GetPosition();
	box_position.x += child_box.GetEdge(BoxArea::Margin, BoxEdge::Left);
	box_position.y += box_cursor + (out_joins_top_margin ? 0.f : out_top_margin_set.Resolve());

	// Aligned by its 'justify-self' in the space beside its margin box.
	const float scrollbar_width = (IsScrollContainer() ? element->GetElementScroll()->GetScrollbarSize(ElementScroll::VERTICAL) : 0.f);
	box_position.x += LayoutDetails::GetJustifySelfOffset(child_element, box.GetSize().x - scrollbar_width - child_box.GetSize(BoxArea::Margin).x);

	// Clearance: the box moves below the floats it clears, its margins no longer collapse with ours.
	if (forced_clearance)
		box_position.y = position.y + box.GetPosition().y + box_cursor;
	const float clear_position = space->DetermineClearPosition(box_position.y, clear_property);
	if (clear_position > box_position.y)
	{
		box_position.y = clear_position;
		out_joins_top_margin = false;
		out_clearance = true;
		top_margin_open = false;
	}

	return box_position;
}

void BlockContainer::CollapseTopMargin(const CollapsibleMarginSet& margin_set)
{
	// Clearance fixes the position of the box at the top of the chain: the margins collapse above it without moving it.
	BlockContainer* chain_top = this;
	while (chain_top->top_margin_parent)
		chain_top = chain_top->top_margin_parent;
	const float delta = (chain_top->clearance ? 0.f : margin_set.Resolve() - top_margin_set.Resolve());
	for (BlockContainer* container = this; container; container = container->top_margin_parent)
	{
		container->top_margin_set = margin_set;
		container->position.y += delta;
		chain_top = container;
	}

	// The boxes below the top of the chain keep their offsets relative to it.
	if (delta != 0.f && chain_top->GetParent())
	{
		BlockContainer* parent = static_cast<BlockContainer*>(chain_top->GetParent());
		chain_top->element->SetOffset(chain_top->position - parent->position, parent->element);
	}
}

void BlockContainer::PlaceQueuedFloats(float vertical_position)
{
	if (!queued_float_elements.empty())
	{
		for (QueuedFloat entry : queued_float_elements)
			PlaceFloat(entry.element, vertical_position, entry.visible_overflow_size);

		queued_float_elements.clear();
	}
}

float BlockContainer::GetShrinkToFitWidth() const
{
	return GetIntrinsicContentWidth([](const LayoutBox& child) { return child.GetShrinkToFitWidth(); },
		[this](float content_width) {
			// Since we are the root of the block formatting context, add the width contributions of the floated boxes in
		    // our context. The basic algorithm used can produce overestimates, since floats may not be located next to
		    // the rest of the content.
			const float edge_left = box.GetPosition().x;
			const float edge_right = edge_left + box.GetSize().x;
			return content_width + space->GetShrinkToFitWidth(edge_left, edge_right);
		});
}

float BlockContainer::GetMinContentWidth() const
{
	// Formatted under a min-content constraint each float sits on its own line: the widest float or child.
	return GetIntrinsicContentWidth([](const LayoutBox& child) { return child.GetMinContentWidth(); },
		[this](float content_width) { return Math::Max(content_width, space->GetMinContentWidth()); });
}

template <typename ChildWidth, typename FloatsWidth>
float BlockContainer::GetIntrinsicContentWidth(ChildWidth child_width, FloatsWidth floats_width) const
{
	auto& computed = element->GetComputedValues();

	float content_width = 0.0f;
	const bool definite_width = (computed.width().type == Style::Width::Length && !LayoutDetails::IsMeasuringContentOf(element));
	if (definite_width)
	{
		// We have a definite width, so use that size (the floats of our formatting context are inside it).
		content_width = box.GetSize().x;
	}
	else
	{
		// Nope, then use the largest outer width of our children. Percentage sizing would be relative to our containing
		// block width and is treated just like 'auto' in this context.
		for (const auto& block_box : child_boxes)
		{
			const float child_inner_width = child_width(*block_box);
			const float child_edges_width = LayoutDetails::GetIntrinsicEdgesWidth(*block_box);

			content_width = Math::Max(content_width, child_edges_width + child_inner_width);
		}
	}

	if (root_space && !definite_width)
		content_width = floats_width(content_width);

	// The size of its content alone while it is measured (LayoutDetails::GetContentWidth).
	if (LayoutDetails::IsMeasuringContentOf(element))
		return content_width;

	float min_width, max_width;
	LayoutDetails::GetMinMaxWidth(min_width, max_width, computed, box, 0.f);
	const auto percentage_dependent = [](auto value) {
		using Type = decltype(value.type);
		return value.type == Type::Percentage
#ifdef RMLUI_MATH_EXPRESSIONS
			|| value.type == Type::Calculation
#endif
			;
	};
	// Percentage-dependent min/max widths are indefinite while measuring intrinsic width. Treat them
	// as their unconstrained defaults here, and resolve them later against the real containing block.
	if (percentage_dependent(computed.min_width()))
		min_width = 0.f;
	if (percentage_dependent(computed.max_width()))
		max_width = FLT_MAX;
	content_width = Math::Clamp(content_width, min_width, max_width);

	return content_width;
}

void BlockContainer::AlignContent()
{
	using Style::AlignContent;
	const ComputedValues& computed = element->GetComputedValues();
	float content_height = box_cursor + trailing_margin_set.Resolve();
	content_height = Math::Max(content_height, space->GetDimensions(FloatedBoxEdge::Margin).y - (position.y + box.GetPosition().y));
	const float free_space = box.GetSize().y - element->GetElementScroll()->GetScrollbarSize(ElementScroll::HORIZONTAL) - content_height;

	// A single alignment subject: the distributed values fall back to their 'safe' positional value. A button's content is
	// centered, safely, whatever its 'align-content' (Chromium's anonymous button content box has auto vertical margins).
	const bool button = element->HasButtonLayout();
	const AlignContent align_content = (button ? AlignContent::Center : computed.align_content());
	float offset = 0.f;
	switch (align_content)
	{
	case AlignContent::Center:
	case AlignContent::SpaceAround:
	case AlignContent::SpaceEvenly: offset = 0.5f * free_space; break;
	case AlignContent::End:
	case AlignContent::FlexEnd: offset = free_space; break;
	default: break;
	}
	const bool distributed = (align_content == AlignContent::SpaceAround || align_content == AlignContent::SpaceEvenly);
	if (free_space < 0.f && (button || distributed || computed.align_content_overflow() == Style::OverflowAlignment::Safe))
		offset = 0.f;
	if (offset == 0.f)
		return;

	// The baselines of our lines move with them.
	content_alignment_offset = offset;
	LayoutDetails::ShiftContentOffsets(element, offset);
	for (ContainerBox* container = this; container; container = container->GetParent())
		container->ShiftStaticPositions(element, Vector2f(0.f, offset));
}

const Box* BlockContainer::GetIfBox() const
{
	return &box;
}

Element* BlockContainer::GetElement() const
{
	return element;
}

const FloatedBoxSpace* BlockContainer::GetBlockBoxSpace() const
{
	return space;
}

Vector2f BlockContainer::GetPosition() const
{
	return position;
}

Box& BlockContainer::GetBox()
{
	return box;
}

const Box& BlockContainer::GetBox() const
{
	return box;
}

void BlockContainer::ResetContents()
{
	RMLUI_ZoneScopedC(0xDD3322);

	if (root_space)
		root_space->Reset();

	child_boxes.clear();
	queued_float_elements.clear();

	box_cursor = 0;
	content_alignment_offset = 0;
	trailing_after_clearance = false;
	// Line clamping (fork, P6-L1): counted again, or not at all once the container is laid out without clamping.
	if (owns_line_clamp)
	{
		if (ignore_line_clamp)
		{
			owns_line_clamp = false;
			line_clamp = nullptr;
		}
		else
			own_line_clamp.lines_until_clamp = element->GetComputedValues().line_clamp();
	}
	clamp_cursor = -1.f;
	first_past_clamp_child = -1;
	if (LineClampAccess::HasBlockEllipsis(element))
		LineClampAccess::SetBlockEllipsis(element, nullptr);
	trailing_margin_set = CollapsibleMarginSet();
	adjoining_floats = false;
	top_margin_open = (!root_space && box.GetEdge(BoxArea::Border, BoxEdge::Top) == 0.f && box.GetEdge(BoxArea::Padding, BoxEdge::Top) == 0.f);
	interrupted_line_box.reset();

	inner_content_size = {};
	inner_content_margin_size = {};
}

String BlockContainer::DebugDumpTree(int depth) const
{
	String value = String(depth * 2, ' ') + "BlockContainer" + " | " + LayoutDetails::GetDebugElementName(element) + '\n';

	for (auto&& block_box : child_boxes)
		value += block_box->DumpLayoutTree(depth + 1);

	return value;
}

InlineContainer* BlockContainer::GetOpenInlineContainer()
{
	return const_cast<InlineContainer*>(static_cast<const BlockContainer&>(*this).GetOpenInlineContainer());
}

const InlineContainer* BlockContainer::GetOpenInlineContainer() const
{
	if (!child_boxes.empty() && child_boxes.back()->GetType() == Type::InlineContainer)
		return rmlui_static_cast<InlineContainer*>(child_boxes.back().get());
	return nullptr;
}

InlineContainer* BlockContainer::EnsureOpenInlineContainer()
{
	// First check to see if we already have an open inline container.
	InlineContainer* inline_container = GetOpenInlineContainer();

	// Otherwise, we open a new one.
	if (!inline_container)
	{
		if (line_clamp)
			NoteLineClampChild();
		const float scrollbar_width = (IsScrollContainer() ? element->GetElementScroll()->GetScrollbarSize(ElementScroll::VERTICAL) : 0.f);
		const float available_width = box.GetSize().x - scrollbar_width;

		auto inline_container_ptr = MakeUnique<InlineContainer>(this, available_width);
		inline_container = inline_container_ptr.get();
		child_boxes.push_back(std::move(inline_container_ptr));

		if (interrupted_line_box)
		{
			inline_container->AddChainedBox(std::move(interrupted_line_box));
			interrupted_line_box.reset();
		}
	}

	return inline_container;
}

bool BlockContainer::CloseOpenInlineContainer()
{
	if (InlineContainer* inline_container = GetOpenInlineContainer())
	{
		EnsureEmptyInterruptedLineBox();

		Vector2f child_position;
		float child_height = 0.f;
		inline_container->Close(&interrupted_line_box, child_position, child_height);

		// Increment our cursor. If this close fails, it means this block container generated an automatic scrollbar. Lines
		// without height are ignored for margin collapsing (CSS 2.1 §8.3.1).
		const bool empty = (child_height <= 0.f);
		const CollapsibleMarginSet margins_after = (empty ? trailing_margin_set : CollapsibleMarginSet());
		if (!EncloseChildBox(inline_container, child_position, {0, child_height}, {0.f, 0.f}, margins_after, empty, false, false))
			return false;

		// Our clamp position: after its line at the clamp point (fork, P6-L1).
		if (line_clamp && clamp_cursor < 0.f && inline_container->GetLineClampHeight() >= 0.f)
			clamp_cursor = child_position.y - (position.y + box.GetPosition().y) + inline_container->GetLineClampHeight();
	}

	return true;
}

void BlockContainer::NoteLineClampChild()
{
	if (line_clamp->lines_until_clamp <= 0 && first_past_clamp_child < 0)
		first_past_clamp_child = (int)child_boxes.size();
}

void BlockContainer::UpdateLineClampAfterChild(int lines_before, bool block_level_child)
{
	// Chromium's BlockLineClampData::UpdateAfterLayout: a block-level box without lines right at the clamp point is
	// content after it (no relayout without clamping); past the clamp point we record our clamp position.
	if (block_level_child && lines_before == 0 && line_clamp->lines_until_clamp == 0)
		line_clamp->lines_until_clamp = -1;
	if (line_clamp->lines_until_clamp <= 0 && clamp_cursor < 0.f)
		clamp_cursor = box_cursor;
}

void BlockContainer::EnsureEmptyInterruptedLineBox()
{
	if (interrupted_line_box)
	{
		RMLUI_ERROR; // Internal error: Interrupted line box leaked.
		interrupted_line_box.reset();
	}
}

void BlockContainer::PlaceFloat(Element* element, float vertical_position, Vector2f visible_overflow_size)
{
	const Box& element_box = element->GetBox();

	const Vector2f border_size = element_box.GetSize(BoxArea::Border);
	visible_overflow_size = Math::Max(border_size, visible_overflow_size);

	const Vector2f margin_top_left = {element_box.GetEdge(BoxArea::Margin, BoxEdge::Left), element_box.GetEdge(BoxArea::Margin, BoxEdge::Top)};
	const Vector2f margin_bottom_right = {element_box.GetEdge(BoxArea::Margin, BoxEdge::Right),
		element_box.GetEdge(BoxArea::Margin, BoxEdge::Bottom)};
	const Vector2f margin_size = border_size + margin_top_left + margin_bottom_right;

	Style::Float float_property = element->GetComputedValues().float_();
	Style::Clear clear_property = element->GetComputedValues().clear();

	float unused_box_width = 0.f;
	const Vector2f margin_position = space->NextFloatPosition(this, unused_box_width, vertical_position, margin_size, float_property, clear_property);
	const Vector2f border_position = margin_position + margin_top_left;

	space->PlaceFloat(float_property, margin_position, margin_size, border_position, visible_overflow_size);

	// Shift the offset into this container's space, which acts as the float element's containing block.
	element->SetOffset(border_position - position, GetElement());
}

bool BlockContainer::GetChildBaseline(const LayoutBox& child_box, bool last, float& out_baseline) const
{
	if (!(last ? child_box.GetBaselineOfLastLine(out_baseline) : child_box.GetBaselineOfFirstLine(out_baseline)))
		return false;

	// Lines and block boxes of our formatting context report it in our coordinates; a box with its own formatting context
	// from the top of its border box.
	const Type type = child_box.GetType();
	const bool same_formatting_context =
		(type == Type::InlineContainer || (type == Type::BlockContainer && !static_cast<const BlockContainer&>(child_box).root_space));
	if (!same_formatting_context && type != Type::Replaced)
		out_baseline += static_cast<const ContainerBox&>(child_box).GetFlowPositionTop();
	return true;
}

bool BlockContainer::GetBaselineOfFirstLine(float& out_baseline) const
{
	// The baseline of our first child that itself has a baseline (not those laid out past the line-clamp point: Chromium,
	// fork P6-L1).
	const auto end = (first_past_clamp_child >= 0 ? child_boxes.begin() + first_past_clamp_child : child_boxes.end());
	for (auto it = child_boxes.begin(); it != end; ++it)
	{
		const auto& child_box = *it;
		if (GetChildBaseline(*child_box, false, out_baseline))
		{
			out_baseline += content_alignment_offset;
			// The baseline of a scroll container stays within its border box (CSS Box Alignment 3 §9.1).
			if (IsScrollContainer())
				out_baseline = Math::Clamp(out_baseline, position.y, position.y + box.GetSize(BoxArea::Border).y);
			return true;
		}
	}
	return false;
}

bool BlockContainer::GetBaselineOfLastLine(float& out_baseline) const
{
	// Return the baseline of our last child that itself has a baseline.
	const int end = (first_past_clamp_child >= 0 ? first_past_clamp_child : (int)child_boxes.size());
	for (int i = end - 1; i >= 0; i--)
	{
		if (GetChildBaseline(*child_boxes[i], true, out_baseline))
		{
			out_baseline += content_alignment_offset;
			return true;
		}
	}

	return false;
}

} // namespace Rml
