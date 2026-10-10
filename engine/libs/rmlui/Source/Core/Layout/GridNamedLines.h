/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports types/named.rs.
*/

#pragma once

#include "GridTypes.h"

namespace Rml {

class GridTrackListValue;
class GridTemplateAreasValue;
class GridLineValue;

namespace GridLayout {

	/**
	    Resolves the line names of grid placements ('grid-row-start: header-end', 'span col 2') into line numbers, from the
	    names of the track templates (repeat()s expanded) and the implicit '<area>-start'/'<area>-end' names of the template
	    areas. A name no line has refers to the first implicit line after (or before) the explicit grid (CSS Grid 2, 8.3).
	 */
	class NamedLineResolver {
	public:
		NamedLineResolver(const GridTrackListValue* columns, const GridTrackListValue* rows, const GridTemplateAreasValue* areas,
			int column_auto_repetitions, int row_auto_repetitions);

		int AreaColumnCount() const { return area_column_count; }
		int AreaRowCount() const { return area_row_count; }
		void SetExplicitColumnCount(int count) { explicit_column_count = count; }
		void SetExplicitRowCount(int count) { explicit_row_count = count; }

		/// The placement of an item in an axis (its start and end grid-line values, nullptr: 'auto') with the names
		/// resolved: CSS line numbers and spans.
		PlacementLine ResolveNames(Axis axis, const GridLineValue* start, const GridLineValue* end) const;

	private:
		using LineMap = UnorderedMap<String, Vector<int>>;

		int FindLineIndex(const LineMap& lines, int explicit_track_count, const String& name, int index, bool is_start, int filter_min,
			int filter_max) const;

		LineMap column_lines, row_lines;
		int area_column_count = 0, area_row_count = 0;
		int explicit_column_count = 0, explicit_row_count = 0;
	};

} // namespace GridLayout
} // namespace Rml
