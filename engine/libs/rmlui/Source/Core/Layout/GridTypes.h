/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports types/coordinates.rs, types/grid_track_counts.rs, types/grid_track.rs and the struct of
    types/grid_item.rs.
*/

#pragma once

#include "../../../Include/RmlUi/Core/StyleTypes.h"
#include "../../../Include/RmlUi/Core/Types.h"
#include <cfloat>
#include <cmath>
#include <optional>

namespace Rml {

class Calculation;

namespace GridLayout {

	/// The maximum number of tracks on each side of the explicit grid's start line, and the longest span (CSS Grid 2,
	/// 'Clamping Overlarge Grids').
	constexpr int MaxGridTracks = 10000;
	constexpr int MinOzLine = -MaxGridTracks;
	constexpr int MaxOzLine = MaxGridTracks;

	constexpr float Infinity = std::numeric_limits<float>::infinity();

	using OptionalFloat = std::optional<float>;

	inline OptionalFloat MaybeMin(OptionalFloat a, OptionalFloat b)
	{
		if (a && b)
			return std::min(*a, *b);
		return a;
	}
	inline OptionalFloat MaybeMax(OptionalFloat a, OptionalFloat b)
	{
		if (a && b)
			return std::max(*a, *b);
		return a;
	}
	inline OptionalFloat MaybeAdd(OptionalFloat a, OptionalFloat b)
	{
		if (a && b)
			return *a + *b;
		return a;
	}
	inline OptionalFloat MaybeSub(OptionalFloat a, OptionalFloat b)
	{
		if (a && b)
			return *a - *b;
		return a;
	}
	// Clamps a value by an optional minimum and maximum: CSS's rule, the minimum wins over the maximum.
	inline OptionalFloat MaybeClamp(OptionalFloat value, OptionalFloat min, OptionalFloat max)
	{
		if (!value)
			return value;
		float result = *value;
		if (max)
			result = std::min(result, *max);
		if (min)
			result = std::max(result, *min);
		return result;
	}
	/// Applies an aspect ratio (width / height, 0 for none) to a size of which one axis is known (Taffy:
	/// maybe_apply_aspect_ratio).
	inline void MaybeApplyAspectRatio(OptionalFloat& width, OptionalFloat& height, float aspect_ratio)
	{
		if (aspect_ratio <= 0.f)
			return;
		if (width && !height)
			height = *width / aspect_ratio;
		else if (!width && height)
			width = *height * aspect_ratio;
	}

	inline float MaybeClamp(float value, OptionalFloat min, OptionalFloat max)
	{
		if (max)
			value = std::min(value, *max);
		if (min)
			value = std::max(value, *min);
		return value;
	}
	inline float MaybeMin(float a, OptionalFloat b)
	{
		return b ? std::min(a, *b) : a;
	}
	inline float MaybeMax(float a, OptionalFloat b)
	{
		return b ? std::max(a, *b) : a;
	}

	/// The physical axes (writing mode horizontal-tb: columns are the inline axis, rows the block axis).
	enum class Axis : uint8_t { Horizontal, Vertical };
	inline Axis OtherAxis(Axis axis)
	{
		return axis == Axis::Horizontal ? Axis::Vertical : Axis::Horizontal;
	}

	template <typename T>
	struct Line {
		T start, end;
	};

	template <typename T>
	struct AxisPair {
		T width, height;
		T& Get(Axis axis) { return axis == Axis::Horizontal ? width : height; }
		const T& Get(Axis axis) const { return axis == Axis::Horizontal ? width : height; }
		void Set(Axis axis, const T& value) { (axis == Axis::Horizontal ? width : height) = value; }
		bool operator==(const AxisPair& other) const { return width == other.width && height == other.height; }
		bool operator!=(const AxisPair& other) const { return !(*this == other); }
	};
	using SizeOptional = AxisPair<OptionalFloat>;

	/// The space a box is sized in: a definite size, or a min-content or max-content constraint.
	struct AvailableSpace {
		enum class Type : uint8_t { Definite, MinContent, MaxContent };
		Type type = Type::MaxContent;
		float value = 0.f;

		static AvailableSpace Definite(float value) { return AvailableSpace{Type::Definite, value}; }
		static AvailableSpace MinContent() { return AvailableSpace{Type::MinContent, 0.f}; }
		static AvailableSpace MaxContent() { return AvailableSpace{Type::MaxContent, 0.f}; }

		bool IsDefinite() const { return type == Type::Definite; }
		OptionalFloat ToOption() const { return IsDefinite() ? OptionalFloat(value) : std::nullopt; }
		float ComputeFreeSpace(float used_space) const
		{
			switch (type)
			{
			case Type::MaxContent: return Infinity;
			case Type::MinContent: return 0.f;
			case Type::Definite: return value - used_space;
			}
			return 0.f;
		}
		bool operator==(const AvailableSpace& other) const { return type == other.type && (type != Type::Definite || value == other.value); }
	};

	/// A grid line placement: 'auto', a line (CSS line numbers: 1-based from the start, negative from the end; or
	/// origin-zero lines: 0 at the start of the explicit grid), or a span.
	struct Placement {
		enum class Type : uint8_t { Auto, Line, Span };
		Type type = Type::Auto;
		int value = 0; // Line: the line; Span: the count

		static Placement Auto() { return Placement{Type::Auto, 0}; }
		static Placement LineAt(int line) { return Placement{Type::Line, line}; }
		static Placement Span(int span) { return Placement{Type::Span, span}; }
	};
	using PlacementLine = Line<Placement>;

	/// The number of tracks of an axis: before the explicit grid, in it, and after it.
	struct TrackCounts {
		int negative_implicit = 0;
		int explicit_count = 0;
		int positive_implicit = 0;

		int Len() const { return negative_implicit + explicit_count + positive_implicit; }
		int ImplicitStartLine() const { return -negative_implicit; }
		int ImplicitEndLine() const { return explicit_count + positive_implicit; }
		/// The origin-zero line to the index (in the cell occupancy matrix) of the track that follows it.
		int OzLineToNextTrack(int oz_line) const { return oz_line + negative_implicit; }
		/// The track index (in the cell occupancy matrix) to the origin-zero line before it.
		int TrackToPrevOzLine(int index) const { return index - negative_implicit; }
	};

	/// A CSS line number (1-based, negative from the end, not 0) to an origin-zero line, clamped to the limited grid.
	inline int GridLineToOriginZero(int grid_line, int explicit_track_count)
	{
		const int explicit_line_count = explicit_track_count + 1;
		const int oz_line = grid_line > 0 ? grid_line - 1 : grid_line + explicit_line_count;
		return std::max(MinOzLine, std::min(oz_line, MaxOzLine));
	}

	/// The index of an origin-zero line in a track vector ([gutter, track, gutter, ..., gutter]), or -1 when the line is
	/// outside the grid.
	inline int TryIntoTrackVecIndex(int oz_line, const TrackCounts& counts)
	{
		if (oz_line < -counts.negative_implicit || oz_line > counts.explicit_count + counts.positive_implicit)
			return -1;
		return 2 * (oz_line + counts.negative_implicit);
	}

	inline int ImpliedNegativeImplicitTracks(int oz_line)
	{
		return oz_line < 0 ? -oz_line : 0;
	}
	inline int ImpliedPositiveImplicitTracks(int oz_line, int explicit_track_count)
	{
		return oz_line > explicit_track_count ? oz_line - explicit_track_count : 0;
	}
	inline int Span(const Line<int>& line)
	{
		return std::max(line.end - line.start, 0);
	}

	/// A resolved <length-percentage> of a track sizing function: pixels, a fraction of the axis' size, or a calc()
	/// expression resolved with the grid container's context.
	struct TrackLength {
		enum class Type : uint8_t { Px, Percent, Calc };
		Type type = Type::Px;
		float value = 0.f; // Px: pixels; Percent: the fraction (0.5 for 50%)
		const Calculation* calculation = nullptr;
		Element* element = nullptr; // resolves the calculation
		bool calculation_uses_percentage = false;

		bool UsesPercentage() const { return type == Type::Percent || (type == Type::Calc && calculation_uses_percentage); }
		/// The size in pixels, nullopt when it needs a percentage basis that is indefinite.
		OptionalFloat Resolve(OptionalFloat basis) const;
		bool operator==(const TrackLength& other) const
		{
			return type == other.type && value == other.value && calculation == other.calculation;
		}
	};

	/// A minimum or maximum track sizing function (a minimum is never 'fit-content()' or flexible).
	struct TrackSizingFunction {
		enum class Type : uint8_t { Length, Auto, MinContent, MaxContent, FitContent, Fr };
		Type type = Type::Auto;
		TrackLength length; // Length: the length; FitContent: the limit
		float fr = 0.f;

		static TrackSizingFunction Zero()
		{
			TrackSizingFunction result;
			result.type = Type::Length;
			return result;
		}
		static TrackSizingFunction AutoFunction() { return TrackSizingFunction(); }

		bool IsAuto() const { return type == Type::Auto; }
		bool IsFr() const { return type == Type::Fr; }
		bool IsMinContent() const { return type == Type::MinContent; }
		bool IsMaxContent() const { return type == Type::MaxContent; }
		bool IsFitContent() const { return type == Type::FitContent; }
		bool IsMaxOrFitContent() const { return type == Type::MaxContent || type == Type::FitContent; }
		bool IsMinOrMaxContent() const { return type == Type::MinContent || type == Type::MaxContent; }
		/// 'auto', 'max-content' and 'fit-content()' (CSS Grid 2, 12.3: treated as max-content except where noted).
		bool IsMaxContentAlike() const { return type == Type::Auto || type == Type::MaxContent || type == Type::FitContent; }
		bool IsIntrinsic() const { return type == Type::Auto || type == Type::MinContent || type == Type::MaxContent || type == Type::FitContent; }
		bool IsLengthOrPercentage() const { return type == Type::Length; }
		bool UsesPercentage() const { return (type == Type::Length || type == Type::FitContent) && length.UsesPercentage(); }
		/// A minimum that behaves as 'auto': 'auto', or a percentage of an indefinite size.
		bool BehavesAsAuto(OptionalFloat parent_size) const { return IsAuto() || (!parent_size && UsesPercentage()); }

		bool HasDefiniteValue(OptionalFloat parent_size) const { return type == Type::Length && (parent_size || !length.UsesPercentage()); }
		OptionalFloat DefiniteValue(OptionalFloat parent_size) const
		{
			if (type != Type::Length)
				return std::nullopt;
			return length.Resolve(parent_size);
		}
		OptionalFloat FitContentLimit(OptionalFloat parent_size) const
		{
			if (type != Type::FitContent)
				return std::nullopt;
			return length.Resolve(parent_size);
		}
		/// The definite value, or the resolved 'fit-content()' limit.
		OptionalFloat DefiniteLimit(OptionalFloat parent_size) const
		{
			if (type == Type::FitContent)
				return length.Resolve(parent_size);
			return DefiniteValue(parent_size);
		}
		float FlexFactor() const { return type == Type::Fr ? fr : 0.f; }
	};

	enum class TrackKind : uint8_t { Track, Gutter };

	/// A track (row or column) or a gutter while sizing (CSS Grid 2, 12.4): gutters are sized like fixed tracks.
	struct GridTrack {
		TrackKind kind = TrackKind::Track;
		bool is_collapsed = false;
		TrackSizingFunction min_track_sizing_function;
		TrackSizingFunction max_track_sizing_function;
		float offset = 0.f;
		float base_size = 0.f;
		float growth_limit = 0.f; // may be infinite
		float content_alignment_adjustment = 0.f;
		float item_incurred_increase = 0.f;
		float base_size_planned_increase = 0.f;
		float growth_limit_planned_increase = 0.f;
		bool infinitely_growable = false;
		float fit_content_limit = Infinity;

		static GridTrack Track(const TrackSizingFunction& min, const TrackSizingFunction& max)
		{
			GridTrack track;
			track.min_track_sizing_function = min;
			track.max_track_sizing_function = max;
			return track;
		}
		static GridTrack Gutter(const TrackSizingFunction& size)
		{
			GridTrack track;
			track.kind = TrackKind::Gutter;
			track.min_track_sizing_function = size;
			track.max_track_sizing_function = size;
			return track;
		}

		/// Collapses the track (an empty 'auto-fit' track, or the outer gutters): fixed at zero.
		void Collapse()
		{
			is_collapsed = true;
			min_track_sizing_function = TrackSizingFunction::Zero();
			max_track_sizing_function = TrackSizingFunction::Zero();
			fit_content_limit = Infinity;
		}
		bool IsFlexible() const { return max_track_sizing_function.IsFr(); }
		bool UsesPercentage() const { return min_track_sizing_function.UsesPercentage() || max_track_sizing_function.UsesPercentage(); }
		bool HasIntrinsicSizingFunction() const { return min_track_sizing_function.IsIntrinsic() || max_track_sizing_function.IsIntrinsic(); }
		void ResolveFitContentLimit(OptionalFloat axis_inner_node_size)
		{
			const OptionalFloat limit = max_track_sizing_function.FitContentLimit(axis_inner_node_size);
			fit_content_limit = limit ? *limit : Infinity;
		}
		float FitContentLimitedGrowthLimit() const { return std::min(growth_limit, fit_content_limit); }
		float FlexFactor() const { return max_track_sizing_function.FlexFactor(); }
	};
	using GridTrackVec = Vector<GridTrack>;

	/// The self alignment of an item, CSS Box Alignment 3 (writing mode horizontal-tb, left-to-right).
	enum class SelfAlign : uint8_t { Start, End, Center, Stretch, Baseline, Normal };
	struct SelfAlignment {
		SelfAlign keyword = SelfAlign::Normal;
		bool safe = false;
	};

	/// The content distribution of the tracks ('justify-content', 'align-content'; 'normal' resolved to 'stretch').
	enum class ContentAlign : uint8_t { Start, End, Center, Stretch, SpaceBetween, SpaceAround, SpaceEvenly };
	struct ContentAlignment {
		ContentAlign keyword = ContentAlign::Stretch;
		bool safe = false;
		bool operator==(const ContentAlignment& other) const { return keyword == other.keyword && safe == other.safe; }
	};

	/// A grid item: an in-flow child of the grid container with its placement, the style its sizing reads, and the
	/// caches of one track sizing pass (types/grid_item.rs).
	struct GridItem {
		Element* element = nullptr;
		int source_order = 0;

		// The placement in origin-zero lines, then as indexes into the track vectors.
		Line<int> row = {0, 0}, column = {0, 0};
		Line<int> row_indexes = {0, 0}, column_indexes = {0, 0};

		// Style.
		bool is_compressible_replaced = false;
		bool is_text = false; // an anonymous item: a run of text directly in the container
		AxisPair<bool> scroll_container = {false, false};
		Style::BoxSizing box_sizing = Style::BoxSizing::ContentBox;
		Style::LengthPercentageAuto size_width, size_height;
		// The sizing keywords of 'width' and 'height' (min-content, max-content, fit-content, stretch), which compute to 'auto'.
		Style::SizingKeyword width_keyword = Style::SizingKeyword::None, height_keyword = Style::SizingKeyword::None;
		float aspect_ratio = 0.f; // The preferred aspect ratio (width / height) of the box-sizing box, 0 for none.
		Style::LengthPercentageAuto min_width, min_height;
		Style::LengthPercentage max_width, max_height;
		Style::LengthPercentage padding[4]; // top, right, bottom, left
		float border[4] = {};
		Style::LengthPercentageAuto margin[4];
		SelfAlignment align_self, justify_self;
		float baseline = 0.f;
		bool has_baseline = false;
		float baseline_shim = 0.f;

		bool crosses_flexible_row = false, crosses_flexible_column = false;
		bool crosses_intrinsic_row = false, crosses_intrinsic_column = false;

		// Caches of one run of the track sizing algorithm.
		std::optional<SizeOptional> grid_area_size_cache;
		std::optional<SizeOptional> known_dimensions_cache_area;
		SizeOptional known_dimensions_cache = {};
		SizeOptional min_content_contribution_cache = {};
		SizeOptional minimum_contribution_cache = {};
		SizeOptional max_content_contribution_cache = {};

		// The final position, for the container's baseline.
		float y_position = 0.f;
		float height = 0.f;

		const Line<int>& Placement(Axis axis) const { return axis == Axis::Vertical ? row : column; }
		const Line<int>& PlacementIndexes(Axis axis) const { return axis == Axis::Vertical ? row_indexes : column_indexes; }
		int SpanIn(Axis axis) const { return Span(Placement(axis)); }
		/// The track vector range of the tracks and gutters the item spans (without its bounding lines): [begin, end).
		int TrackRangeBegin(Axis axis) const { return PlacementIndexes(axis).start + 1; }
		int TrackRangeEnd(Axis axis) const { return PlacementIndexes(axis).end; }
		bool CrossesFlexibleTrack(Axis axis) const { return axis == Axis::Horizontal ? crosses_flexible_column : crosses_flexible_row; }
		bool CrossesIntrinsicTrack(Axis axis) const { return axis == Axis::Horizontal ? crosses_intrinsic_column : crosses_intrinsic_row; }
		bool HasAutoBlockMargin() const
		{
			return margin[0].type == Style::LengthPercentageAuto::Auto || margin[2].type == Style::LengthPercentageAuto::Auto;
		}
		bool HasCyclicBlockSizeDependency() const;
		/// 'align-self: baseline' without an auto block margin (CSS Box Alignment 3, 9.2).
		bool ParticipatesInBaselineAlignment() const
		{
			return align_self.keyword == SelfAlign::Baseline && !HasAutoBlockMargin() && !HasCyclicBlockSizeDependency();
		}
	};

} // namespace GridLayout
} // namespace Rml
