#pragma once

#include "../../../Include/RmlUi/Core/Box.h"
#include "../../../Include/RmlUi/Core/StyleTypes.h"
#include "InlineTypes.h"

namespace Rml {

class ElementText;
struct FontMetrics;

/**
    A box that takes part in inline layout.

    The inline-level box is used to generate fragments that are placed within line boxes.
 */
class InlineLevelBox {
public:
	virtual ~InlineLevelBox();

	// Create a fragment from this box, if it can fit within the available width.
	virtual FragmentConstructor CreateFragment(InlineLayoutMode mode, float available_width, float right_spacing_width, bool first_box,
		LayoutOverflowHandle overflow_handle) = 0;

	// Submit a fragment's position and size to be displayed on the underlying element.
	virtual void Submit(const PlacedFragment& placed_fragment) = 0;

	float GetHeightAboveBaseline() const { return height_above_baseline; }
	float GetDepthBelowBaseline() const { return depth_below_baseline; }
	Style::VerticalAlign::Type GetVerticalAlign() const { return vertical_align_type; }
	float GetVerticalOffsetFromParent() const { return vertical_offset_from_parent; }
	float GetSpacingLeft() const { return spacing_left; }
	float GetSpacingRight() const { return spacing_right; }

	virtual String DebugDumpNameValue() const = 0;
	virtual String DebugDumpTree(int depth) const;

	Element* GetElement() const { return element; }

	void* operator new(size_t size);
	void operator delete(void* chunk, size_t size);

protected:
	InlineLevelBox(Element* element) : element(element) { RMLUI_ASSERT(element); }
	const FontMetrics& GetFontMetrics() const;

	// Set the height used for inline layout, and the vertical offset relative to our parent box.
	void SetHeightAndVerticalAlignment(float height_above_baseline, float depth_below_baseline, const InlineLevelBox* parent);

	// Set the height used for inline layout.
	void SetHeight(float height_above_baseline, float depth_below_baseline);

	// Set the inner-to-outer spacing (margin + border + padding) for inline boxes.
	void SetInlineBoxSpacing(float spacing_left, float spacing_right);

	// Calls Element::OnLayout (proxy for private access to Element).
	void SubmitElementOnLayout();

private:
	float height_above_baseline = 0.f;
	float depth_below_baseline = 0.f;

	Style::VerticalAlign::Type vertical_align_type = {};
	float vertical_offset_from_parent = 0.f;

	float spacing_left = 0.f;  // Left margin-border-padding for inline boxes.
	float spacing_right = 0.f; // Right margin-border-padding for inline boxes.

	Element* element;
};

/**
    Atomic inline-level boxes are sized boxes that cannot be split.

    This includes inline-block elements, replaced inline-level elements, inline tables, and inline flex containers.
 */
class InlineLevelBox_Atomic final : public InlineLevelBox {
public:
	InlineLevelBox_Atomic(const InlineLevelBox* parent, Element* element, const Box& box);

	FragmentConstructor CreateFragment(InlineLayoutMode mode, float available_width, float right_spacing_width, bool first_box,
		LayoutOverflowHandle overflow_handle) override;

	void Submit(const PlacedFragment& placed_fragment) override;

	// Line clamping (fork, P6-L1): the ellipsis follows the border box, not the margin box (Chromium's line items).
	float GetMarginLeft() const;
	float GetBorderWidth() const;
	// False when it holds an absolutely positioned descendant: Chromium's LineTruncator does not hide it then.
	bool CanBeHiddenByEllipsis() const;

	String DebugDumpNameValue() const override { return "InlineLevelBox_Atomic"; }

private:
	Box box;
};

/**
    Inline-level text boxes represent text nodes.

    Generates fragments to display its text, splitting it up as necessary to fit in the available space.
 */
class InlineLevelBox_Text final : public InlineLevelBox {
public:
	InlineLevelBox_Text(ElementText* element);

	FragmentConstructor CreateFragment(InlineLayoutMode mode, float available_width, float right_spacing_width, bool first_box,
		LayoutOverflowHandle overflow_handle) override;

	void Submit(const PlacedFragment& placed_fragment) override;

	/// Line clamping (fork, P6-L1): cuts a fragment to the characters that fit in 'space' beside the ellipsis, as
	/// Chromium's LineTruncator::TruncateChild: the part on the line's start side (the start of a run in the line's
	/// direction, the end of one against it), whole clusters, at least one character for the first child of the line.
	/// @param[in] width The fragment's width.
	/// @param[in,out] justification The space of a justified line around the fragment's characters (p6-justify), or null:
	/// they are measured with it, and it is cut with them.
	/// @param[out] out_shift How far the kept part moves on a right-to-left line (its right edge stays in place).
	/// @return The width of the kept part, or a negative value when none fits (the fragment is unchanged).
	float TruncateForEllipsis(LayoutFragmentHandle handle, float width, float space, bool line_rtl, bool first_child,
		Vector<JustificationSpacing>* justification, float& out_shift);

	String DebugDumpNameValue() const override;

	ElementText* GetTextElement();
	// The text of a fragment, as it will be submitted (the line box trims the white space at the end of its line).
	String& GetFragmentText(LayoutFragmentHandle handle) { return fragments[handle]; }

private:
	Vector2f element_offset;
	StringList fragments;
};

} // namespace Rml
