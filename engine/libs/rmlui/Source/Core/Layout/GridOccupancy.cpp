/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports types/cell_occupancy.rs.
*/

#include "GridOccupancy.h"
#include <algorithm>

namespace Rml {
namespace GridLayout {

	void CellOccupancyMatrix::TrackIntervals::Paint(int start, int end, CellOccupancyState state)
	{
		if (start >= end)
			return;

		// The common case: the range lies after every interval (placement mostly advances forwards).
		if (intervals.empty())
		{
			intervals.push_back(Interval{start, end, state});
			return;
		}
		Interval& last = intervals.back();
		if (last.end <= start)
		{
			if (last.state == state && last.end == start)
				last.end = end;
			else
				intervals.push_back(Interval{start, end, state});
			return;
		}

		Vector<Interval> result;
		result.reserve(intervals.size() + 2);
		// The parts of intervals before the range.
		for (const Interval& interval : intervals)
		{
			if (interval.end <= start)
				result.push_back(interval);
			else if (interval.start < start)
				result.push_back(Interval{interval.start, start, interval.state});
		}
		// The range, merged with the preceding interval when it touches it with the same state.
		if (!result.empty() && result.back().state == state && result.back().end == start)
			result.back().end = end;
		else
			result.push_back(Interval{start, end, state});
		// The parts of intervals after the range.
		for (const Interval& interval : intervals)
		{
			Interval trimmed;
			if (interval.start >= end)
				trimmed = interval;
			else if (interval.end > end)
				trimmed = Interval{end, interval.end, interval.state};
			else
				continue;
			if (result.back().state == trimmed.state && result.back().end == trimmed.start)
				result.back().end = trimmed.end;
			else
				result.push_back(trimmed);
		}
		intervals = std::move(result);
	}

	bool CellOccupancyMatrix::TrackIntervals::CollisionExtent(int start, int end, int& result) const
	{
		// The last cell of the last interval the range overlaps.
		for (auto it = intervals.rbegin(); it != intervals.rend(); ++it)
		{
			if (it->start < end && it->end > start)
			{
				result = it->end - 1;
				return true;
			}
		}
		return false;
	}

	bool CellOccupancyMatrix::TrackIntervals::LastOfState(CellOccupancyState state, int& result) const
	{
		for (auto it = intervals.rbegin(); it != intervals.rend(); ++it)
		{
			if (it->state == state)
			{
				result = it->end - 1;
				return true;
			}
		}
		return false;
	}

	CellOccupancyMatrix::CellOccupancyMatrix(const TrackCounts& columns, const TrackCounts& rows) : columns(columns), rows(rows)
	{
		row_intervals.resize(rows.Len());
		column_intervals.resize(columns.Len());
	}

	void CellOccupancyMatrix::ExpandToFitRange(Line<int> row_span, Line<int> column_span)
	{
		const int req_negative_rows = std::max(-rows.negative_implicit - row_span.start, 0);
		const int req_positive_rows = std::max(row_span.end - rows.ImplicitEndLine(), 0);
		const int req_negative_cols = std::max(-columns.negative_implicit - column_span.start, 0);
		const int req_positive_cols = std::max(column_span.end - columns.ImplicitEndLine(), 0);

		// The intervals are in origin-zero coordinates: tracks are only added, nothing shifts.
		if (req_negative_rows > 0)
			row_intervals.insert(row_intervals.begin(), (size_t)req_negative_rows, TrackIntervals());
		if (req_positive_rows > 0)
			row_intervals.resize(row_intervals.size() + (size_t)req_positive_rows);
		if (req_negative_cols > 0)
			column_intervals.insert(column_intervals.begin(), (size_t)req_negative_cols, TrackIntervals());
		if (req_positive_cols > 0)
			column_intervals.resize(column_intervals.size() + (size_t)req_positive_cols);

		rows.negative_implicit += req_negative_rows;
		rows.positive_implicit += req_positive_rows;
		columns.negative_implicit += req_negative_cols;
		columns.positive_implicit += req_positive_cols;
	}

	void CellOccupancyMatrix::MarkAreaAs(Axis primary_axis, Line<int> primary_span, Line<int> secondary_span, CellOccupancyState value)
	{
		const Line<int> row_span = (primary_axis == Axis::Horizontal ? secondary_span : primary_span);
		const Line<int> column_span = (primary_axis == Axis::Horizontal ? primary_span : secondary_span);

		ExpandToFitRange(row_span, column_span);

		const int row_begin = rows.OzLineToNextTrack(row_span.start), row_end = rows.OzLineToNextTrack(row_span.end);
		const int column_begin = columns.OzLineToNextTrack(column_span.start), column_end = columns.OzLineToNextTrack(column_span.end);
		for (int row_index = row_begin; row_index < row_end; row_index++)
			row_intervals[row_index].Paint(column_span.start, column_span.end, value);
		for (int column_index = column_begin; column_index < column_end; column_index++)
			column_intervals[column_index].Paint(row_span.start, row_span.end, value);
	}

	bool CellOccupancyMatrix::LineAreaCollisionJump(Axis primary_axis, Line<int> primary_span, Line<int> secondary_span, int& next_position) const
	{
		const Axis secondary_axis = OtherAxis(primary_axis);
		const Vector<TrackIntervals>& track_lists = TrackLists(secondary_axis);
		const TrackCounts& secondary_counts = GetTrackCounts(secondary_axis);

		// Cells outside the tracks are unoccupied.
		const int secondary_start = std::max(secondary_counts.OzLineToNextTrack(secondary_span.start), 0);
		const int secondary_end = std::min(secondary_counts.OzLineToNextTrack(secondary_span.end), (int)track_lists.size());

		bool found = false;
		int extent = 0;
		for (int index = secondary_start; index < secondary_end; index++)
		{
			int cell = 0;
			if (!track_lists[index].CollisionExtent(primary_span.start, primary_span.end, cell))
				continue;
			extent = found ? std::max(extent, cell) : cell;
			found = true;
		}
		if (found)
			next_position = extent + 1;
		return found;
	}

	bool CellOccupancyMatrix::LineAreaIsUnoccupied(Axis primary_axis, Line<int> primary_span, Line<int> secondary_span) const
	{
		int next_position = 0;
		return !LineAreaCollisionJump(primary_axis, primary_span, secondary_span, next_position);
	}

	bool CellOccupancyMatrix::OccupiedTrackJump(Axis axis, Line<int> span, int& next_position) const
	{
		const TrackCounts& counts = GetTrackCounts(axis);
		const Vector<TrackIntervals>& track_lists = TrackLists(axis);
		const int start = std::max(counts.OzLineToNextTrack(span.start), 0);
		const int end = std::min(counts.OzLineToNextTrack(span.end), (int)track_lists.size());
		for (int index = end - 1; index >= start; index--)
		{
			if (!track_lists[index].IsEmpty())
			{
				next_position = counts.TrackToPrevOzLine(index) + 1;
				return true;
			}
		}
		return false;
	}

	bool CellOccupancyMatrix::RowIsOccupied(int row_index) const
	{
		return row_index >= 0 && row_index < (int)row_intervals.size() && !row_intervals[row_index].IsEmpty();
	}

	bool CellOccupancyMatrix::ColumnIsOccupied(int column_index) const
	{
		return column_index >= 0 && column_index < (int)column_intervals.size() && !column_intervals[column_index].IsEmpty();
	}

	bool CellOccupancyMatrix::LastOfType(Axis track_type, int start_at, CellOccupancyState kind, int& result) const
	{
		const Axis other = OtherAxis(track_type);
		const TrackCounts& track_counts = GetTrackCounts(other);
		const int index = track_counts.OzLineToNextTrack(start_at);
		const Vector<TrackIntervals>& track_lists = TrackLists(other);
		if (index < 0 || index >= (int)track_lists.size())
			return false;
		return track_lists[index].LastOfState(kind, result);
	}

} // namespace GridLayout
} // namespace Rml
