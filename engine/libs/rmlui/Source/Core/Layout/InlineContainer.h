#pragma once

#include "../../../Include/RmlUi/Core/Box.h"
#include "../../../Include/RmlUi/Core/Types.h"
#include "InlineBox.h"
#include "LayoutBox.h"

namespace Rml {

class BlockContainer;
class LineBox;
struct LineClampContext;
struct LineClampEllipsis;

/**
    A container for inline-level boxes.

    Always a direct child of a block container where it acts as a block-level box, and starts a new inline formatting
    context. It maintains a stack of line boxes in which fragments generated from inline-level boxes are placed within.
    Not a proper CSS term, but effectively a "block container that only contains inline-level boxes".
 */
class InlineContainer final : public LayoutBox {
public:
	/// Creates a new block box in an inline context.
	InlineContainer(BlockContainer* parent, float available_width);
	~InlineContainer();

	/// Adds a new inline-level element to this inline-context box.
	/// @param[in] element The new inline-level element.
	/// @param[in] box The box defining the element's bounds.
	/// @return The inline box if one was generated for the elmeent, otherwise nullptr.
	/// @note Any non-null return value must be closed with a call to CloseInlineElement().
	InlineBox* AddInlineElement(Element* element, const Box& box);

	/// Closes the previously added inline box.
	/// @param[in] inline_box The box to close.
	/// @note Calls to this function should be submitted in reverse order to AddInlineElement().
	void CloseInlineElement(InlineBox* inline_box);

	/// Add a break to the last line.
	void AddBreak(float line_height);
	/// Adds a line box for resuming one that was previously split.
	/// @param[in] open_line_box The line box overflowing from a previous inline container.
	void AddChainedBox(UniquePtr<LineBox> open_line_box);

	/// Closes the box. This will determine the element's height (if it was unspecified).
	/// @param[out] out_open_line_box Optionally, output the open inline box.
	/// @param[out] out_position Outputs the position of this container.
	/// @param[out] out_height Outputs the height of this container.
	void Close(UniquePtr<LineBox>* out_open_line_box, Vector2f& out_position, float& out_height);

	/// Update the placement of the open line box, e.g. to account for newly placed floats.
	void UpdateOpenLineBoxPlacement();

	/// Returns the position and tentative size of the currently open line box, if any.
	bool GetOpenLineBoxDimensions(float& out_vertical_position, Vector2f& out_tentative_size) const;
	/// Returns an estimate for the position of a hypothetical next box to be placed, relative to the content box of this container.
	Vector2f GetStaticPositionEstimate(bool inline_level_box) const;

	/// Line clamping (fork, P6-L1): the height of this container's lines up to and including the first one closed past
	/// the clamp point (the clamp line, or the first line when it starts past it), or a negative value.
	float GetLineClampHeight() const { return clamp_height; }

	// -- Inherited from LayoutBox --

	float GetShrinkToFitWidth() const override;
	bool GetBaselineOfLastLine(float& out_baseline) const override;
	bool GetBaselineOfFirstLine(float& out_baseline) const override; // (p3-layout: flex baseline alignment)
	String DebugDumpTree(int depth) const override;

private:
	using LineBoxList = Vector<UniquePtr<LineBox>>;

	LineBox* EnsureOpenLineBox();
	LineBox* GetOpenLineBox() const;
	InlineBoxBase* GetOpenInlineBox();

	/// Close any open line box.
	/// @param[in] split_all_open_boxes Split all open inline boxes, even if they have no content.
	/// @param[out] out_split_line Optionally return any resulting split line, otherwise it will be added as a new line box to this container.
	void CloseOpenLineBox(bool split_all_open_boxes, UniquePtr<LineBox>* out_split_line = nullptr);

	/// Find and set the position and line width for the currently open line box.
	/// @param[in] line_box The currently open line box.
	/// @param[in] minimum_width The minimum line width to consider for this search.
	/// @param[in] minimum_height The minimum line height to to be considered for this and future searches.
	void UpdateLineBoxPlacement(LineBox* line_box, float minimum_width, float minimum_height);

	// Line clamping (fork, P6-L1): counts a line box before it is closed, and makes room for the ellipsis on the clamp
	// line; after it is closed, records the clamp height. A line of nothing but a <br> counts too (AddBreak).
	void CountLineForLineClamp(LineBox* line_box);
	void CountEmptyLineForLineClamp();
	void RecordLineClampHeight();

	BlockContainer* parent; // [not-null]

	Vector2f position;
	Vector2f box_size;

	// The element's computed line-height. Not necessarily the same as the height of our lines.
	float element_line_height = 0.f;
	// True if content should wrap instead of overflowing the line box.
	bool wrap_content = false;
	// The element's text-align property.
	Style::TextAlign text_align = {};
	// The element's direction is right-to-left (fork, P6-L1: overflowing lines and the line-clamp ellipsis).
	bool rtl = false;

	// The vertical position of the currently open line, or otherwise the next one to be placed, relative to the top of this box.
	float box_cursor = 0;

	// The root of the tree of inline boxes located in this inline container.
	InlineBoxRoot root_inline_box;

	// The list of line boxes, each of which flows fragments generated by inline boxes.
	// @performance Investigate using a list backed by the pools instead, to avoid allocations.
	LineBoxList line_boxes;

	// Line clamping (fork, P6-L1; LineClamp.h): the line-clamp context of our block formatting context, if any; the height
	// of our lines up to the clamp point (GetLineClampHeight); the first line laid out past the clamp point (it and those
	// after it give no baseline, as in Chromium).
	LineClampContext* line_clamp = nullptr;
	float clamp_height = -1.f;
	int first_past_clamp_line = -1;
	// The ellipsis of our clamp line, which the line places and its fragments copy when it is closed.
	UniquePtr<LineClampEllipsis> clamp_ellipsis;
};

} // namespace Rml
