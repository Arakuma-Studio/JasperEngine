/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports types/named.rs.
*/

#include "GridNamedLines.h"
#include "../GridStyleValues.h"
#include <algorithm>
#include <climits>

namespace Rml {
namespace GridLayout {

	static void AddLineNames(UnorderedMap<String, Vector<int>>& map, const GridTrackListValue* list, int auto_repetitions)
	{
		if (!list || list->none)
			return;
		int current_line = 0;
		for (size_t i = 0; i < list->line_names.size(); i++)
		{
			current_line += 1;
			for (const String& name : list->line_names[i])
				map[name].push_back(current_line);

			if (i < list->components.size() && list->components[i].is_repeat)
			{
				const GridTrackRepeat& repeat = list->components[i].repeat;
				const int repeat_count = repeat.IsAutoRepeat() ? auto_repetitions : repeat.repetitions;
				const int lines_per_repetition = (int)repeat.tracks.size();
				for (int r = 0; r < repeat_count; r++)
				{
					for (size_t j = 0; j < repeat.line_names.size(); j++)
					{
						for (const String& name : repeat.line_names[j])
							map[name].push_back(current_line + (int)j);
					}
					current_line += lines_per_repetition;
					// Names of lines beyond the track limit can never be resolved.
					if (current_line > MaxGridTracks)
						break;
				}
				// The last line names of the repeat merge with the names after it.
				if (repeat_count > 0)
					current_line = std::max(current_line - 1, 0);
			}
		}
	}

	NamedLineResolver::NamedLineResolver(const GridTrackListValue* columns, const GridTrackListValue* rows, const GridTemplateAreasValue* areas,
		int column_auto_repetitions, int row_auto_repetitions)
	{
		AddLineNames(column_lines, columns, column_auto_repetitions);
		AddLineNames(row_lines, rows, row_auto_repetitions);

		if (areas && !areas->rows.empty())
		{
			area_column_count = areas->num_columns;
			area_row_count = areas->num_rows;
			for (const GridTemplateArea& area : areas->areas)
			{
				column_lines[area.name + "-start"].push_back(area.column_start);
				column_lines[area.name + "-end"].push_back(area.column_end);
				row_lines[area.name + "-start"].push_back(area.row_start);
				row_lines[area.name + "-end"].push_back(area.row_end);
			}
		}

		for (LineMap* map : {&column_lines, &row_lines})
		{
			for (auto& name_lines : *map)
			{
				Vector<int>& lines = name_lines.second;
				std::sort(lines.begin(), lines.end());
				lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
			}
		}
	}

	int NamedLineResolver::FindLineIndex(const LineMap& lines, int explicit_track_count, const String& name, int index, bool is_start,
		int filter_min, int filter_max) const
	{
		// An index of 0 is "no index given".
		if (index == 0)
			index = 1;

		auto GetLine = [&](const Vector<int>& all_lines) -> int {
			// The lines strictly between the filter bounds.
			auto begin = std::upper_bound(all_lines.begin(), all_lines.end(), filter_min);
			auto end = std::lower_bound(begin, all_lines.end(), filter_max);
			const long count = long(end - begin);
			const long abs_index = std::labs(long(index));
			long line = 0;
			if (abs_index <= count)
				line = (index > 0 ? *(begin + (abs_index - 1)) : *(end - abs_index));
			else
			{
				// Past the named lines, every implicit line counts as having the name.
				const long remaining = (abs_index - count) * (index > 0 ? 1 : -1);
				line = (index > 0 ? long(explicit_track_count) + 1 + remaining : -(long(explicit_track_count) + 1 + remaining));
			}
			return int(std::max(long(SHRT_MIN), std::min(line, long(SHRT_MAX))));
		};

		auto it = lines.find(name);
		if (it != lines.end())
			return GetLine(it->second);

		it = lines.find(name + (is_start ? "-start" : "-end"));
		if (it != lines.end())
			return GetLine(it->second);

		// A name no line has matches the first implicit line after the explicit grid (or before it, for a negative index).
		const long line = (index > 0 ? long(explicit_track_count) + 1 + index : -(long(explicit_track_count) + 1 + index));
		return int(std::max(long(SHRT_MIN), std::min(line, long(SHRT_MAX))));
	}

	PlacementLine NamedLineResolver::ResolveNames(Axis axis, const GridLineValue* start, const GridLineValue* end) const
	{
		const LineMap& lines = (axis == Axis::Horizontal ? column_lines : row_lines);
		const int explicit_track_count = (axis == Axis::Horizontal ? explicit_column_count : explicit_row_count);

		using Kind = GridLineValue::Kind;
		const Kind start_kind = start ? start->kind : Kind::Auto;
		const Kind end_kind = end ? end->kind : Kind::Auto;

		// Named lines resolve on their own.
		bool start_is_line = false, end_is_line = false;
		int start_line = 0, end_line = 0;
		if (start_kind == Kind::NamedLine)
		{
			start_line = FindLineIndex(lines, explicit_track_count, start->name, start->index, true, INT_MIN, INT_MAX);
			start_is_line = true;
		}
		else if (start_kind == Kind::Line)
		{
			start_line = start->index;
			start_is_line = true;
		}
		if (end_kind == Kind::NamedLine)
		{
			end_line = FindLineIndex(lines, explicit_track_count, end->name, end->index, false, INT_MIN, INT_MAX);
			end_is_line = true;
		}
		else if (end_kind == Kind::Line)
		{
			end_line = end->index;
			end_is_line = true;
		}

		auto Normalize = [explicit_track_count](int line) { return line > 0 ? line : std::max(explicit_track_count + 1 + line, 0); };

		// A named span counts the lines of that name from the other, definite line.
		if (start_is_line && end_kind == Kind::NamedSpan)
		{
			const int found = FindLineIndex(lines, explicit_track_count, end->name, end->index, false, Normalize(start_line), INT_MAX);
			return PlacementLine{Placement::LineAt(start_line), Placement::LineAt(found)};
		}
		if (start_kind == Kind::NamedSpan && end_is_line)
		{
			const int found = FindLineIndex(lines, explicit_track_count, start->name, start->index, true, INT_MIN, Normalize(end_line));
			return PlacementLine{Placement::LineAt(found), Placement::LineAt(end_line)};
		}

		auto Convert = [](const GridLineValue* value, bool is_line, int line) -> Placement {
			if (is_line)
				return Placement::LineAt(line);
			if (!value)
				return Placement::Auto();
			switch (value->kind)
			{
			case Kind::Span: return Placement::Span(value->index);
			// A lone named span is a span of 1.
			case Kind::NamedSpan: return Placement::Span(1);
			default: return Placement::Auto();
			}
		};
		return PlacementLine{Convert(start, start_is_line, start_line), Convert(end, end_is_line, end_line)};
	}

} // namespace GridLayout
} // namespace Rml
