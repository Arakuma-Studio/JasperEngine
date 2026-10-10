/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports explicit_grid.rs and implicit_grid.rs.
*/

#pragma once

#include "GridTypes.h"

namespace Rml {

class GridTrackListValue;
class GridAutoTracksValue;
class GridLineValue;
struct GridTrackSize;

namespace GridLayout {

	/// The track style of one axis of a grid container.
	struct GridAxisStyle {
		const GridTrackListValue* template_list = nullptr; // nullptr: 'none'
		const GridAutoTracksValue* auto_tracks = nullptr;  // nullptr: 'auto'
		TrackSizingFunction gap;                           // the gutter: a fixed sizing function
		Element* element = nullptr;                        // resolves relative lengths and calc()
	};

	/// The minimum and maximum track sizing functions of a <track-size>.
	void GetTrackSizingFunctions(const GridTrackSize& size, Element& element, TrackSizingFunction& min, TrackSizingFunction& max);

	enum class AutoRepeatStrategy {
		// The container has a definite size or maximum size: the most repetitions that do not overflow it.
		MaxRepetitionsThatDoNotOverflow,
		// Only a definite minimum size: the fewest repetitions that fill it.
		MinRepetitionsThatDoOverflow,
	};

	/// The number of repetitions of the auto repeat (0 without one) and the number of tracks of the explicit grid template
	/// in an axis (CSS Grid 2, 7.2.3.2).
	void ComputeExplicitGridSizeInAxis(const GridAxisStyle& style, OptionalFloat auto_fit_container_size, AutoRepeatStrategy strategy,
		int& auto_repetition_count, int& template_track_count);

	/// Builds the tracks of an axis ([gutter, track, gutter, ..., gutter]) from its template and implicit track sizes.
	/// @param[in] track_has_items Whether a track (index in the cell occupancy matrix) holds an item: empty 'auto-fit' tracks collapse.
	template <typename TrackHasItems>
	void InitializeGridTracks(GridTrackVec& tracks, const TrackCounts& counts, const GridAxisStyle& style, int auto_repetition_count,
		TrackHasItems track_has_items);

	/// The placement of an in-flow item as declared: its start and end grid-line values in each axis (nullptr: 'auto').
	struct ItemPlacementStyle {
		const GridLineValue* column_start = nullptr;
		const GridLineValue* column_end = nullptr;
		const GridLineValue* row_start = nullptr;
		const GridLineValue* row_end = nullptr;
	};

	/// Estimates the track counts of the whole grid from the items' declared placement (named lines ignored), to size the
	/// cell occupancy matrix: exact for the negative implicit and explicit tracks, a lower bound for the positive ones.
	void ComputeGridSizeEstimate(int explicit_column_count, int explicit_row_count, const Vector<ItemPlacementStyle>& items,
		TrackCounts& column_counts, TrackCounts& row_counts);

	// ---- Implementation of the template ----

	void InitializeGridTracksImpl(GridTrackVec& tracks, const TrackCounts& counts, const GridAxisStyle& style, int auto_repetition_count,
		const Vector<bool>& track_has_items);

	template <typename TrackHasItems>
	void InitializeGridTracks(GridTrackVec& tracks, const TrackCounts& counts, const GridAxisStyle& style, int auto_repetition_count,
		TrackHasItems track_has_items)
	{
		Vector<bool> has_items((size_t)counts.Len());
		for (int i = 0; i < counts.Len(); i++)
			has_items[i] = track_has_items(i);
		InitializeGridTracksImpl(tracks, counts, style, auto_repetition_count, has_items);
	}

} // namespace GridLayout
} // namespace Rml
