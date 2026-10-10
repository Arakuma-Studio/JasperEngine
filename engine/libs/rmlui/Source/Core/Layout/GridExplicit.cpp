/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports explicit_grid.rs and implicit_grid.rs.
*/

#include "GridExplicit.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../GridStyleValues.h"
#include <algorithm>
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "../Calculation.h"
#endif

namespace Rml {
namespace GridLayout {

	static TrackLength ToTrackLength(const LengthPercentageValue& value, Element& element)
	{
		TrackLength result;
#ifdef RMLUI_MATH_EXPRESSIONS
		if (value.calculation)
		{
			result.type = TrackLength::Type::Calc;
			result.calculation = value.calculation.get();
			result.element = &element;
			result.calculation_uses_percentage = Any(value.calculation->GetDependencyMask() & Unit::PERCENT);
			return result;
		}
#endif
		if (value.value.unit == Unit::PERCENT)
		{
			result.type = TrackLength::Type::Percent;
			result.value = value.value.number * 0.01f;
		}
		else
		{
			result.type = TrackLength::Type::Px;
			result.value = element.ResolveLength(value.value);
		}
		return result;
	}

	static TrackSizingFunction ToTrackSizingFunction(const GridTrackBreadth& breadth, Element& element)
	{
		TrackSizingFunction result;
		switch (breadth.kind)
		{
		case GridTrackBreadth::Kind::Length:
			result.type = TrackSizingFunction::Type::Length;
			result.length = ToTrackLength(breadth.length, element);
			break;
		case GridTrackBreadth::Kind::Auto: result.type = TrackSizingFunction::Type::Auto; break;
		case GridTrackBreadth::Kind::MinContent: result.type = TrackSizingFunction::Type::MinContent; break;
		case GridTrackBreadth::Kind::MaxContent: result.type = TrackSizingFunction::Type::MaxContent; break;
		case GridTrackBreadth::Kind::Fr:
			result.type = TrackSizingFunction::Type::Fr;
			result.fr = breadth.fr;
			break;
		}
		return result;
	}

	void GetTrackSizingFunctions(const GridTrackSize& size, Element& element, TrackSizingFunction& min, TrackSizingFunction& max)
	{
		switch (size.form)
		{
		case GridTrackSize::Form::Breadth:
			max = ToTrackSizingFunction(size.max, element);
			// A flexible minimum is 'auto' (CSS Grid 2, 7.2.4).
			min = max.IsFr() ? TrackSizingFunction::AutoFunction() : max;
			break;
		case GridTrackSize::Form::MinMax:
			min = ToTrackSizingFunction(size.min, element);
			max = ToTrackSizingFunction(size.max, element);
			break;
		case GridTrackSize::Form::FitContent:
			min = TrackSizingFunction::AutoFunction();
			max.type = TrackSizingFunction::Type::FitContent;
			max.length = ToTrackLength(size.fit_content_limit, element);
			break;
		}
	}

	// A track size for the auto-repeat count: its maximum if definite, else its minimum, the maximum floored by the minimum.
	static float TrackDefiniteValue(const GridTrackSize& size, Element& element, OptionalFloat parent_size)
	{
		TrackSizingFunction min, max;
		GetTrackSizingFunctions(size, element, min, max);
		const OptionalFloat max_size = max.DefiniteValue(parent_size);
		const OptionalFloat min_size = min.DefiniteValue(parent_size);
		if (max_size)
			return MaybeMax(*max_size, min_size);
		return min_size ? *min_size : 0.f;
	}

	void ComputeExplicitGridSizeInAxis(const GridAxisStyle& style, OptionalFloat auto_fit_container_size, AutoRepeatStrategy strategy,
		int& auto_repetition_count, int& template_track_count)
	{
		auto_repetition_count = 0;
		template_track_count = 0;
		const GridTrackListValue* list = style.template_list;
		if (!list || list->none || list->components.empty())
			return;
		Element& element = *style.element;

		// The tracks of the single sizes and of the fixed repeat()s, clamped to the track limit.
		long non_auto_repeating_track_count = 0;
		int num_auto_repeats = 0;
		const GridTrackRepeat* auto_repeat = nullptr;
		long auto_repeat_insertion_point = 0;
		for (const GridTemplateComponent& component : list->components)
		{
			if (!component.is_repeat)
			{
				non_auto_repeating_track_count += 1;
				if (!auto_repeat)
					auto_repeat_insertion_point += 1;
			}
			else if (component.repeat.IsAutoRepeat())
			{
				num_auto_repeats++;
				if (!auto_repeat)
					auto_repeat = &component.repeat;
			}
			else
			{
				const long tracks = long(component.repeat.repetitions) * long(component.repeat.tracks.size());
				non_auto_repeating_track_count += tracks;
				if (!auto_repeat)
					auto_repeat_insertion_point += tracks;
			}
			if (component.is_repeat && component.repeat.tracks.empty())
				return;
		}
		non_auto_repeating_track_count = std::min(non_auto_repeating_track_count, long(MaxGridTracks));

		// The parser accepts one auto repeat, and only with fixed sizes: an invalid template has no explicit tracks.
		if (num_auto_repeats > 1)
			return;
		if (num_auto_repeats == 0)
		{
			template_track_count = int(non_auto_repeating_track_count);
			return;
		}

		const int repetition_track_count = (int)auto_repeat->tracks.size();
		long num_repetitions = 1;
		if (auto_fit_container_size)
		{
			const float inner_container_size = *auto_fit_container_size;
			const OptionalFloat parent_size = inner_container_size;

			float non_repeating_track_used_space = 0.f;
			for (const GridTemplateComponent& component : list->components)
			{
				if (!component.is_repeat)
					non_repeating_track_used_space += TrackDefiniteValue(component.track, element, parent_size);
				else if (!component.repeat.IsAutoRepeat())
				{
					float sum = 0.f;
					for (const GridTrackSize& track : component.repeat.tracks)
						sum += TrackDefiniteValue(track, element, parent_size);
					non_repeating_track_used_space += sum * float(component.repeat.repetitions);
				}
			}
			const OptionalFloat gap = style.gap.DefiniteValue(inner_container_size);
			const float gap_size = gap ? *gap : 0.f;

			float per_repetition_track_used_space = 0.f;
			for (const GridTrackSize& track : auto_repeat->tracks)
				per_repetition_track_used_space += TrackDefiniteValue(track, element, parent_size);

			// The first repetition with the other tracks: its gutters depend on the number of the other tracks.
			const float first_repetition_and_non_repeating_tracks_used_space = non_repeating_track_used_space + per_repetition_track_used_space +
				float(std::max(non_auto_repeating_track_count + long(repetition_track_count) - 1, 0L)) * gap_size;

			// The number of repetitions is at least 1.
			if (first_repetition_and_non_repeating_tracks_used_space > inner_container_size)
				num_repetitions = 1;
			else
			{
				const float per_repetition_gap_used_space = float(repetition_track_count) * gap_size;
				const float per_repetition_used_space = per_repetition_track_used_space + per_repetition_gap_used_space;
				const float num_repetition_that_fit =
					(inner_container_size - first_repetition_and_non_repeating_tracks_used_space) / per_repetition_used_space;
				const double extra = (strategy == AutoRepeatStrategy::MaxRepetitionsThatDoNotOverflow ? std::floor(double(num_repetition_that_fit))
																									 : std::ceil(double(num_repetition_that_fit)));
				// Zero-sized repetitions fit infinitely often: clamped below.
				num_repetitions = (!std::isfinite(extra) || extra > double(MaxGridTracks) ? long(MaxGridTracks) : long(std::max(extra, 0.0))) + 1;
			}
		}

		// Clamp the repetitions so that the tracks before the auto repeat are kept and the tracks after it are truncated.
		const long remaining_tracks = std::max(long(MaxGridTracks) - auto_repeat_insertion_point, 0L);
		if (remaining_tracks == 0)
			num_repetitions = 0;
		else
		{
			const long max_repetitions = (remaining_tracks + repetition_track_count - 1) / repetition_track_count;
			num_repetitions = std::max(1L, std::min(num_repetitions, max_repetitions));
		}

		auto_repetition_count = int(num_repetitions);
		template_track_count = int(std::min(non_auto_repeating_track_count + long(repetition_track_count) * num_repetitions, long(MaxGridTracks)));
	}

	// Appends 'count' implicit tracks sized by the auto track list, cycling it from 'offset'.
	static void CreateImplicitTracks(GridTrackVec& tracks, int count, const GridAxisStyle& style, size_t offset)
	{
		const GridAutoTracksValue* auto_tracks = style.auto_tracks;
		const size_t auto_track_count = auto_tracks ? auto_tracks->tracks.size() : 0;
		for (int i = 0; i < count; i++)
		{
			TrackSizingFunction min, max;
			if (auto_track_count == 0)
			{
				min = TrackSizingFunction::AutoFunction();
				max = TrackSizingFunction::AutoFunction();
			}
			else
				GetTrackSizingFunctions(auto_tracks->tracks[(offset + (size_t)i) % auto_track_count], *style.element, min, max);
			tracks.push_back(GridTrack::Track(min, max));
			tracks.push_back(GridTrack::Gutter(style.gap));
		}
	}

	void InitializeGridTracksImpl(GridTrackVec& tracks, const TrackCounts& counts, const GridAxisStyle& style, int auto_repetition_count,
		const Vector<bool>& track_has_items)
	{
		tracks.clear();
		tracks.reserve((size_t)counts.Len() * 2 + 1);
		tracks.push_back(GridTrack::Gutter(style.gap));

		const size_t auto_track_count = style.auto_tracks ? style.auto_tracks->tracks.size() : 0;

		// The negative implicit tracks repeat the auto tracks backwards from the explicit grid: the last auto track is the one
		// just before it.
		if (counts.negative_implicit > 0)
		{
			const size_t offset = auto_track_count == 0 ? 0 : auto_track_count - ((size_t)counts.negative_implicit % auto_track_count);
			CreateImplicitTracks(tracks, counts.negative_implicit, style, offset);
		}

		int current_track_index = counts.negative_implicit;
		// The explicit track count is clamped to the track limit: the template is truncated to match.
		const int explicit_track_limit = counts.negative_implicit + counts.explicit_count;
		Element& element = *style.element;

		auto AddTrack = [&](const GridTrackSize& size) {
			TrackSizingFunction min, max;
			GetTrackSizingFunctions(size, element, min, max);
			tracks.push_back(GridTrack::Track(min, max));
			tracks.push_back(GridTrack::Gutter(style.gap));
			current_track_index++;
		};

		// A count of zero results from an invalid template too: then the template is ignored.
		const GridTrackListValue* list = style.template_list;
		if (counts.explicit_count > 0 && list && !list->none)
		{
			for (const GridTemplateComponent& component : list->components)
			{
				if (!component.is_repeat)
				{
					if (current_track_index < explicit_track_limit)
						AddTrack(component.track);
					continue;
				}
				const GridTrackRepeat& repeat = component.repeat;
				const int track_count = (int)repeat.tracks.size();
				if (!repeat.IsAutoRepeat())
				{
					const int repeated_track_count = std::min(track_count * repeat.repetitions, explicit_track_limit - current_track_index);
					for (int i = 0; i < repeated_track_count; i++)
						AddTrack(repeat.tracks[(size_t)(i % track_count)]);
					continue;
				}

				const int auto_repeated_track_count = std::min(track_count * auto_repetition_count, explicit_track_limit - current_track_index);
				for (int i = 0; i < auto_repeated_track_count; i++)
				{
					const bool collapse = (repeat.count == GridTrackRepeat::Count::AutoFit &&
						!(current_track_index < (int)track_has_items.size() && track_has_items[(size_t)current_track_index]));
					AddTrack(repeat.tracks[(size_t)(i % track_count)]);
					// Empty auto-fit tracks collapse, with the gutter after them.
					if (collapse)
					{
						tracks[tracks.size() - 2].Collapse();
						tracks.back().Collapse();
					}
				}

				// The gutter before a collapsed track is kept, which is right when a track that is not collapsed follows. An
				// auto-fit repeat at the end of the list has none: collapse back to the last track that is not collapsed.
				const bool is_last = (current_track_index == counts.Len());
				if (repeat.count == GridTrackRepeat::Count::AutoFit && is_last)
				{
					for (auto it = tracks.rbegin(); it != tracks.rend(); ++it)
					{
						if (it->kind == TrackKind::Track && !it->is_collapsed)
							break;
						it->Collapse();
					}
				}
			}
		}

		// Tracks the template areas add beyond the template, then the positive implicit tracks.
		const int grid_area_tracks = (counts.negative_implicit + counts.explicit_count) - current_track_index;
		CreateImplicitTracks(tracks, counts.positive_implicit + grid_area_tracks, style, 0);

		// The first and last grid lines take no space.
		tracks.front().Collapse();
		tracks.back().Collapse();
	}

	// ---- implicit_grid.rs ----

	// The declared placement in origin-zero lines, named lines read as 'auto'.
	static Placement ToOriginZeroIgnoringNamed(const GridLineValue* value, int explicit_track_count)
	{
		if (!value)
			return Placement::Auto();
		switch (value->kind)
		{
		case GridLineValue::Kind::Span: return Placement::Span(std::max(1, std::min(value->index, MaxGridTracks)));
		case GridLineValue::Kind::Line:
			if (value->index == 0)
				return Placement::Auto();
			return Placement::LineAt(GridLineToOriginZero(value->index, explicit_track_count));
		default: return Placement::Auto();
		}
	}

	static int IndefiniteSpan(const PlacementLine& line)
	{
		using Type = Placement::Type;
		if (line.start.type == Type::Span)
			return line.start.value;
		if (line.end.type == Type::Span)
			return line.end.value;
		return 1;
	}

	// The smallest and largest origin-zero lines an item uses (named lines ignored), and its span when it has no line.
	static void ChildMinLineMaxLineSpan(const GridLineValue* start, const GridLineValue* end, int explicit_track_count, int& min_line,
		int& max_line, int& span)
	{
		using Type = Placement::Type;
		const PlacementLine oz = {ToOriginZeroIgnoringNamed(start, explicit_track_count), ToOriginZeroIgnoringNamed(end, explicit_track_count)};
		const bool start_line = (oz.start.type == Type::Line), end_line = (oz.end.type == Type::Line);

		min_line = 0;
		max_line = 0;
		if (start_line && end_line)
		{
			const int a = oz.start.value, b = oz.end.value;
			min_line = (a == b ? a : std::min(a, b));
			max_line = (a == b ? a + 1 : std::max(a, b));
		}
		else if (start_line)
		{
			min_line = oz.start.value;
			max_line = oz.start.value + (oz.end.type == Type::Span ? oz.end.value : 1);
		}
		else if (end_line)
		{
			// An auto start with a definite end is a span of 1 ending there.
			min_line = oz.end.value - (oz.start.type == Type::Span ? oz.start.value : 1);
			max_line = oz.end.value;
		}

		span = (!start_line && !end_line) ? IndefiniteSpan(oz) : 1;
		min_line = std::max(min_line, MinOzLine);
		max_line = std::min(max_line, MaxOzLine);
	}

	void ComputeGridSizeEstimate(int explicit_column_count, int explicit_row_count, const Vector<ItemPlacementStyle>& items,
		TrackCounts& column_counts, TrackCounts& row_counts)
	{
		int col_min = 0, col_max = 0, col_max_span = 0;
		int row_min = 0, row_max = 0, row_max_span = 0;
		for (const ItemPlacementStyle& item : items)
		{
			int min_line, max_line, span;
			ChildMinLineMaxLineSpan(item.column_start, item.column_end, explicit_column_count, min_line, max_line, span);
			col_min = std::min(col_min, min_line);
			col_max = std::max(col_max, max_line);
			col_max_span = std::max(col_max_span, span);
			ChildMinLineMaxLineSpan(item.row_start, item.row_end, explicit_row_count, min_line, max_line, span);
			row_min = std::min(row_min, min_line);
			row_max = std::max(row_max, max_line);
			row_max_span = std::max(row_max_span, span);
		}

		column_counts.negative_implicit = ImpliedNegativeImplicitTracks(col_min);
		column_counts.explicit_count = explicit_column_count;
		column_counts.positive_implicit = ImpliedPositiveImplicitTracks(col_max, explicit_column_count);
		row_counts.negative_implicit = ImpliedNegativeImplicitTracks(row_min);
		row_counts.explicit_count = explicit_row_count;
		row_counts.positive_implicit = ImpliedPositiveImplicitTracks(row_max, explicit_row_count);

		// Room for the widest span of an item without a line.
		if (column_counts.Len() < col_max_span)
			column_counts.positive_implicit = col_max_span - column_counts.explicit_count - column_counts.negative_implicit;
		if (row_counts.Len() < row_max_span)
			row_counts.positive_implicit = row_max_span - row_counts.explicit_count - row_counts.negative_implicit;
	}

} // namespace GridLayout
} // namespace Rml
