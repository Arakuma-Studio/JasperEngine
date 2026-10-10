#pragma once

#include "../../../Include/RmlUi/Core/Math.h"
#include "../../../Include/RmlUi/Core/Types.h"
#include "ContainerBox.h"

namespace Rml {

class FloatedBoxSpace;
class LineBox;
class InlineBox;
class InlineContainer;
struct InlineBoxHandle {
	InlineBox* inline_box;
};

/// Vertical margins collapsing together (CSS 2.1 §8.3.1): the collapsed margin is the largest positive margin plus the most
/// negative one.
struct CollapsibleMarginSet {
	float positive = 0.f, negative = 0.f;

	void Add(float margin)
	{
		if (margin > 0.f)
			positive = Math::Max(positive, margin);
		else
			negative = Math::Min(negative, margin);
	}
	void Add(const CollapsibleMarginSet& other)
	{
		positive = Math::Max(positive, other.positive);
		negative = Math::Min(negative, other.negative);
	}
	float Resolve() const { return positive + negative; }
};

/**
    A container for block-level boxes.

    Takes part in a block formatting context. Block-level boxes are stacked vertically as they are added. Inline-level
    elements create a new inline container if one is not already open. The inline container itself acts as an anonymous
    block-level box in this block container.

    Positions are relative to the root of the block formatting context this box takes part in, unless otherwise noted.
    However, elements themselves are offset relative to their containing block. We act as the containing block for
    static and relative children, and possibly for absolutely positioned descendants.

 */
class BlockContainer final : public ContainerBox {
public:
	/// Creates a new block box for rendering a block element.
	/// @param[in] parent The parent of this block box. This will be nullptr for the root element.
	/// @param[in] element The element this block box is laying out.
	/// @param[in] box The box used for this block box.
	/// @param[in] min_height The minimum height of the content box.
	/// @param[in] max_height The maximum height of the content box.
	BlockContainer(ContainerBox* parent_container, FloatedBoxSpace* space, Element* element, const Box& box, float min_height, float max_height);
	~BlockContainer();

	/// Closes the box. This will determine this container's height if it was unspecified.
	/// @param[in] parent_block_container Our parent which will be sized to contain this box, or nullptr for the root of the block formatting context.
	/// @return False if the block box caused an automatic vertical scrollbar to appear, forcing a reformat of the current block formatting context.
	bool Close(BlockContainer* parent_block_container);

	/// Creates and opens a new block box, and adds it as a child of this one.
	/// @param[in] element The new block element.
	/// @param[in] box The dimensions of the new box, whose content height may be indefinite (-1).
	/// @param[in] min_height The minimum height of the content box.
	/// @param[in] max_height The maximum height of the content box.
	/// @return The block box representing the element. Once the element's children have been positioned, Close() must be called on it.
	BlockContainer* OpenBlockBox(Element* element, const Box& box, float min_height, float max_height);

	/// Adds a block-level box whose contents have already been formatted in an independent formatting context.
	/// @param[in] block_level_box The box to add as a new child of this.
	/// @param[in] element The element represented by the new box.
	/// @param[in] box The dimensions of the new box.
	/// @return A pointer to the added block-level box.
	LayoutBox* AddBlockLevelBox(UniquePtr<LayoutBox> block_level_box, Element* element, const Box& box);

	// Adds an element to this block container to be handled as a floating element.
	void AddFloatElement(Element* element, Vector2f visible_overflow_size);

	/// Adds a new inline-level element to this block container.
	/// @param[in] element The new inline element.
	/// @param[in] box The box defining the element's bounds.
	/// @return A handle for the inline element, which must later be submitted to 'CloseInlineElement()'.
	/// @note Adds a new inline container to this box if needed, which starts a new inline formatting context.
	InlineBoxHandle AddInlineElement(Element* element, const Box& box);
	/// Closes a previously added inline element. This must be called after all its children have been added.
	/// @param[in] handle A handle previously returned from 'AddInlineElement()'.
	void CloseInlineElement(InlineBoxHandle handle);

	// Adds a line-break to this block box.
	void AddBreak();

	// Estimates the static position of a hypothetical next element to be placed.
	Vector2f GetOpenStaticPosition(Style::Display display) const;

	/// Returns the position of a new child box to be placed here, relative to the block formatting context space: the
	/// cursor after the margins pending there (unless they collapse with our own top margin).
	Vector2f NextBoxPosition() const;

	// Places all queued floating elements.
	void PlaceQueuedFloats(float vertical_position);

	// Resets this box, so that it can be formatted again.
	void ResetContents();

	Vector2f GetPosition() const;
	Element* GetElement() const;
	const FloatedBoxSpace* GetBlockBoxSpace() const;

	Box& GetBox();
	const Box& GetBox() const;

	// -- Inherited from LayoutBox --

	/// True when inline content is open on the current line (white-space-only text then collapses to a space).
	bool HasOpenInlineContent() const { return GetOpenInlineContainer() != nullptr; }

	const Box* GetIfBox() const override;
	float GetShrinkToFitWidth() const override;
	float GetMinContentWidth() const override;
	bool GetBaselineOfLastLine(float& out_baseline) const override;
	bool GetBaselineOfFirstLine(float& out_baseline) const override;

private:
	// Shared by GetShrinkToFitWidth and GetMinContentWidth: the widest child (by 'child_width') and the floats, clamped.
	template <typename ChildWidth, typename FloatsWidth>
	float GetIntrinsicContentWidth(ChildWidth child_width, FloatsWidth floats_width) const;

	InlineContainer* EnsureOpenInlineContainer();
	InlineContainer* GetOpenInlineContainer();
	const InlineContainer* GetOpenInlineContainer() const;

	/// Positions the border box of a block-level child, relative to the block formatting context: after the margins before it
	/// collapsed with its top margin (CSS 2.1 §8.3.1), or at our content top when its top margin collapses with ours (we and
	/// the boxes whose top margins collapse with ours then move down by the change of the collapsed margin).
	/// @param[in] child_box The dimensions of the child box.
	/// @param[in] clear_property The floats the child clears.
	/// @param[out] out_top_margin_set The margins collapsed before the child's border top.
	/// @param[out] out_joins_top_margin True if the child's top margin collapses with ours.
	/// @param[out] out_clearance True if the child was moved below floats (clearance).
	/// @param[in] new_formatting_context True if the child establishes an independent formatting context.
	Vector2f PlaceBlockLevelChild(Element* child_element, const Box& child_box, Style::Clear clear_property, CollapsibleMarginSet& out_top_margin_set,
		bool& out_joins_top_margin, bool& out_clearance, bool new_formatting_context);
	/// Collapses more margins with our top margin: we, and the boxes whose top margins collapse with ours, move down by the
	/// change of the collapsed margin.
	void CollapseTopMargin(const CollapsibleMarginSet& margin_set);

	/// Increment our cursor and content size, to enclose a sized block-level child box.
	/// @param[in] child The child box.
	/// @param[in] child_position The border position of the child, relative to the current block formatting context.
	/// @param[in] child_border_size The border size of the child.
	/// @param[in] child_margin_bottom_right The edge size of the bottom-right margin corner.
	/// @param[in] child_bottom_margin_set The margins collapsing below the child's border bottom.
	/// @param[in] child_collapsed_through True if the child's top and bottom margins collapse through it (an empty box).
	/// @param[in] child_joined_top_margin True if the child's top margin collapses with ours.
	/// @param[in] child_clearance True if the child was moved below floats (clearance).
	/// @return False if this caused an automatic vertical scrollbar to appear, forcing a reformat of the current block formatting context.
	bool EncloseChildBox(LayoutBox* child, Vector2f child_position, Vector2f child_border_size, Vector2f child_margin_bottom_right,
		const CollapsibleMarginSet& child_bottom_margin_set, bool child_collapsed_through, bool child_joined_top_margin, bool child_clearance);

	// Moves our content by 'align-content' in our free height (when we are the root of a block formatting context).
	void AlignContent();

	// Closes the inline container if there is one open. Returns false if our formatting context needs to be reformatted.
	bool CloseOpenInlineContainer();

	// Ensures that the interrupted line box is empty, otherwise it indicates an internal error.
	void EnsureEmptyInterruptedLineBox();

	// Positions a floating element within this block box.
	void PlaceFloat(Element* element, float vertical_position, Vector2f visible_overflow_size);

	// Debug dump layout tree.
	String DebugDumpTree(int depth) const override;

	using LayoutBoxList = Vector<UniquePtr<LayoutBox>>;
	struct QueuedFloat {
		Element* element;
		Vector2f visible_overflow_size;
	};
	using QueuedFloatList = Vector<QueuedFloat>;

	// Position of this box, relative to the border area of the root of our block formatting context.
	Vector2f position;

	Box box;
	float min_height = 0.f;
	float max_height = -1.f;

	// The vertical position of the next block box to be added to this box, relative to our box's top content edge.
	float box_cursor = 0.f;

	// Stores the floated boxes in the current block formatting context, if we are the root of the formatting context.
	UniquePtr<FloatedBoxSpace> root_space;
	// Pointer to the floated box space of the current block formatting context. [not-null]
	FloatedBoxSpace* space;
	// List of block-level boxes contained in this box.
	LayoutBoxList child_boxes;
	// Stores floated elements that are waiting for a line break to be positioned.
	QueuedFloatList queued_float_elements;
	// Stores the unplaced part of a line box that was split by a block-level box.
	UniquePtr<LineBox> interrupted_line_box;

	// The inner content size (excluding any padding/border/margins).
	// This is extended as child block boxes are closed, or from external formatting contexts.
	Vector2f inner_content_size;

	// The bottom-right extent of the margin boxes of our direct children, relative to our content area. Contributes to our own scrollable overflow.
	Vector2f inner_content_margin_size;

	// Margin collapsing (CSS 2.1 §8.3.1, phase 3). Our cursor ('box_cursor') is below the last in-flow content, the margins
	// after it are pending in 'trailing_margin_set'.
	CollapsibleMarginSet top_margin_set;         // The margins collapsed above our border top: ours and those adjoining it.
	BlockContainer* top_margin_parent = nullptr; // Our parent, while our top margin collapses with its top margin.
	CollapsibleMarginSet trailing_margin_set;    // The margins after the cursor.
	bool top_margin_open = false;                // No in-flow content yet, and our first child's top margin collapses with ours.
	CollapsibleMarginSet bottom_margin_set;      // After closing: the margins collapsing below our border bottom.
	bool collapsed_through = false;              // After closing: our top and bottom margins collapse through us (we are empty).
	bool joined_parent_top_margin = false;       // Our top margin collapses with our parent's.
	bool clearance = false;                      // We were moved below floats.
	bool adjoining_floats = false;               // Floats were placed while our top margin was open (CSS 2.1 §9.5).
};

} // namespace Rml
