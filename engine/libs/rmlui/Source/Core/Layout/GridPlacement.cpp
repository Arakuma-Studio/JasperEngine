/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports placement.rs and the placement helpers of style/grid.rs.
*/

#include "GridPlacement.h"
#include "GridNamedLines.h"
#include <algorithm>
#include <climits>

namespace Rml {
namespace GridLayout {

	PlacementLine ToOriginZero(const PlacementLine& line, int explicit_track_count)
	{
		auto Convert = [explicit_track_count](const Placement& placement) {
			switch (placement.type)
			{
			case Placement::Type::Auto: return Placement::Auto();
			case Placement::Type::Span: return Placement::Span(std::max(1, std::min(placement.value, MaxGridTracks)));
			case Placement::Type::Line:
				// Line 0 is invalid: 'auto'.
				if (placement.value == 0)
					return Placement::Auto();
				return Placement::LineAt(GridLineToOriginZero(placement.value, explicit_track_count));
			}
			return Placement::Auto();
		};
		return PlacementLine{Convert(line.start), Convert(line.end)};
	}

	bool IsDefinite(const PlacementLine& oz_line)
	{
		return oz_line.start.type == Placement::Type::Line || oz_line.end.type == Placement::Type::Line;
	}

	Line<int> ResolveDefiniteGridLines(const PlacementLine& oz_line)
	{
		using Type = Placement::Type;
		const Placement& start = oz_line.start;
		const Placement& end = oz_line.end;
		if (start.type == Type::Line && end.type == Type::Line)
		{
			if (start.value == end.value)
				return {start.value, start.value + 1};
			return {std::min(start.value, end.value), std::max(start.value, end.value)};
		}
		if (start.type == Type::Line && end.type == Type::Span)
			return {start.value, start.value + end.value};
		if (start.type == Type::Line)
			return {start.value, start.value + 1};
		if (start.type == Type::Span && end.type == Type::Line)
			return {end.value - start.value, end.value};
		RMLUI_ASSERT(end.type == Type::Line);
		return {end.value - 1, end.value};
	}

	Line<std::optional<int>> ResolveAbsolutelyPositionedGridTracks(const PlacementLine& oz_line)
	{
		using Type = Placement::Type;
		const Placement& start = oz_line.start;
		const Placement& end = oz_line.end;
		if (start.type == Type::Line && end.type == Type::Line)
		{
			if (start.value == end.value)
				return {start.value, start.value + 1};
			return {std::min(start.value, end.value), std::max(start.value, end.value)};
		}
		if (start.type == Type::Line && end.type == Type::Span)
			return {start.value, start.value + end.value};
		if (start.type == Type::Line)
			return {start.value, std::nullopt};
		if (start.type == Type::Span && end.type == Type::Line)
			return {end.value - start.value, end.value};
		if (start.type == Type::Auto && end.type == Type::Line)
			return {std::nullopt, end.value};
		return {std::nullopt, std::nullopt};
	}

	// The span of an indefinite placement.
	static int IndefiniteSpan(const PlacementLine& line)
	{
		if (line.start.type == Placement::Type::Span)
			return line.start.value;
		if (line.end.type == Placement::Type::Span)
			return line.end.value;
		return 1;
	}

	static int AdvancePosition(int position)
	{
		return position >= INT_MAX ? position : position + 1;
	}

	static Line<int> ResolveIndefiniteGridSpan(int position, int span)
	{
		auto Clamp = [](long value) { return int(std::max(long(SHRT_MIN), std::min(value, long(SHRT_MAX)))); };
		return {Clamp(position), Clamp(long(position) + long(span))};
	}

	// A placement is kept inside the limited grid with a span of at least 1.
	static Line<int> ClampSpanToLimitedGrid(Line<int> span)
	{
		const int start = std::max(MinOzLine, std::min(span.start, MaxOzLine - 1));
		const int end = std::max(start + 1, std::min(span.end, MaxOzLine));
		return {start, end};
	}

	static bool IsDense(Style::GridAutoFlow flow)
	{
		return flow == Style::GridAutoFlow::RowDense || flow == Style::GridAutoFlow::ColumnDense;
	}

	// 'row' flow fills rows: the primary axis is the horizontal one, in which the cursor advances.
	static Axis PrimaryAxis(Style::GridAutoFlow flow)
	{
		return (flow == Style::GridAutoFlow::Row || flow == Style::GridAutoFlow::RowDense) ? Axis::Horizontal : Axis::Vertical;
	}

	struct ItemPlacement {
		PlacementLine horizontal, vertical;
		const PlacementLine& Get(Axis axis) const { return axis == Axis::Horizontal ? horizontal : vertical; }
	};

	static void RecordGridPlacement(CellOccupancyMatrix& matrix, GridItem& item, Axis primary_axis, Line<int> primary_span,
		Line<int> secondary_span, CellOccupancyState placement_type)
	{
		// Placements outside the limited grid are clamped into it (CSS Grid 2, overlarge grids).
		primary_span = ClampSpanToLimitedGrid(primary_span);
		secondary_span = ClampSpanToLimitedGrid(secondary_span);
		matrix.MarkAreaAs(primary_axis, primary_span, secondary_span, placement_type);
		if (primary_axis == Axis::Horizontal)
		{
			item.column = primary_span;
			item.row = secondary_span;
		}
		else
		{
			item.column = secondary_span;
			item.row = primary_span;
		}
	}

	// Step 2: an item with a definite position in the secondary axis only.
	static void PlaceDefiniteSecondaryAxisItem(const CellOccupancyMatrix& matrix, const ItemPlacement& placement, Style::GridAutoFlow auto_flow,
		Line<int>& primary_result, Line<int>& secondary_result)
	{
		const Axis primary_axis = PrimaryAxis(auto_flow);
		const Axis secondary_axis = OtherAxis(primary_axis);
		const int primary_axis_grid_start_line = matrix.GetTrackCounts(primary_axis).ImplicitStartLine();

		const Line<int> secondary_axis_placement = ResolveDefiniteGridLines(placement.Get(secondary_axis));
		int position = primary_axis_grid_start_line;
		if (!IsDense(auto_flow))
		{
			int last = 0;
			if (matrix.LastOfType(primary_axis, secondary_axis_placement.start, CellOccupancyState::AutoPlaced, last))
				position = last;
		}
		const int primary_axis_span = IndefiniteSpan(placement.Get(primary_axis));

		for (;;)
		{
			const Line<int> primary_axis_placement = ResolveIndefiniteGridSpan(position, primary_axis_span);
			int next_position = 0;
			if (!matrix.LineAreaCollisionJump(primary_axis, primary_axis_placement, secondary_axis_placement, next_position))
			{
				primary_result = primary_axis_placement;
				secondary_result = secondary_axis_placement;
				return;
			}
			position = next_position;
		}
	}

	// Step 4: an item with a definite position in the primary axis at most.
	static void PlaceIndefinitelyPositionedItem(const CellOccupancyMatrix& matrix, const ItemPlacement& placement, Style::GridAutoFlow auto_flow,
		int primary_idx, int secondary_idx, Line<int>& primary_result, Line<int>& secondary_result)
	{
		const Axis primary_axis = PrimaryAxis(auto_flow);
		const Axis secondary_axis = OtherAxis(primary_axis);
		const PlacementLine& primary_placement_style = placement.Get(primary_axis);
		const PlacementLine& secondary_placement_style = placement.Get(secondary_axis);

		const int secondary_span = IndefiniteSpan(secondary_placement_style);
		const int primary_axis_grid_start_line = matrix.GetTrackCounts(primary_axis).ImplicitStartLine();
		const int primary_axis_grid_end_line = matrix.GetTrackCounts(primary_axis).ImplicitEndLine();
		const int secondary_axis_grid_start_line = matrix.GetTrackCounts(secondary_axis).ImplicitStartLine();

		if (IsDefinite(primary_placement_style))
		{
			const Line<int> primary_span = ResolveDefiniteGridLines(primary_placement_style);
			// The search in the secondary axis starts at the cursor's track, or the next one when the item lies before the cursor.
			if (IsDense(auto_flow))
				secondary_idx = secondary_axis_grid_start_line;
			else if (primary_span.start < primary_idx)
				secondary_idx = AdvancePosition(secondary_idx);

			for (;;)
			{
				const Line<int> secondary_placement = ResolveIndefiniteGridSpan(secondary_idx, secondary_span);
				int next_position = 0;
				if (matrix.LineAreaCollisionJump(secondary_axis, secondary_placement, primary_span, next_position))
				{
					secondary_idx = next_position;
					continue;
				}
				primary_result = primary_span;
				secondary_result = secondary_placement;
				return;
			}
		}

		const int primary_span_count = IndefiniteSpan(primary_placement_style);
		// An item that spans every primary axis track only fits in a stripe of empty tracks.
		const bool spans_all_primary_tracks = primary_span_count >= matrix.GetTrackCounts(primary_axis).Len();

		// Search along the primary axis to the end of the tracks, then on the next secondary track.
		for (;;)
		{
			const Line<int> primary_span = ResolveIndefiniteGridSpan(primary_idx, primary_span_count);
			const Line<int> secondary_placement = ResolveIndefiniteGridSpan(secondary_idx, secondary_span);

			if (primary_span.end > primary_axis_grid_end_line)
			{
				// Out of bounds even at the start: it never fits, the placement is clamped when recorded.
				if (primary_idx == primary_axis_grid_start_line)
				{
					primary_result = primary_span;
					secondary_result = secondary_placement;
					return;
				}
				secondary_idx = AdvancePosition(secondary_idx);
				primary_idx = primary_axis_grid_start_line;
				continue;
			}

			if (spans_all_primary_tracks)
			{
				int next_position = 0;
				if (matrix.OccupiedTrackJump(secondary_axis, secondary_placement, next_position))
				{
					secondary_idx = next_position;
					primary_idx = primary_axis_grid_start_line;
					continue;
				}
				primary_result = primary_span;
				secondary_result = secondary_placement;
				return;
			}

			int next_position = 0;
			if (matrix.LineAreaCollisionJump(primary_axis, primary_span, secondary_placement, next_position))
			{
				primary_idx = next_position;
				continue;
			}
			primary_result = primary_span;
			secondary_result = secondary_placement;
			return;
		}
	}

	void PlaceGridItems(CellOccupancyMatrix& matrix, Vector<GridItem>& items, const Vector<ItemPlacementStyle>& placement_styles,
		Style::GridAutoFlow auto_flow, const NamedLineResolver& named_line_resolver)
	{
		const Axis primary_axis = PrimaryAxis(auto_flow);
		const Axis secondary_axis = OtherAxis(primary_axis);
		const int explicit_col_count = matrix.GetTrackCounts(Axis::Horizontal).explicit_count;
		const int explicit_row_count = matrix.GetTrackCounts(Axis::Vertical).explicit_count;

		// 0. Resolve each item's placement once.
		Vector<ItemPlacement> placements;
		placements.reserve(placement_styles.size());
		for (const ItemPlacementStyle& style : placement_styles)
		{
			ItemPlacement placement;
			placement.horizontal =
				ToOriginZero(named_line_resolver.ResolveNames(Axis::Horizontal, style.column_start, style.column_end), explicit_col_count);
			placement.vertical = ToOriginZero(named_line_resolver.ResolveNames(Axis::Vertical, style.row_start, style.row_end), explicit_row_count);
			placements.push_back(placement);
		}

		// 1. The items with a definite position in both axes.
		for (size_t i = 0; i < items.size(); i++)
		{
			const ItemPlacement& placement = placements[i];
			if (!(IsDefinite(placement.horizontal) && IsDefinite(placement.vertical)))
				continue;
			const Line<int> primary_span = ResolveDefiniteGridLines(placement.Get(primary_axis));
			const Line<int> secondary_span = ResolveDefiniteGridLines(placement.Get(secondary_axis));
			RecordGridPlacement(matrix, items[i], primary_axis, primary_span, secondary_span, CellOccupancyState::DefinitelyPlaced);
		}

		// 2. The items locked to a secondary axis track.
		for (size_t i = 0; i < items.size(); i++)
		{
			const ItemPlacement& placement = placements[i];
			if (!(IsDefinite(placement.Get(secondary_axis)) && !IsDefinite(placement.Get(primary_axis))))
				continue;
			Line<int> primary_span, secondary_span;
			PlaceDefiniteSecondaryAxisItem(matrix, placement, auto_flow, primary_span, secondary_span);
			RecordGridPlacement(matrix, items[i], primary_axis, primary_span, secondary_span, CellOccupancyState::AutoPlaced);
		}

		// 3. The implicit grid already holds the definite items (the estimate and MarkAreaAs grew it).

		// 4. The remaining items, by the auto-placement cursor.
		const int primary_axis_grid_start_line = matrix.GetTrackCounts(primary_axis).ImplicitStartLine();
		const int secondary_axis_grid_start_line = matrix.GetTrackCounts(secondary_axis).ImplicitStartLine();
		int cursor_primary = primary_axis_grid_start_line, cursor_secondary = secondary_axis_grid_start_line;
		for (size_t i = 0; i < items.size(); i++)
		{
			const ItemPlacement& placement = placements[i];
			if (IsDefinite(placement.Get(secondary_axis)))
				continue;
			Line<int> primary_span, secondary_span;
			PlaceIndefinitelyPositionedItem(matrix, placement, auto_flow, cursor_primary, cursor_secondary, primary_span, secondary_span);
			RecordGridPlacement(matrix, items[i], primary_axis, primary_span, secondary_span, CellOccupancyState::AutoPlaced);

			// 'dense' searches from the start for every item; otherwise the next item is placed after this one.
			if (IsDense(auto_flow))
			{
				cursor_primary = primary_axis_grid_start_line;
				cursor_secondary = secondary_axis_grid_start_line;
			}
			else
			{
				cursor_primary = primary_span.end;
				cursor_secondary = secondary_span.start;
			}
		}
	}

} // namespace GridLayout
} // namespace Rml
