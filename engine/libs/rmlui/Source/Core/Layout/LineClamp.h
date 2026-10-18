#pragma once

#include "../../../Include/RmlUi/Core/Types.h"

namespace Rml {

/*
    Line clamping (fork, phase 6, P6-L1): Chromium 153's legacy '-webkit-line-clamp' (BlockLayoutAlgorithm and
    LineClampData with the CSSLineClamp flag off).

    A line-clamp container ('display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: N', computed to
    'flow-root') is the root of a block formatting context; it and the block boxes of that context that do not establish
    their own (not floats, inline-blocks, flex items, ...) share one context: every line box with content (and every line
    of nothing but a <br>) counts down 'lines_until_clamp' from N. The line it reaches 1 at is the clamp line: it is
    truncated to fit an ellipsis (LineBox::TruncateForLineClamp). Every block box of the context records the position after
    its first child line or box once the count is past the clamp point (0 or less): an automatic height ends there,
    without the margins after it or the floats below it (Chromium's legacy intrinsic block size). The content after the
    clamp point is laid out and painted as usual: 'overflow: hidden' hides it, as in Chromium.

    When the count ends at exactly 0 in the container, nothing follows the clamp line: the container is laid out again
    without clamping, so it has no ellipsis (BlockLayoutAlgorithm::FinishLayout, ShouldRelayoutWithNoForcedTruncate).
    A block box without lines that follows the clamp point (an empty block, a block establishing its own formatting
    context) counts as content after it (UpdateAfterLayout: 'lines_until_clamp' becomes -1).
*/
struct LineClampContext {
	// The lines left until the clamp point: 1 when the next line is the clamp line, 0 or less past it.
	int lines_until_clamp = 0;
};

} // namespace Rml
