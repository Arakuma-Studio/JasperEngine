/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports alignment.rs and compute/common/alignment.rs.
*/

#pragma once

#include "GridTypes.h"

namespace Rml {
namespace GridLayout {

	/// The content alignment of a grid container ('justify-content', 'align-content'), 'normal' as 'stretch'.
	ContentAlignment ToContentAlignment(Style::JustifyContent value, Style::OverflowAlignment overflow);
	ContentAlignment ToContentAlignment(Style::AlignContent value, Style::OverflowAlignment overflow);

	/// The fallbacks of a content distribution (CSS Box Alignment 3, 4.3): a single track or no free space falls back to a
	/// positional alignment, 'safe' alignment to 'start' when the tracks overflow.
	ContentAlign ApplyAlignmentFallback(float free_space, int num_items, ContentAlignment alignment);
	/// The offset of a track before it: the first track's from the content edge, the others' from the previous track.
	float ComputeAlignmentOffset(float free_space, int num_items, ContentAlign alignment, bool is_first);

	/// Positions the tracks and gutters of an axis in the container (content alignment), relative to its border box.
	void AlignTracks(float grid_container_content_box_size, Line<float> padding, Line<float> border, GridTrackVec& tracks,
		ContentAlignment track_alignment);

	/// Aligns a box of a size within its grid area in one axis: returns its border-box start and resolves its margins
	/// (auto margins take the free space).
	/// @param[in] margin The margins, absent when 'auto'.
	float AlignItemWithinArea(Line<float> grid_area, SelfAlignment alignment, float resolved_size, Line<OptionalFloat> margin, float baseline_shim,
		Line<float>& resolved_margin);

} // namespace GridLayout
} // namespace Rml
