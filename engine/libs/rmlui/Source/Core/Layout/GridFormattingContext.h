/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports mod.rs (compute_grid_layout).
*/

#pragma once

#include "../../../Include/RmlUi/Core/Types.h"
#include "FormattingContext.h"
#include "GridNamedLines.h"
#include "GridTypes.h"

namespace Rml {

class Box;
class ContainerBox;
class GridContainer;
class LayoutBox;

namespace GridLayout {

	/**
	    The grid areas of the absolutely positioned boxes whose containing block is a grid container (CSS Grid 2, 10.1): from
	    their grid-placement properties, lines resolved against the grid's final tracks, absent or unknown lines at the padding
	    edges. Kept by the GridContainer until it lays out its absolutely positioned boxes.
	 */
	class AbsoluteAreaResolver {
	public:
		struct AxisTracks {
			TrackCounts counts;
			Vector<Line<float>> positions; // the start and end of each track, from the container's border box
			OptionalFloat empty_axis_line; // the axis' one grid line when it has no track
			float padding_start = 0.f, padding_end = 0.f;
		};

		AbsoluteAreaResolver(const NamedLineResolver& names, AxisTracks columns, AxisTracks rows);

		/// The grid area of an absolutely positioned box, relative to the container's border box.
		Rectanglef Resolve(Element* element) const;

	private:
		Line<float> ResolveAxis(Axis axis, const AxisTracks& tracks, Element* element) const;

		NamedLineResolver names;
		AxisTracks columns, rows;
	};

} // namespace GridLayout

/*
    Formats a grid container element and its grid items according to the CSS Grid layout rules: a port of Taffy's grid
    algorithm (placement, track sizing, alignment) on RmlUi's formatting of the items.
*/
class GridFormattingContext final : public FormattingContext {
public:
	/// Formats a grid container element and its items.
	static UniquePtr<LayoutBox> Format(ContainerBox* parent_container, Element* element, const Box* override_initial_box);

	/// The width of a grid container's content box under a max-content constraint: its columns sized for max-content.
	static float GetMaxContentWidth(Element* element, Vector2f containing_block);
	/// The width of a grid container's content box under a min-content constraint.
	static float GetMinContentWidth(Element* element, Vector2f containing_block);
};

} // namespace Rml
