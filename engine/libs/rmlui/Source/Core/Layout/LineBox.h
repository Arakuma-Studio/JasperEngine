#pragma once

#include "../../../Include/RmlUi/Core/StyleTypes.h"
#include "../../../Include/RmlUi/Core/TextShapingContext.h"
#include "InlineTypes.h"

namespace Rml {

class InlineBox;
class InlineBoxRoot;
class InlineLevelBox;

/*
    Horizontally places fragments generated from inline-level boxes.

    Inline boxes can nest other inline-level boxes, thereby creating a tree of inline-level boxes. This tree structure
    needs to be considered for the generated fragments as well, since their size and position depend on this tree
    structure.

    Note that a single inline-level box can generate multiple fragments, possibly placed in different line boxes.
*/
class LineBox final {
public:
	LineBox() = default;
	~LineBox();

	// Set the line box position and dimensions.
	void SetLineBox(Vector2f line_position, float line_width, float line_minimum_height);

	/// Generates a fragment from a box and places it on this line, if possible.
	/// @param[in] box The inline-level box to be placed.
	/// @param[in] layout_mode The inline layout mode, which affects the fragment generation.
	/// @param[in,out] inout_overflow_handle A handle to resume fragment generation from a partially placed box.
	/// @return True if the box should be placed again on a new line, either because the box could not fit or there is more content to be placed.
	bool AddBox(InlineLevelBox* box, InlineLayoutMode layout_mode, LayoutOverflowHandle& inout_overflow_handle);

	/// Closes the open inline box.
	/// @param[in] inline_box The inline box to be closed. Should match the currently open box, strictly used for verification.
	/// @note Only inline boxes need to be closed, not other inline-level boxes since they do not contain child boxes.
	void CloseInlineBox(InlineBox* inline_box);

	/// Vertically positions each fragment and sizes the line, after splitting any open inline boxes to be placed on a new line.
	/// @param[in] root_inline_box The root inline box of our inline container.
	/// @param[in] split_all_open_boxes Split all open inline boxes, even if they have no content.
	/// @param[out] out_height_of_line Resulting height of line. This can be different from the element's computed line-height property.
	/// @return The next line if any open fragments had to be split or wrapped down.
	UniquePtr<LineBox> DetermineVerticalPositioning(const InlineBoxRoot* root_inline_box, bool split_all_open_boxes, float& out_height_of_line);

	// Closes the line and submits all fragments. Thereby positioning, sizing, and placing their corresponding boxes.
	// The white space at the end of the line is removed (collapsible) or hangs ('pre-wrap'), and takes no part in the
	// alignment; a justified line without justification opportunities is aligned to its start (p6-justify).
	// @param[in] text_align The line's alignment, 'text-align', resolved.
	// @param[in] text_align_last The alignment of a last line, 'text-align-last', resolved: for a last line, also one whose
	// text ends at a preserved line feed (a forced break).
	// @param[in] last_line The line is the last of its container or ends with a <br>.
	// @param[in] rtl The direction of the inline container is right-to-left (its start is the right).
	// @note The line must have been vertically positioned before closing.
	void Close(Element* offset_parent, Vector2f offset_parent_position, Style::TextAlign text_align, Style::TextAlign text_align_last, bool last_line,
		bool rtl);

	/// Indents the line by 'text-indent' (p6-justify, P6-J2): a left-to-right line starts after the indent (at its first box),
	/// a right-to-left one ends before it.
	void SetIndent(float indent, bool rtl);
	/// The line was ended by a preserved line feed (a forced break). Available after close.
	bool EndsWithLineFeed() const { return ends_with_line_feed; }

	float GetBoxCursor() const { return box_cursor; }
	// The width the content of the line takes, with the indent at its end (right-to-left).
	float GetContentExtent() const { return box_cursor + end_indent; }
	Vector2f GetPosition() const { return line_position; }
	float GetLineWidth() const { return line_width; }
	float GetLineMinimumHeight() const { return line_minimum_height; }

	InlineBox* GetOpenInlineBox();

	bool IsClosed() const { return is_closed; }
	bool HasContent() const { return has_content; }

	// Returns true if this line should be collapsed, such that it takes up no height in the inline container.
	bool CanCollapseLine() const;

	// Returns the width of the contents of the line, relative to the left edge of the line box. Includes spacing due to horizontal alignment.
	// @note Only available after line has been closed.
	float GetExtentRight() const;

	// Returns the baseline position, relative to the top of the line box.
	// @note Only available after line has been closed.
	float GetBaseline() const;

	String DebugDumpTree(int depth) const;

	void* operator new(size_t size);
	void operator delete(void* chunk, size_t size);

private:
	using FragmentIndex = int;
	using VerticalAlignType = Style::VerticalAlign::Type;

	static constexpr FragmentIndex RootFragmentIndex = -1;

	struct Fragment {
		Fragment() = default;
		Fragment(InlineLevelBox* box, FragmentConstructor constructor, VerticalAlignType vertical_align, float position_x, FragmentIndex parent) :
			box(box), type(constructor.type), fragment_handle(constructor.fragment_handle), vertical_align(vertical_align), position(position_x, 0.f),
			layout_width(constructor.layout_width), parent(parent)
		{}

		InlineLevelBox* box = nullptr;
		FragmentType type = FragmentType::Invalid;
		LayoutFragmentHandle fragment_handle = {};
		VerticalAlignType vertical_align = {};

		// Layout state.
		Vector2f position;        // Position relative to start of the line, disregarding floats, (x: outer-left edge, y: baseline).
		float layout_width = 0.f; // Inner width for inline boxes, otherwise outer width.

		// Vertical alignment state.
		FragmentIndex parent = RootFragmentIndex;
		FragmentIndex aligned_subtree_root = RootFragmentIndex; // Index of the aligned subtree the fragment belongs to.
		float baseline_offset = 0.f;                            // Vertical offset from aligned subtree root baseline to our baseline.

		// For inline boxes.
		bool split_left = false;
		bool split_right = false;
		bool has_content = false;
		FragmentIndex children_end_index = 0; // One-past-last-child of this box, as index into fragment list.

		// For aligned subtree root.
		float max_ascent = 0.f;
		float max_descent = 0.f;
	};
	using FragmentList = Vector<Fragment>;

	// Place an open fragment.
	void CloseFragment(Fragment& open_fragment, float right_inner_edge_position);

	// Splits the line, returning a new line if there are any open fragments.
	UniquePtr<LineBox> SplitLine(bool split_all_open_boxes);

	// Moves the start of the line by the indent, and the open inline boxes with it (not for an outside list marker).
	void ApplyPendingIndent(InlineLevelBox* box);

	// The last character of the line's text when its last content is text ('\0' otherwise).
	char GetLastTextCharacter() const;
	// The closing of a line with white space at its end or justified (p6-justify), out of the common path.
	void CloseTrimmedOrJustified(Element* offset_parent, Vector2f offset_parent_position, Style::TextAlign text_align, bool rtl);
	// The offset that aligns content of the given width.
	float GetAlignmentOffset(Style::TextAlign text_align, float content_width) const;
	// Positions and submits the fragments, with the spacing of the justified text runs (by fragment index) when justified.
	template <bool justified>
	void SubmitFragments(Element* offset_parent, Vector2f offset_parent_position, const Vector<Vector<JustificationSpacing>>* justification);

	// Removes the collapsible white space at the end of the line from its text and the boxes around it (CSS Text 3
	// §4.1.3). Returns the width of the preserved spaces that hang at its end ('pre-wrap'); 'out_justify_end' is where the
	// justified content ends: the fragment and the byte offset in its text (the fragments after it are not justified).
	float TrimTrailingWhiteSpace(int& out_justify_end_fragment, int& out_justify_end_offset);
	// Changes the width of a fragment: the fragments after it move, the inline boxes around it grow or shrink.
	void ResizeFragment(int fragment_index, float delta);
	// Distributes the free space of the line over its justification opportunities (p6-justify). Returns false, and
	// changes nothing, when there are none.
	bool Justify(float space, bool rtl, int justify_end_fragment, int justify_end_offset, Vector<Vector<JustificationSpacing>>& out_spacing);

	// Vertically align all descendants of the subtree. Returns the ascent of the top-most box, and descent of the bottom-most box.
	void VerticallyAlignSubtree(int subtree_root_index, int children_end_index, float& max_ascent, float& max_descent);

	// Returns true if the fragment establishes an aligned subtree.
	static bool IsAlignedSubtreeRoot(const Fragment& fragment)
	{
		return (fragment.vertical_align == VerticalAlignType::Top || fragment.vertical_align == VerticalAlignType::Center ||
			fragment.vertical_align == VerticalAlignType::Bottom);
	}
	// Returns the aligned subtree root for a given fragment, based on its ancestors.
	FragmentIndex DetermineAlignedSubtreeRoot(FragmentIndex index) const
	{
		while (index != RootFragmentIndex)
		{
			const Fragment& fragment = fragments[index];
			if (IsAlignedSubtreeRoot(fragment))
				return index;
			index = fragment.parent;
		}
		return index;
	}

	template <typename Func>
	void ForAllOpenFragments(Func&& func)
	{
		FragmentIndex index = open_fragments_leaf;
		while (index != RootFragmentIndex)
		{
			Fragment& fragment = fragments[index];
			index = fragment.parent;
			func(fragment);
		}
	}

	// Position of the line, relative to our block formatting context root.
	Vector2f line_position;
	// Available space for the line. Based on our parent box content width, possibly shrinked due to floating boxes.
	float line_width = 0.f;
	// Lower-bound estimate for the line box height.
	float line_minimum_height = 0.f;

	// The horizontal cursor. This is the outer-right position of the last placed fragment.
	float box_cursor = 0.f;
	// The contribution of opened inline boxes to the placement of the next fragment, due to their left edges (margin-border-padding).
	float open_spacing_left = 0.f;

	// List of fragments in this line box.
	FragmentList fragments;

	// Index of the last open fragment. The list of open fragments is this leaf and all of its ancestors up to the root.
	FragmentIndex open_fragments_leaf = RootFragmentIndex;

	bool has_content = false;
	// The last text run or atomic inline placed, when it is a text run (its index), else -1.
	FragmentIndex last_text_run = -1;
	// 'text-indent' (P6-J2): the indent still to apply at the start (left to right), the indent at the end (right to left).
	float pending_indent = 0.f;
	float end_indent = 0.f;
	bool ends_with_line_feed = false;
	bool is_vertically_positioned = false;
	bool is_closed = false;

	// Content offset due to space distribution from 'text-align'. Available after close.
	float offset_horizontal_alignment = 0.f;
	// The right edge of a justified line's content (p6-justify). Available after close.
	float justified_extent = 0.f;
	// The line box's height above baseline. Available after close.
	float total_height_above_baseline = 0.f;
};

} // namespace Rml
