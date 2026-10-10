/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports types/cell_occupancy.rs.
*/

#pragma once

#include "GridTypes.h"

namespace Rml {
namespace GridLayout {

	enum class CellOccupancyState : uint8_t { Unoccupied, DefinitelyPlaced, AutoPlaced };

	/**
	    The occupied cells of the grid while items are placed (CSS Grid 2, 8.5), sparse: per track (in both orientations) a
	    sorted list of disjoint intervals of occupied cells, in origin-zero line coordinates along the track. Tracks are added
	    on either side as items need them; it keeps the track counts of both axes.
	 */
	class CellOccupancyMatrix {
	public:
		CellOccupancyMatrix(const TrackCounts& columns, const TrackCounts& rows);

		/// Marks an area (bounded by origin-zero lines) as occupied, growing the implicit grid to hold it.
		void MarkAreaAs(Axis primary_axis, Line<int> primary_span, Line<int> secondary_span, CellOccupancyState value);

		/// Checks an area for occupied cells. Returns false if it is entirely unoccupied; else true and, in 'next_position',
		/// the next search position along the primary axis that does not collide with the cells found.
		bool LineAreaCollisionJump(Axis primary_axis, Line<int> primary_span, Line<int> secondary_span, int& next_position) const;
		bool LineAreaIsUnoccupied(Axis primary_axis, Line<int> primary_span, Line<int> secondary_span) const;

		/// For a span of tracks in an axis: false if they are all unoccupied, else true and the line after the last occupied one.
		bool OccupiedTrackJump(Axis axis, Line<int> span, int& next_position) const;

		bool RowIsOccupied(int row_index) const;
		bool ColumnIsOccupied(int column_index) const;

		const TrackCounts& GetTrackCounts(Axis axis) const { return axis == Axis::Horizontal ? columns : rows; }

		/// In the track of the other axis that follows the line 'start_at', the last cell of the given state, as the line
		/// before it; false if there is none.
		bool LastOfType(Axis track_type, int start_at, CellOccupancyState kind, int& result) const;

	private:
		struct Interval {
			int start, end; // [start, end) in origin-zero lines
			CellOccupancyState state;
		};
		struct TrackIntervals {
			Vector<Interval> intervals;

			bool IsEmpty() const { return intervals.empty(); }
			void Paint(int start, int end, CellOccupancyState state);
			bool CollisionExtent(int start, int end, int& result) const;
			bool LastOfState(CellOccupancyState state, int& result) const;
		};

		const Vector<TrackIntervals>& TrackLists(Axis track_axis) const { return track_axis == Axis::Horizontal ? column_intervals : row_intervals; }
		void ExpandToFitRange(Line<int> row_span, Line<int> column_span);

		TrackCounts columns, rows;
		Vector<TrackIntervals> row_intervals;    // per row track: occupied cells in column lines
		Vector<TrackIntervals> column_intervals; // per column track: occupied cells in row lines
	};

} // namespace GridLayout
} // namespace Rml
