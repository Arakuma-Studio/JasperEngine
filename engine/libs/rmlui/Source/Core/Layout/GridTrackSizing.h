/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports track_sizing.rs.
*/

#pragma once

#include "GridItemMeasure.h"
#include "GridTypes.h"

namespace Rml {
namespace GridLayout {

	/// The extra size of each inner gutter of the other axis when items are measured: the space 'space-*' content
	/// alignment would add, if the container and the other axis' tracks have definite sizes.
	float ComputeAlignmentGutterAdjustment(ContentAlignment alignment, OptionalFloat axis_inner_node_size, TrackSizeEstimate estimate,
		const GridTrackVec& tracks);

	/// Converts the items' origin-zero placements to indexes into the track vectors.
	void ResolveItemTrackIndexes(Vector<GridItem>& items, const TrackCounts& column_counts, const TrackCounts& row_counts);

	/// Whether each item crosses a flexible or an intrinsic track, in each axis.
	void DetermineIfItemCrossesFlexibleOrIntrinsicTracks(Vector<GridItem>& items, const GridTrackVec& columns, const GridTrackVec& rows);

	/// The track sizing algorithm in one axis (CSS Grid 2, 12.3-12.8). Gutters are sized as fixed tracks.
	/// @param[in] estimate How the sizes of the other axis' tracks are estimated while items are measured.
	void TrackSizingAlgorithm(Axis axis, OptionalFloat axis_min_size, OptionalFloat axis_max_size, ContentAlignment axis_alignment,
		ContentAlignment other_axis_alignment, AxisPair<AvailableSpace> available_grid_space, SizeOptional inner_node_size,
		GridTrackVec& axis_tracks, GridTrackVec& other_axis_tracks, Vector<GridItem>& items, TrackSizeEstimate estimate,
		bool has_baseline_aligned_item);

} // namespace GridLayout
} // namespace Rml
