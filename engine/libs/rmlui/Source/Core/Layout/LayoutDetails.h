#pragma once

#include "../../../Include/RmlUi/Core/StyleTypes.h"
#include "../../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Box;
class BlockContainer;
class ContainerBox;
class LayoutBox;

/**
    ComputedAxisSize is an abstraction of an element's computed size properties along a single axis, either horizontally or vertically,
    allowing eg. rows and columns alike to use the same algorithms. Here, 'a' means left or top, 'b' means right or bottom.
*/
struct ComputedAxisSize {
	Style::LengthPercentageAuto size;
	Style::LengthPercentageAuto min_size; // 'auto' resolves to 0 here; flex items get their automatic minimum where they are sized
	Style::LengthPercentage max_size;
	Style::Padding padding_a, padding_b;
	Style::Margin margin_a, margin_b;
	float border_a, border_b;
	Style::BoxSizing box_sizing;
};

struct ContainingBlock {
	ContainerBox* container;
	Vector2f size;
};

/// The boxes a container lays out, in layout order (see LayoutDetails::GetLayoutChildren).
using LayoutChildList = Vector<Element*>;

/// A LayoutChildList borrowed from a stack of lists kept for reuse, so that listing the layout children of every box
/// allocates nothing once warm. Lists are returned in reverse order of borrowing (scoped, as layout recursion is).
class ScopedLayoutChildList {
public:
	ScopedLayoutChildList();
	~ScopedLayoutChildList();
	ScopedLayoutChildList(const ScopedLayoutChildList&) = delete;
	ScopedLayoutChildList& operator=(const ScopedLayoutChildList&) = delete;

	LayoutChildList& operator*() { return *list; }
	LayoutChildList* operator->() { return list; }

private:
	LayoutChildList* list;
};

/// The space available to a box along one axis while it is sized by its content (CSS Sizing 3): a definite size, or a
/// min-content or max-content constraint.
struct AvailableSpace {
	enum Type : uint8_t { Definite, MinContent, MaxContent };
	Type type = MaxContent;
	float value = 0.f;

	static AvailableSpace MakeDefinite(float value) { return AvailableSpace{Definite, value}; }
	static AvailableSpace MakeMinContent() { return AvailableSpace{MinContent, 0.f}; }
	static AvailableSpace MakeMaxContent() { return AvailableSpace{MaxContent, 0.f}; }
	bool IsDefinite() const { return type == Definite; }
};

enum class BuildBoxMode {
	Block,          // Sets edges and size if available, auto width can result in shrink-to-fit width, auto margins are used for alignment.
	Inline,         // Sets edges, ignores width, height, and auto margins.
	UnalignedBlock, // Like block, but auto width returns -1, and auto margins are resolved to zero.
};

/**
    Layout functions for sizing elements.

    Some procedures based on the CSS 2.1 specification, 'Section 10. Visual formatting model details'.
 */
class LayoutDetails {
public:
	/// A text element of white space only that CSS collapses away (white-space other than pre, pre-wrap, break-spaces).
	static bool IsCollapsibleWhiteSpaceText(Element* element);

	/// Returns the boxes to lay out inside an element, in order: its ::marker, its ::before, its DOM children, its ::after
	/// (pseudo-elements once they are generated, Element::GetPseudoElement). A 'display: contents' child is replaced by its
	/// own layout children, recursively. With 'apply_order' (flex and grid containers) the list is stable-sorted by the
	/// 'order' property, so that the boxes are placed, and painted, in order-modified document order.
	/// @param[in] parent The element whose boxes to list.
	/// @param[out] children Cleared, then filled.
	/// @param[in] apply_order True to sort by 'order'.
	static void GetLayoutChildren(Element* parent, LayoutChildList& children, bool apply_order);

	/// Generates the box dimensions for an element.
	/// @param[out] box The box to be built.
	/// @param[in] containing_block The dimensions of the content area of the block containing the element.
	/// @param[in] element The element to build the box for.
	/// @param[in] box_context The formatting context in which the box is generated.
	static void BuildBox(Box& box, Vector2f containing_block, Element* element, BuildBoxMode box_context = BuildBoxMode::Block);

	// Retrieves the minimum and maximum width from an element's computed values.
	static void GetMinMaxWidth(float& min_width, float& max_width, const ComputedValues& computed, const Box& box, float containing_block_width);

	// Retrieves the minimum and maximum height from an element's computed values.
	static void GetMinMaxHeight(float& min_height, float& max_height, const ComputedValues& computed, const Box& box, float containing_block_height);

	// Retrieves the minimum and maximum height, set to the box's content height if it is definite (>= 0), otherwise retrieves the minimum and maximum
	// heights from an element's computed values.
	static void GetDefiniteMinMaxHeight(float& min_height, float& max_height, const ComputedValues& computed, const Box& box,
		float containing_block_height);

	/// Returns the containing block for a box.
	/// @param[in] parent_container The parent container of the current box.
	/// @param[in] position The position property of the current box.
	/// @return The containing block box and size, possibly indefinite (represented by negative size) along one or both axes.
	static ContainingBlock GetContainingBlock(ContainerBox* parent_container, Style::Position position);
	/// Returns the containing block of an element's box: as above, or its overriding containing block, if the box formatting it
	/// gave one (ContainerBox::AddAbsoluteElement).
	static ContainingBlock GetContainingBlock(ContainerBox* parent_container, Element* element);

	/// Builds margins of a Box, and resolves any auto width or height for non-inline elements. The height may be left unresolved if it depends on the
	/// element's children.
	/// @param[in,out] box The box to generate. The padding and borders, in addition to any definite content area, must be set on the box already.
	/// Auto width and height are specified by negative content size.
	/// @param[in] min_size The element's minimum width and height.
	/// @param[in] max_size The element's maximum width and height.
	/// @param[in] containing_block The size of the containing block.
	/// @param[in] element The element the box is being generated for.
	/// @param[in] box_context The formatting context in which the box is generated.
	/// @param[in] replaced_element True when the element is a replaced element.
	static void BuildBoxSizeAndMargins(Box& box, Vector2f min_size, Vector2f max_size, Vector2f containing_block, Element* element,
		BuildBoxMode box_context, bool replaced_element);

	/// Formats the element and returns the width of its contents: its shrink-to-fit (fit-content) width
	/// min(max-content, max(min-content, available)), the available width being the containing block's less the element's
	/// margins, borders and paddings, or 'available_width' (content-box) when given (>= 0).
	static float GetShrinkToFitWidth(Element* element, Vector2f containing_block, float available_width = -1.f);

	/// Intrinsic sizing (CSS Sizing 3): the element's min-content width, the width of its border box formatted under a
	/// min-content constraint (no width to give: its widest unbreakable content, every soft wrap opportunity taken). A
	/// definite 'width' is its own min-content width.
	/// @param[in] element The element to measure.
	/// @param[in] containing_block The containing block's size (negative: indefinite; percentages then resolve as 0).
	/// @return The width of the border box.
	static float GetMinContentWidth(Element* element, Vector2f containing_block);
	/// The element's max-content width: the width of its border box formatted without a width constraint (shrink-to-fit
	/// without the clamp to the available width). A definite 'width' is its own max-content width.
	/// @return The width of the border box.
	static float GetMaxContentWidth(Element* element, Vector2f containing_block);
	/// Formats the element with a definite border-box width and returns the height of its border box.
	static float GetContentHeightForWidth(Element* element, float border_box_width, Vector2f containing_block);

	/// Content-based sizes (CSS Sizing 3), for a box whose size its formatting context determines (flex items, absolutely
	/// positioned boxes, sizing keywords): the element's own 'width', 'height' and min-/max- sizes are ignored (the caller
	/// clamps where CSS does).
	/// The border-box width under an available width: its min-content or max-content width, or its fit-content width
	/// min(max-content, max(min-content, available)) for a definite one.
	/// @param[in] containing_block The containing block's size, negative where indefinite.
	/// @param[in] known_height A border-box height to transfer through the element's aspect ratio, if any (negative: none).
	static float GetContentWidth(Element* element, AvailableSpace available_width, Vector2f containing_block, float known_height = -1.f);
	/// The border-box height of the element laid out at a border-box width, from its aspect ratio if it has one, otherwise
	/// from its content.
	static float GetContentHeight(Element* element, float border_box_width, Vector2f containing_block);
	/// True while the content size of this element is measured (GetContentWidth, GetContentHeight): its layout box then
	/// reports its content width even when its 'width' is definite, and its min-/max- sizes do not apply.
	static bool IsMeasuringContentOf(const Element* element);

	/// The horizontal offset of an in-flow block-level box in its containing block by its 'justify-self' (CSS Box Alignment 3
	/// §6.1, the parent's 'justify-items' for 'auto'), given the free space beside its margin box. Zero for 'normal' and
	/// 'stretch', and for boxes with an 'auto' horizontal margin (the margins take the space).
	static float GetJustifySelfOffset(Element* element, float free_space);
	/// True if an in-flow block-level box with an 'auto' width is sized as 'fit-content' by its 'justify-self' (any value but
	/// 'normal', 'stretch', 'auto' resolving to them, and the baseline values).
	static bool IsJustifiedFitContent(Element* element);
	/// Moves the content of a block container (the elements positioned against it, through inline elements) down by 'delta',
	/// for its 'align-content'.
	static void ShiftContentOffsets(Element* container, float delta);
	/// True if the height of an element's box does not depend on its content (CSS Sizing 3 'definite'), as percentages of
	/// it need: a definite 'height', an absolutely positioned box with 'top' and 'bottom', a flex or grid item, the root.
	static bool HasDefiniteHeight(Element* element);
	/// The horizontal margins, borders and paddings of a box as they contribute to an intrinsic width (percentages of the
	/// indefinite width being measured count as 0).
	static float GetIntrinsicEdgesWidth(const LayoutBox& layout_box);

	/// Build computed axis size along the horizontal direction (width and friends).
	static ComputedAxisSize BuildComputedHorizontalSize(const ComputedValues& computed);
	/// Build computed axis size along the vertical direction (height and friends).
	static ComputedAxisSize BuildComputedVerticalSize(const ComputedValues& computed);
	/// Resolve edge sizes from a computed axis size.
	static void GetEdgeSizes(float& margin_a, float& margin_b, float& padding_border_a, float& padding_border_b,
		const ComputedAxisSize& computed_size, float base_value);

	static String GetDebugElementName(Element* element);

	/// True for a scroll container: 'overflow' other than 'visible' and 'clip' (after the value fix-up, either both axes
	/// scroll or neither does). A box with 'overflow: clip' clips its content but is not a scroll container.
	static bool IsScrollContainer(Style::Overflow overflow_x, Style::Overflow overflow_y)
	{
		return (overflow_x != Style::Overflow::Visible && overflow_x != Style::Overflow::Clip) ||
			(overflow_y != Style::Overflow::Visible && overflow_y != Style::Overflow::Clip);
	}

private:
	/// Appends the layout children of an element to a list (GetLayoutChildren without the ordering).
	static void AppendLayoutChildren(Element* parent, LayoutChildList& children);

	/// Calculates and returns the content size for replaced elements.
	static Vector2f CalculateSizeForReplacedElement(Vector2f specified_content_size, Vector2f min_size, Vector2f max_size, Vector2f intrinsic_size,
		float intrinsic_ratio);

	/// Builds the block-specific width and horizontal margins of a Box.
	/// @param[in,out] box The box to generate. The padding and borders must be set on the box already. The content area is used instead of the width
	/// property, and -1 means auto width.
	/// @param[in] computed The computed values of the element the box is being generated for.
	/// @param[in] min_width The minimum content width of the element.
	/// @param[in] max_width The maximum content width of the element.
	/// @param[in] containing_block The size of the containing block.
	/// @param[in] element The element the box is being generated for.
	/// @param[in] replaced_element True when the element is a replaced element.
	/// @param[in] override_shrink_to_fit_width Provide a fixed shrink-to-fit width instead of formatting the element when its properties allow
	/// shrinking.
	static void BuildBoxWidth(Box& box, const ComputedValues& computed, float min_width, float max_width, Vector2f containing_block, Element* element,
		bool replaced_element, float override_shrink_to_fit_width = -1);
	/// Builds the block-specific height and vertical margins of a Box.
	/// @param[in,out] box The box to generate. The padding and borders must be set on the box already. The content area is used instead of the height
	/// property, and -1 means auto height.
	/// @param[in] computed The computed values of the element the box is being generated for.
	/// @param[in] min_height The minimum content height of the element.
	/// @param[in] max_height The maximum content height of the element.
	/// @param[in] containing_block The size of the containing block (percentage margins resolve against its width).
	static void BuildBoxHeight(Box& box, const ComputedValues& computed, float min_height, float max_height, Vector2f containing_block);

	/// The width and horizontal margins of an absolutely positioned box (CSS Position 3 §4.1, §4.4): resolved against its
	/// inset-modified containing block, an 'auto' width being stretch-fit between two insets or fit-content otherwise, 'auto'
	/// margins taking the free space only between two insets.
	static void BuildAbsoluteBoxWidth(Box& box, const ComputedValues& computed, float min_width, float max_width, Vector2f containing_block,
		Element* element, bool replaced_element);
	/// The height and vertical margins of an absolutely positioned box, as BuildAbsoluteBoxWidth ('auto' between two insets
	/// is stretch-fit, otherwise the content decides).
	static void BuildAbsoluteBoxHeight(Box& box, const ComputedValues& computed, float min_height, float max_height, Vector2f containing_block,
		Element* element, bool replaced_element);
};

} // namespace Rml
