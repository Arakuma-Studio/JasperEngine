/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports placement.rs and the placement helpers of style/grid.rs.
*/

#pragma once

#include "GridExplicit.h"
#include "GridOccupancy.h"

namespace Rml {
namespace GridLayout {

	class NamedLineResolver;

	/// A placement in CSS line numbers to origin-zero lines (a line 0 is 'auto', spans clamped to [1, MaxGridTracks]).
	PlacementLine ToOriginZero(const PlacementLine& line, int explicit_track_count);
	bool IsDefinite(const PlacementLine& oz_line);
	/// The lines of a definite origin-zero placement (CSS Grid 2, 8.3.1: swapped when reversed, a line pair of one line spans 1).
	Line<int> ResolveDefiniteGridLines(const PlacementLine& oz_line);
	/// The lines of an absolutely positioned item's origin-zero placement; a side that is 'auto' or an unresolvable span
	/// is absent (the padding edge).
	Line<std::optional<int>> ResolveAbsolutelyPositionedGridTracks(const PlacementLine& oz_line);

	/// Places the items (CSS Grid 2, 8.5): those with definite positions, those locked to a row (column), then the others by
	/// the auto-placement cursor ('dense' restarts it for each item). The items are in order-modified document order.
	/// @param[in] placements The declared placement of each item, in the order of 'items'.
	void PlaceGridItems(CellOccupancyMatrix& cell_occupancy_matrix, Vector<GridItem>& items, const Vector<ItemPlacementStyle>& placements,
		Style::GridAutoFlow auto_flow, const NamedLineResolver& named_line_resolver);

} // namespace GridLayout
} // namespace Rml
