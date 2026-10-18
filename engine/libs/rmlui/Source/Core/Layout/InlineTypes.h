#pragma once

#include "../../../Include/RmlUi/Core/Types.h"

namespace Rml {

struct JustificationSpacing;

using LayoutOverflowHandle = int;
using LayoutFragmentHandle = int;

enum class InlineLayoutMode {
	WrapAny,          // Allow wrapping to avoid overflow, even if nothing is placed.
	WrapAfterContent, // Allow wrapping to avoid overflow, but first place at least *some* content on this line.
	Nowrap,           // Place all content on this line, regardless of overflow.
};

enum class FragmentType : byte {
	Invalid,   // Could not be placed.
	InlineBox, // An inline box.
	SizedBox,  // Sized inline-level boxes that are not inline-boxes.
	TextRun,   // Text runs.
};

struct FragmentConstructor {
	FragmentType type = FragmentType::Invalid;
	float layout_width = 0.f;
	LayoutFragmentHandle fragment_handle = {}; // Handle to enable the inline-level box to reference any fragment-specific data.
	LayoutOverflowHandle overflow_handle = {}; // Overflow handle is non-zero when there is another fragment to be layed out.
	bool forced_break = false;                 // The text run ends at a preserved line feed: its line is ended by a forced break.
};

struct PlacedFragment {
	Element* offset_parent;
	LayoutFragmentHandle handle;
	Vector2f position;
	float layout_width;
	bool split_left;
	bool split_right;
	// The line is a zero-height line box (CSS 2.1 §9.4.2: nothing but empty inline boxes): an inline box on it is placed at
	// the line's top with no height, as Chromium gives such an empty span.
	bool collapsed_line = false;
	float line_top = 0.f; // The line's top, relative to the offset parent (with collapsed_line).
	// A text run of a justified line: the space added around its characters (p6-justify), null otherwise.
	const Vector<JustificationSpacing>* justification = nullptr;
};

} // namespace Rml
