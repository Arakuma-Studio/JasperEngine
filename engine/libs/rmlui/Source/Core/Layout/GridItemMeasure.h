/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports the contribution functions of types/grid_item.rs, and measures the items with RmlUi's layout: Taffy
    measures border boxes under known dimensions and available space, RmlUi formats content boxes (phase-3 spec 3.14.3).
*/

#pragma once

#include "GridTypes.h"

namespace Rml {
namespace GridLayout {

	/// How the size of a track of the other axis is estimated while items are measured (CSS Grid 2, 12.1): its maximum
	/// track sizing function if definite (the first column sizing), or its base size (afterwards).
	enum class TrackSizeEstimate : uint8_t { MaxTrackSizingFunction, BaseSize };

	OptionalFloat MaybeResolve(const Style::LengthPercentageAuto& value, OptionalFloat basis);
	/// A maximum size: 'none' (FLT_MAX) is absent.
	OptionalFloat MaybeResolveMax(const Style::LengthPercentage& value, OptionalFloat basis);
	float ResolveOrZero(const Style::LengthPercentage& value, OptionalFloat basis);

	/// Reads the style of an item: sizes, edges, overflow, and its self alignment (with the container's '*-items').
	void ReadGridItemStyle(GridItem& item, Style::AlignItems container_align_items, Style::JustifyItems container_justify_items);

	/// The padding and border of the item, resolved against the width of its grid area (0 when indefinite).
	AxisPair<float> PaddingBorderSize(const GridItem& item, OptionalFloat width_basis);
	/// The sum of the item's margins in each axis (percentages of 'inner_node_width'; horizontal ones of 0, as browsers do
	/// for intrinsic contributions), with the baseline shim.
	AxisPair<float> MarginsAxisSumsWithBaselineShims(const GridItem& item, OptionalFloat inner_node_width);

	/// The size of the grid area of an item: the sum of its tracks in 'axis' when they are all fixed, and the estimated
	/// size of its tracks in the other axis.
	SizeOptional GridAreaSize(const GridItem& item, Axis axis, const GridTrackVec& axis_tracks, const GridTrackVec& other_axis_tracks,
		SizeOptional inner_node_size, TrackSizeEstimate estimate);
	SizeOptional GridAreaSizeCached(GridItem& item, Axis axis, const GridTrackVec& axis_tracks, const GridTrackVec& other_axis_tracks,
		SizeOptional inner_node_size, TrackSizeEstimate estimate);
	/// The size estimate of a track of the other axis.
	OptionalFloat TrackSizeEstimateOf(const GridTrack& track, OptionalFloat parent_size, TrackSizeEstimate estimate);

	/// Measures the border-box size of an item in an axis (Taffy's measure_child_size): a known size is its own; widths
	/// are min-content, max-content or fit-content widths; heights are formatted for the known or measured width.
	float MeasureChildSize(GridItem& item, SizeOptional known_dimensions, SizeOptional parent_size, AxisPair<AvailableSpace> available_space,
		Axis axis);

	float MinContentContribution(GridItem& item, Axis axis, SizeOptional grid_area_size, SizeOptional available_space);
	float MinContentContributionCached(GridItem& item, Axis axis, SizeOptional grid_area_size, SizeOptional available_space);
	float MaxContentContribution(GridItem& item, Axis axis, SizeOptional grid_area_size, SizeOptional available_space);
	float MaxContentContributionCached(GridItem& item, Axis axis, SizeOptional grid_area_size, SizeOptional available_space);
	/// The minimum contribution (CSS Grid 2, 6.6: the automatic minimum size of grid items).
	float MinimumContributionCached(GridItem& item, Axis axis, const GridTrackVec& axis_tracks, SizeOptional grid_area_size,
		SizeOptional inner_node_size);

	/// The sum of the fixed maximum track sizing functions (and 'fit-content()' limits) of the tracks an item spans, if
	/// they all have one.
	OptionalFloat SpannedTrackLimit(const GridItem& item, Axis axis, const GridTrackVec& axis_tracks, OptionalFloat axis_parent_size);
	/// As SpannedTrackLimit, without the 'fit-content()' limits.
	OptionalFloat SpannedFixedTrackLimit(const GridItem& item, Axis axis, const GridTrackVec& axis_tracks, OptionalFloat axis_parent_size);

	/// The first baseline of an item laid out under a min-content width constraint, from its border-box top, or false.
	bool MeasureItemBaseline(GridItem& item, SizeOptional inner_node_size, float& baseline, float& height);

} // namespace GridLayout
} // namespace Rml
