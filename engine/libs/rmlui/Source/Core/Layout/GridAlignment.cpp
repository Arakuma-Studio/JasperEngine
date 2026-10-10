/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports alignment.rs and compute/common/alignment.rs.
*/

#include "GridAlignment.h"
#include <algorithm>

namespace Rml {
namespace GridLayout {

	ContentAlignment ToContentAlignment(Style::JustifyContent value, Style::OverflowAlignment overflow)
	{
		using Style::JustifyContent;
		ContentAlignment result;
		result.safe = (overflow == Style::OverflowAlignment::Safe);
		switch (value)
		{
		case JustifyContent::FlexStart:
		case JustifyContent::Start:
		case JustifyContent::Left: result.keyword = ContentAlign::Start; break;
		case JustifyContent::FlexEnd:
		case JustifyContent::End:
		case JustifyContent::Right: result.keyword = ContentAlign::End; break;
		case JustifyContent::Center: result.keyword = ContentAlign::Center; break;
		case JustifyContent::SpaceBetween: result.keyword = ContentAlign::SpaceBetween; break;
		case JustifyContent::SpaceAround: result.keyword = ContentAlign::SpaceAround; break;
		case JustifyContent::SpaceEvenly: result.keyword = ContentAlign::SpaceEvenly; break;
		// 'normal' behaves as 'stretch' in a grid container.
		case JustifyContent::Stretch:
		case JustifyContent::Normal: result.keyword = ContentAlign::Stretch; break;
		}
		return result;
	}

	ContentAlignment ToContentAlignment(Style::AlignContent value, Style::OverflowAlignment overflow)
	{
		using Style::AlignContent;
		ContentAlignment result;
		result.safe = (overflow == Style::OverflowAlignment::Safe);
		switch (value)
		{
		case AlignContent::FlexStart:
		case AlignContent::Start: result.keyword = ContentAlign::Start; break;
		case AlignContent::FlexEnd:
		case AlignContent::End: result.keyword = ContentAlign::End; break;
		case AlignContent::Center: result.keyword = ContentAlign::Center; break;
		case AlignContent::SpaceBetween: result.keyword = ContentAlign::SpaceBetween; break;
		case AlignContent::SpaceAround: result.keyword = ContentAlign::SpaceAround; break;
		case AlignContent::SpaceEvenly: result.keyword = ContentAlign::SpaceEvenly; break;
		case AlignContent::Stretch:
		case AlignContent::Normal: result.keyword = ContentAlign::Stretch; break;
		}
		return result;
	}

	ContentAlign ApplyAlignmentFallback(float free_space, int num_items, ContentAlignment alignment)
	{
		ContentAlign keyword = alignment.keyword;
		bool is_safe = alignment.safe;

		// 1. A single item, or overflowing items: the distributed alignments fall back to a position and become 'safe'.
		if (num_items <= 1 || free_space <= 0.f)
		{
			switch (keyword)
			{
			case ContentAlign::Stretch:
			case ContentAlign::SpaceBetween:
				keyword = ContentAlign::Start;
				is_safe = true;
				break;
			case ContentAlign::SpaceAround:
			case ContentAlign::SpaceEvenly:
				keyword = ContentAlign::Center;
				is_safe = true;
				break;
			default: break;
			}
		}

		// 2. Safe alignment falls back to 'start' when the subject overflows.
		if (free_space <= 0.f && is_safe)
			keyword = ContentAlign::Start;
		return keyword;
	}

	float ComputeAlignmentOffset(float free_space, int num_items, ContentAlign alignment, bool is_first)
	{
		if (is_first)
		{
			switch (alignment)
			{
			case ContentAlign::Start: return 0.f;
			case ContentAlign::End: return free_space;
			case ContentAlign::Center: return free_space / 2.f;
			case ContentAlign::Stretch: return 0.f;
			case ContentAlign::SpaceBetween: return 0.f;
			case ContentAlign::SpaceAround: return free_space >= 0.f ? (free_space / float(num_items)) / 2.f : free_space / 2.f;
			case ContentAlign::SpaceEvenly: return free_space >= 0.f ? free_space / float(num_items + 1) : free_space / 2.f;
			}
			return 0.f;
		}
		free_space = std::max(free_space, 0.f);
		switch (alignment)
		{
		case ContentAlign::Start:
		case ContentAlign::End:
		case ContentAlign::Center:
		case ContentAlign::Stretch: return 0.f;
		case ContentAlign::SpaceBetween: return free_space / float(num_items - 1);
		case ContentAlign::SpaceAround: return free_space / float(num_items);
		case ContentAlign::SpaceEvenly: return free_space / float(num_items + 1);
		}
		return 0.f;
	}

	void AlignTracks(float grid_container_content_box_size, Line<float> padding, Line<float> border, GridTrackVec& tracks,
		ContentAlignment track_alignment)
	{
		float used_size = 0.f;
		for (const GridTrack& track : tracks)
			used_size += track.base_size;
		const float free_space = grid_container_content_box_size - used_size;
		const float origin = padding.start + border.start;

		// The tracks that are not collapsed (gutters not counted).
		int num_tracks = 0;
		for (size_t i = 1; i < tracks.size(); i += 2)
			num_tracks += int(!tracks[i].is_collapsed);

		// Gutters are tracks here: no gap is applied at alignment.
		const ContentAlign alignment = ApplyAlignmentFallback(free_space, num_tracks, track_alignment);

		// With every track collapsed none takes the offset, but the grid's lines are still aligned: on the origin.
		const float empty_grid_offset = (num_tracks == 0 ? ComputeAlignmentOffset(free_space, num_tracks, alignment, true) : 0.f);

		float total_offset = origin + empty_grid_offset;
		bool seen_non_collapsed_track = false;
		for (size_t i = 0; i < tracks.size(); i++)
		{
			GridTrack& track = tracks[i];
			const bool is_gutter = (i % 2 == 0);
			const bool is_non_collapsed_track = !is_gutter && !track.is_collapsed;
			const bool is_first = is_non_collapsed_track && !seen_non_collapsed_track;
			const float offset = (is_non_collapsed_track ? ComputeAlignmentOffset(free_space, num_tracks, alignment, is_first) : 0.f);
			track.offset = total_offset + offset;
			total_offset = total_offset + offset + track.base_size;
			if (is_non_collapsed_track)
				seen_non_collapsed_track = true;
		}
	}

	float AlignItemWithinArea(Line<float> grid_area, SelfAlignment alignment, float resolved_size, Line<OptionalFloat> margin, float baseline_shim,
		Line<float>& resolved_margin)
	{
		const Line<float> non_auto_margin = {(margin.start ? *margin.start : 0.f) + baseline_shim, margin.end ? *margin.end : 0.f};
		const float grid_area_size = std::max(grid_area.end - grid_area.start, 0.f);
		const float free_space = std::max(grid_area_size - resolved_size - non_auto_margin.start - non_auto_margin.end, 0.f);

		// Auto margins take the free space.
		const int auto_margin_count = int(!margin.start) + int(!margin.end);
		const float auto_margin_size = (auto_margin_count > 0 ? free_space / float(auto_margin_count) : 0.f);
		resolved_margin = {(margin.start ? *margin.start : auto_margin_size) + baseline_shim, margin.end ? *margin.end : auto_margin_size};

		// 'safe' alignment of an overflowing item falls back to 'start'.
		const bool overflows = resolved_size + non_auto_margin.start + non_auto_margin.end > grid_area_size;
		SelfAlign keyword = alignment.keyword;
		if (alignment.safe && overflows)
			keyword = SelfAlign::Start;

		float offset = 0.f;
		switch (keyword)
		{
		// Baseline alignment is done by the baseline shim: the item is start-aligned with it.
		case SelfAlign::Start:
		case SelfAlign::Baseline:
		case SelfAlign::Stretch:
		case SelfAlign::Normal: offset = resolved_margin.start; break;
		case SelfAlign::End: offset = grid_area_size - resolved_size - resolved_margin.end; break;
		case SelfAlign::Center: offset = (grid_area_size - resolved_size + resolved_margin.start - resolved_margin.end) / 2.f; break;
		}
		return grid_area.start + offset;
	}

} // namespace GridLayout
} // namespace Rml
