/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports the contribution functions of types/grid_item.rs, and measures the items with RmlUi's layout.
*/

#include "GridItemMeasure.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "ContainerBox.h"
#include "FormattingContext.h"
#include "LayoutBox.h"
#include "LayoutDetails.h"
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "../Calculation.h"
	#include "../CalculationResolver.h"
#endif

namespace Rml {
namespace GridLayout {

#ifdef RMLUI_MATH_EXPRESSIONS
	static OptionalFloat ResolveCalculation(const CalculationPtr& calculation, OptionalFloat basis)
	{
		if (!calculation)
			return std::nullopt;
		if (!basis && Any(calculation->GetDependencyMask() & Unit::PERCENT))
			return std::nullopt;
		float result = 0.f;
		if (!ResolveUsedCalculation(*calculation, basis ? *basis : 0.f, result))
			return std::nullopt;
		return result;
	}
#endif

	OptionalFloat MaybeResolve(const Style::LengthPercentageAuto& value, OptionalFloat basis)
	{
		switch (value.type)
		{
		case Style::LengthPercentageAuto::Auto: return std::nullopt;
		case Style::LengthPercentageAuto::Length: return value.value;
		case Style::LengthPercentageAuto::Percentage:
			if (!basis)
				return std::nullopt;
			return value.value * 0.01f * *basis;
#ifdef RMLUI_MATH_EXPRESSIONS
		case Style::LengthPercentageAuto::Calculation: return ResolveCalculation(value.calculation, basis);
#endif
		}
		return std::nullopt;
	}

	OptionalFloat MaybeResolveMax(const Style::LengthPercentage& value, OptionalFloat basis)
	{
		switch (value.type)
		{
		case Style::LengthPercentage::Length:
			if (value.value >= FLT_MAX)
				return std::nullopt;
			return value.value;
		case Style::LengthPercentage::Percentage:
			if (!basis)
				return std::nullopt;
			return value.value * 0.01f * *basis;
#ifdef RMLUI_MATH_EXPRESSIONS
		case Style::LengthPercentage::Calculation: return ResolveCalculation(value.calculation, basis);
#endif
		}
		return std::nullopt;
	}

	float ResolveOrZero(const Style::LengthPercentage& value, OptionalFloat basis)
	{
		switch (value.type)
		{
		case Style::LengthPercentage::Length: return value.value;
		case Style::LengthPercentage::Percentage: return basis ? value.value * 0.01f * *basis : 0.f;
#ifdef RMLUI_MATH_EXPRESSIONS
		case Style::LengthPercentage::Calculation:
		{
			const OptionalFloat result = ResolveCalculation(value.calculation, basis ? basis : OptionalFloat(0.f));
			return result ? *result : 0.f;
		}
#endif
		}
		return 0.f;
	}

	static float ResolveMarginOrZero(const Style::LengthPercentageAuto& value, OptionalFloat basis)
	{
		const OptionalFloat result = MaybeResolve(value, basis ? basis : OptionalFloat(0.f));
		return result ? *result : 0.f;
	}

	static SelfAlignment ToSelfAlignment(Style::AlignSelf value, Style::OverflowAlignment overflow)
	{
		using Style::AlignSelf;
		SelfAlignment result;
		result.safe = (overflow == Style::OverflowAlignment::Safe);
		switch (value)
		{
		case AlignSelf::FlexStart:
		case AlignSelf::Start:
		case AlignSelf::SelfStart: result.keyword = SelfAlign::Start; break;
		case AlignSelf::FlexEnd:
		case AlignSelf::End:
		case AlignSelf::SelfEnd: result.keyword = SelfAlign::End; break;
		case AlignSelf::Center: result.keyword = SelfAlign::Center; break;
		case AlignSelf::Baseline: result.keyword = SelfAlign::Baseline; break;
		// Last-baseline alignment is not done: its fallback, 'safe end'.
		case AlignSelf::LastBaseline:
			result.keyword = SelfAlign::End;
			result.safe = true;
			break;
		case AlignSelf::Stretch: result.keyword = SelfAlign::Stretch; break;
		case AlignSelf::Auto:
		case AlignSelf::Normal: result.keyword = SelfAlign::Normal; break;
		}
		return result;
	}

	static SelfAlignment ToSelfAlignment(Style::JustifySelf value, Style::OverflowAlignment overflow)
	{
		using Style::JustifySelf;
		SelfAlignment result;
		result.safe = (overflow == Style::OverflowAlignment::Safe);
		switch (value)
		{
		case JustifySelf::FlexStart:
		case JustifySelf::Start:
		case JustifySelf::SelfStart:
		case JustifySelf::Left: result.keyword = SelfAlign::Start; break;
		case JustifySelf::FlexEnd:
		case JustifySelf::End:
		case JustifySelf::SelfEnd:
		case JustifySelf::Right: result.keyword = SelfAlign::End; break;
		case JustifySelf::Center: result.keyword = SelfAlign::Center; break;
		case JustifySelf::Baseline: result.keyword = SelfAlign::Baseline; break;
		case JustifySelf::LastBaseline:
			result.keyword = SelfAlign::End;
			result.safe = true;
			break;
		case JustifySelf::Stretch: result.keyword = SelfAlign::Stretch; break;
		case JustifySelf::Auto:
		case JustifySelf::Normal: result.keyword = SelfAlign::Normal; break;
		}
		return result;
	}

	void ReadGridItemStyle(GridItem& item, Style::AlignItems container_align_items, Style::JustifyItems container_justify_items)
	{
		const ComputedValues& computed = item.element->GetComputedValues();
		item.is_text = (rmlui_dynamic_cast<ElementText*>(item.element) != nullptr);
		item.is_compressible_replaced = false;
		item.scroll_container = {computed.overflow_x() != Style::Overflow::Visible, computed.overflow_y() != Style::Overflow::Visible};
		item.box_sizing = computed.box_sizing();
		item.size_width = computed.width();
		item.size_height = computed.height();
		item.width_keyword = computed.width_keyword();
		item.height_keyword = computed.height_keyword();
		item.aspect_ratio = (item.element->IsReplaced() ? 0.f : computed.aspect_ratio());
		item.min_width = computed.min_width();
		item.min_height = computed.min_height();
		item.max_width = computed.max_width();
		item.max_height = computed.max_height();
		item.padding[0] = computed.padding_top();
		item.padding[1] = computed.padding_right();
		item.padding[2] = computed.padding_bottom();
		item.padding[3] = computed.padding_left();
		item.border[0] = computed.border_top_width();
		item.border[1] = computed.border_right_width();
		item.border[2] = computed.border_bottom_width();
		item.border[3] = computed.border_left_width();
		item.margin[0] = computed.margin_top();
		item.margin[1] = computed.margin_right();
		item.margin[2] = computed.margin_bottom();
		item.margin[3] = computed.margin_left();
		if (item.is_text)
		{
			// An anonymous item: its box has the initial values, the computed values of a text node are its parent's.
			item.size_width = item.size_height = Style::LengthPercentageAuto(Style::LengthPercentageAuto::Auto);
			item.width_keyword = item.height_keyword = Style::SizingKeyword::None;
			item.aspect_ratio = 0.f;
			item.min_width = item.min_height = Style::LengthPercentageAuto(Style::LengthPercentageAuto::Auto);
			item.max_width = item.max_height = Style::LengthPercentage(Style::LengthPercentage::Length, FLT_MAX);
			for (int i = 0; i < 4; i++)
			{
				item.padding[i] = Style::LengthPercentage();
				item.border[i] = 0.f;
				item.margin[i] = Style::LengthPercentageAuto();
			}
			item.box_sizing = Style::BoxSizing::ContentBox;
			item.scroll_container = {false, false};
		}

		// 'auto' self alignment takes the container's '*-items' ('legacy' behaves as 'normal').
		const Style::AlignSelf align_self = (item.is_text ? Style::AlignSelf::Auto : computed.align_self());
		if (align_self == Style::AlignSelf::Auto)
		{
			const Style::AlignSelf from_items = static_cast<Style::AlignSelf>(static_cast<int>(container_align_items) + 1);
			const ComputedValues& container = item.element->GetParentNode()->GetComputedValues();
			item.align_self = ToSelfAlignment(from_items, container.align_items_overflow());
		}
		else
			item.align_self = ToSelfAlignment(align_self, computed.align_self_overflow());

		const Style::JustifySelf justify_self = (item.is_text ? Style::JustifySelf::Auto : computed.justify_self());
		if (justify_self == Style::JustifySelf::Auto)
		{
			const Style::JustifySelf from_items = (container_justify_items == Style::JustifyItems::Legacy
					? Style::JustifySelf::Normal
					: static_cast<Style::JustifySelf>(static_cast<int>(container_justify_items) + 1));
			const ComputedValues& container = item.element->GetParentNode()->GetComputedValues();
			item.justify_self = ToSelfAlignment(from_items, container.justify_items_overflow());
		}
		else
			item.justify_self = ToSelfAlignment(justify_self, computed.justify_self_overflow());
	}

	AxisPair<float> PaddingBorderSize(const GridItem& item, OptionalFloat width_basis)
	{
		return AxisPair<float>{
			ResolveOrZero(item.padding[1], width_basis) + ResolveOrZero(item.padding[3], width_basis) + item.border[1] + item.border[3],
			ResolveOrZero(item.padding[0], width_basis) + ResolveOrZero(item.padding[2], width_basis) + item.border[0] + item.border[2],
		};
	}

	AxisPair<float> MarginsAxisSumsWithBaselineShims(const GridItem& item, OptionalFloat inner_node_width)
	{
		return AxisPair<float>{
			ResolveMarginOrZero(item.margin[3], 0.f) + ResolveMarginOrZero(item.margin[1], 0.f),
			ResolveMarginOrZero(item.margin[0], inner_node_width) + item.baseline_shim + ResolveMarginOrZero(item.margin[2], inner_node_width),
		};
	}

	OptionalFloat TrackSizeEstimateOf(const GridTrack& track, OptionalFloat parent_size, TrackSizeEstimate estimate)
	{
		if (estimate == TrackSizeEstimate::BaseSize)
			return track.base_size;
		return track.max_track_sizing_function.DefiniteValue(parent_size);
	}

	SizeOptional GridAreaSize(const GridItem& item, Axis axis, const GridTrackVec& axis_tracks, const GridTrackVec& other_axis_tracks,
		SizeOptional inner_node_size, TrackSizeEstimate estimate)
	{
		SizeOptional size = {};

		// In the axis being sized, the grid area is definite only when every spanned track is fixed (CSS Grid 2, 12.1).
		{
			OptionalFloat sum = 0.f;
			for (int i = item.TrackRangeBegin(axis); i < item.TrackRangeEnd(axis); i++)
			{
				const GridTrack& track = axis_tracks[i];
				const OptionalFloat min_size = track.min_track_sizing_function.DefiniteValue(inner_node_size.Get(axis));
				const OptionalFloat max_size = track.max_track_sizing_function.DefiniteValue(inner_node_size.Get(axis));
				if (!min_size || !max_size || *min_size != *max_size)
				{
					sum = std::nullopt;
					break;
				}
				*sum += track.base_size;
			}
			size.Set(axis, sum);
		}

		const Axis other = OtherAxis(axis);
		{
			OptionalFloat sum = 0.f;
			for (int i = item.TrackRangeBegin(other); i < item.TrackRangeEnd(other); i++)
			{
				const GridTrack& track = other_axis_tracks[i];
				const OptionalFloat track_size = TrackSizeEstimateOf(track, inner_node_size.Get(other), estimate);
				if (!track_size)
				{
					sum = std::nullopt;
					break;
				}
				*sum += *track_size + track.content_alignment_adjustment;
			}
			size.Set(other, sum);
		}
		return size;
	}

	SizeOptional GridAreaSizeCached(GridItem& item, Axis axis, const GridTrackVec& axis_tracks, const GridTrackVec& other_axis_tracks,
		SizeOptional inner_node_size, TrackSizeEstimate estimate)
	{
		if (!item.grid_area_size_cache)
			item.grid_area_size_cache = GridAreaSize(item, axis, axis_tracks, other_axis_tracks, inner_node_size, estimate);
		return *item.grid_area_size_cache;
	}

	// The border-box minimum, preferred and maximum sizes of an item in a grid area (Taffy: known_dimensions).
	static SizeOptional KnownDimensions(const GridItem& item, SizeOptional grid_area_size)
	{
		const AxisPair<float> margins = MarginsAxisSumsWithBaselineShims(item, grid_area_size.width);
		// Percentage padding and borders resolve against the grid area's width when it is definite.
		const AxisPair<float> padding_border = PaddingBorderSize(item, grid_area_size.width);
		const AxisPair<float> box_sizing_adjustment =
			(item.box_sizing == Style::BoxSizing::ContentBox ? padding_border : AxisPair<float>{0.f, 0.f});

		// The preferred aspect ratio is that of the box-sizing box: applied before the padding and border are added.
		OptionalFloat inherent_width = MaybeResolve(item.size_width, grid_area_size.width);
		OptionalFloat inherent_height = MaybeResolve(item.size_height, grid_area_size.height);
		MaybeApplyAspectRatio(inherent_width, inherent_height, item.aspect_ratio);
		inherent_width = MaybeAdd(inherent_width, box_sizing_adjustment.width);
		inherent_height = MaybeAdd(inherent_height, box_sizing_adjustment.height);
		// The size is floored by the padding and border.
		OptionalFloat min_width = MaybeResolve(item.min_width, grid_area_size.width);
		OptionalFloat min_height = MaybeResolve(item.min_height, grid_area_size.height);
		MaybeApplyAspectRatio(min_width, min_height, item.aspect_ratio);
		min_width = MaybeAdd(min_width, box_sizing_adjustment.width);
		min_height = MaybeAdd(min_height, box_sizing_adjustment.height);
		min_width = min_width ? std::max(*min_width, padding_border.width) : padding_border.width;
		min_height = min_height ? std::max(*min_height, padding_border.height) : padding_border.height;
		OptionalFloat max_width = MaybeResolveMax(item.max_width, grid_area_size.width);
		OptionalFloat max_height = MaybeResolveMax(item.max_height, grid_area_size.height);
		MaybeApplyAspectRatio(max_width, max_height, item.aspect_ratio);
		max_width = MaybeAdd(max_width, box_sizing_adjustment.width);
		max_height = MaybeAdd(max_height, box_sizing_adjustment.height);

		auto AreaMinusMargins = [&](Axis axis) -> OptionalFloat {
			const OptionalFloat area = grid_area_size.Get(axis);
			if (!area)
				return std::nullopt;
			return std::max(*area - margins.Get(axis), 0.f);
		};

		// An auto size is stretched by 'stretch' or 'normal' self alignment without auto margins (always 'normal' here, unlike
		// at the final alignment).
		// A content sizing keyword (min-content, max-content, fit-content) is not stretched: the size is measured. 'stretch'
		// fills the area whatever the alignment.
		using Style::SizingKeyword;
		const bool stretch_width = (item.width_keyword == SizingKeyword::Stretch ||
			(item.width_keyword == SizingKeyword::None &&
				(item.justify_self.keyword == SelfAlign::Stretch || item.justify_self.keyword == SelfAlign::Normal)));
		const bool stretch_height = (item.height_keyword == SizingKeyword::Stretch ||
			(item.height_keyword == SizingKeyword::None &&
				(item.align_self.keyword == SelfAlign::Stretch || item.align_self.keyword == SelfAlign::Normal)));
		OptionalFloat width = inherent_width;
		if (!width && item.margin[3].type != Style::LengthPercentageAuto::Auto && item.margin[1].type != Style::LengthPercentageAuto::Auto &&
			stretch_width)
			width = AreaMinusMargins(Axis::Horizontal);
		// The aspect ratio again after each stretch (as Taffy does).
		OptionalFloat height = inherent_height;
		MaybeApplyAspectRatio(width, height, item.aspect_ratio);
		if (!height && item.margin[0].type != Style::LengthPercentageAuto::Auto && item.margin[2].type != Style::LengthPercentageAuto::Auto &&
			stretch_height)
			height = AreaMinusMargins(Axis::Vertical);
		MaybeApplyAspectRatio(width, height, item.aspect_ratio);

		return SizeOptional{MaybeClamp(width, min_width, max_width), MaybeClamp(height, min_height, max_height)};
	}

	static SizeOptional KnownDimensionsCached(GridItem& item, SizeOptional grid_area_size)
	{
		if (item.known_dimensions_cache_area && *item.known_dimensions_cache_area == grid_area_size)
			return item.known_dimensions_cache;
		item.known_dimensions_cache = KnownDimensions(item, grid_area_size);
		item.known_dimensions_cache_area = grid_area_size;
		return item.known_dimensions_cache;
	}

	// The border-box width of an item for an available width: min-content, max-content or fit-content, clamped by its
	// minimum and maximum widths.
	static float MeasureWidth(GridItem& item, Vector2f containing_block, AvailableSpace available_width)
	{
		Element* element = item.element;
		// A content sizing keyword of 'width' is the constraint the item is measured under.
		switch (item.width_keyword)
		{
		case Style::SizingKeyword::MinContent: available_width = AvailableSpace::MinContent(); break;
		case Style::SizingKeyword::MaxContent: available_width = AvailableSpace::MaxContent(); break;
		case Style::SizingKeyword::FitContentLength:
		{
			const Style::LengthPercentage limit = element->GetComputedValues().fit_content_width();
			if (limit.type == Style::LengthPercentage::Length || containing_block.x >= 0.f)
				available_width = AvailableSpace::Definite(ResolveValue(limit, Math::Max(containing_block.x, 0.f)));
		}
		break;
		default: break;
		}
		float width = 0.f;
		switch (available_width.type)
		{
		case AvailableSpace::Type::MinContent: width = LayoutDetails::GetMinContentWidth(element, containing_block); break;
		case AvailableSpace::Type::MaxContent: width = LayoutDetails::GetMaxContentWidth(element, containing_block); break;
		case AvailableSpace::Type::Definite:
		{
			const float max_content = LayoutDetails::GetMaxContentWidth(element, containing_block);
			if (max_content <= available_width.value)
				width = max_content;
			else
				width = std::max(LayoutDetails::GetMinContentWidth(element, containing_block), available_width.value);
			break;
		}
		}

		const OptionalFloat basis = (containing_block.x >= 0.f ? OptionalFloat(containing_block.x) : std::nullopt);
		const AxisPair<float> padding_border = PaddingBorderSize(item, basis);
		const float adjustment = (item.box_sizing == Style::BoxSizing::ContentBox ? padding_border.width : 0.f);
		const OptionalFloat min_width = MaybeAdd(MaybeResolve(item.min_width, basis), adjustment);
		const OptionalFloat max_width = MaybeAdd(MaybeResolveMax(item.max_width, basis), adjustment);
		return std::max(MaybeClamp(width, min_width, max_width), padding_border.width);
	}

	float MeasureChildSize(GridItem& item, SizeOptional known_dimensions, SizeOptional parent_size, AxisPair<AvailableSpace> available_space,
		Axis axis)
	{
		const Vector2f containing_block(parent_size.width ? *parent_size.width : -1.f, parent_size.height ? *parent_size.height : -1.f);
		// A size from the other one through the preferred aspect ratio (of the box-sizing box).
		const OptionalFloat basis = (containing_block.x >= 0.f ? OptionalFloat(containing_block.x) : std::nullopt);
		const AxisPair<float> padding_border = PaddingBorderSize(item, basis);
		const AxisPair<float> ratio_edges = (item.box_sizing == Style::BoxSizing::ContentBox ? padding_border : AxisPair<float>{0.f, 0.f});
		if (axis == Axis::Horizontal)
		{
			if (known_dimensions.width)
				return *known_dimensions.width;
			if (known_dimensions.height && item.aspect_ratio > 0.f)
				return std::max(*known_dimensions.height - ratio_edges.height, 0.f) * item.aspect_ratio + ratio_edges.width;
			return MeasureWidth(item, containing_block, available_space.width);
		}
		if (known_dimensions.height)
			return *known_dimensions.height;
		const float width = known_dimensions.width ? *known_dimensions.width : MeasureWidth(item, containing_block, available_space.width);
		if (item.aspect_ratio > 0.f)
			return std::max(width - ratio_edges.width, 0.f) / item.aspect_ratio + ratio_edges.height;
		return LayoutDetails::GetContentHeightForWidth(item.element, width, containing_block);
	}

	// The available space for the item's border box: the space of its margin box less its margins (auto ones as 0).
	static SizeOptional AvailableSpaceMinusMargins(const GridItem& item, SizeOptional grid_area_size, SizeOptional available_space)
	{
		if (!available_space.width && !available_space.height)
			return available_space;
		const float margin_left = ResolveMarginOrZero(item.margin[3], grid_area_size.width);
		const float margin_right = ResolveMarginOrZero(item.margin[1], grid_area_size.width);
		const float margin_top = ResolveMarginOrZero(item.margin[0], grid_area_size.width);
		const float margin_bottom = ResolveMarginOrZero(item.margin[2], grid_area_size.width);
		SizeOptional result = available_space;
		if (result.width)
			result.width = std::max(*result.width - margin_left - margin_right, 0.f);
		if (result.height)
			result.height = std::max(*result.height - margin_top - margin_bottom - item.baseline_shim, 0.f);
		return result;
	}

	static AxisPair<AvailableSpace> ToAvailableSpace(SizeOptional size, AvailableSpace indefinite)
	{
		return AxisPair<AvailableSpace>{size.width ? AvailableSpace::Definite(*size.width) : indefinite,
			size.height ? AvailableSpace::Definite(*size.height) : indefinite};
	}

	float MinContentContribution(GridItem& item, Axis axis, SizeOptional grid_area_size, SizeOptional available_space)
	{
		const SizeOptional known_dimensions = KnownDimensionsCached(item, grid_area_size);
		if (const OptionalFloat known = known_dimensions.Get(axis))
			return *known;
		// The grid area is the containing block of the item while it is measured.
		return MeasureChildSize(item, known_dimensions, grid_area_size,
			ToAvailableSpace(AvailableSpaceMinusMargins(item, grid_area_size, available_space), AvailableSpace::MinContent()), axis);
	}

	float MinContentContributionCached(GridItem& item, Axis axis, SizeOptional grid_area_size, SizeOptional available_space)
	{
		if (const OptionalFloat cached = item.min_content_contribution_cache.Get(axis))
			return *cached;
		const float size = MinContentContribution(item, axis, grid_area_size, available_space);
		item.min_content_contribution_cache.Set(axis, size);
		return size;
	}

	float MaxContentContribution(GridItem& item, Axis axis, SizeOptional grid_area_size, SizeOptional available_space)
	{
		const SizeOptional known_dimensions = KnownDimensionsCached(item, grid_area_size);
		if (const OptionalFloat known = known_dimensions.Get(axis))
			return *known;
		return MeasureChildSize(item, known_dimensions, grid_area_size,
			ToAvailableSpace(AvailableSpaceMinusMargins(item, grid_area_size, available_space), AvailableSpace::MaxContent()), axis);
	}

	float MaxContentContributionCached(GridItem& item, Axis axis, SizeOptional grid_area_size, SizeOptional available_space)
	{
		if (const OptionalFloat cached = item.max_content_contribution_cache.Get(axis))
			return *cached;
		const float size = MaxContentContribution(item, axis, grid_area_size, available_space);
		item.max_content_contribution_cache.Set(axis, size);
		return size;
	}

	static float MinimumContribution(GridItem& item, Axis axis, const GridTrackVec& axis_tracks, SizeOptional grid_area_size,
		SizeOptional inner_node_size)
	{
		const AxisPair<float> padding_border = PaddingBorderSize(item, grid_area_size.width);
		const AxisPair<float> box_sizing_adjustment =
			(item.box_sizing == Style::BoxSizing::ContentBox ? padding_border : AxisPair<float>{0.f, 0.f});

		// A definite preferred size.
		const Style::LengthPercentageAuto& size_style = (axis == Axis::Horizontal ? item.size_width : item.size_height);
		if (const OptionalFloat size = MaybeResolve(size_style, grid_area_size.Get(axis)))
			return *size + box_sizing_adjustment.Get(axis);

		// Else a definite minimum size (indefinite percentages resolve against 0).
		const Style::LengthPercentageAuto& min_style = (axis == Axis::Horizontal ? item.min_width : item.min_height);
		const OptionalFloat area = grid_area_size.Get(axis);
		if (const OptionalFloat min_size = MaybeResolve(min_style, area ? area : OptionalFloat(0.f)))
			return *min_size + box_sizing_adjustment.Get(axis);

		// The automatic minimum of a scroll container is 0.
		if (item.scroll_container.Get(axis))
			return 0.f;

		// The automatic minimum size (CSS Grid 2, 6.6): the content-based minimum if the item spans an 'auto' minimum track
		// and, when it spans several tracks, no flexible one.
		bool spans_auto_min_track = false, spans_flexible_track = false;
		const int begin = item.TrackRangeBegin(axis), end = item.TrackRangeEnd(axis);
		for (int i = begin; i < end; i++)
		{
			spans_auto_min_track |= axis_tracks[i].min_track_sizing_function.BehavesAsAuto(inner_node_size.Get(axis));
			spans_flexible_track |= axis_tracks[i].max_track_sizing_function.IsFr();
		}
		const bool only_span_one_track = (end - begin == 1);
		if (!(spans_auto_min_track && (only_span_one_track || !spans_flexible_track)))
			return 0.f;

		float minimum_contribution = MinContentContributionCached(item, axis, grid_area_size, grid_area_size);
		// It is clamped by the fixed maximum track sizing functions of the spanned tracks (not by 'fit-content()' limits).
		const OptionalFloat limit = SpannedFixedTrackLimit(item, axis, axis_tracks, inner_node_size.Get(axis));
		return MaybeMin(minimum_contribution, limit);
	}

	float MinimumContributionCached(GridItem& item, Axis axis, const GridTrackVec& axis_tracks, SizeOptional grid_area_size,
		SizeOptional inner_node_size)
	{
		if (const OptionalFloat cached = item.minimum_contribution_cache.Get(axis))
			return *cached;
		const float size = MinimumContribution(item, axis, axis_tracks, grid_area_size, inner_node_size);
		item.minimum_contribution_cache.Set(axis, size);
		return size;
	}

	OptionalFloat SpannedTrackLimit(const GridItem& item, Axis axis, const GridTrackVec& axis_tracks, OptionalFloat axis_parent_size)
	{
		float limit = 0.f;
		for (int i = item.TrackRangeBegin(axis); i < item.TrackRangeEnd(axis); i++)
		{
			const OptionalFloat track_limit = axis_tracks[i].max_track_sizing_function.DefiniteLimit(axis_parent_size);
			if (!track_limit)
				return std::nullopt;
			limit += *track_limit;
		}
		return limit;
	}

	OptionalFloat SpannedFixedTrackLimit(const GridItem& item, Axis axis, const GridTrackVec& axis_tracks, OptionalFloat axis_parent_size)
	{
		float limit = 0.f;
		for (int i = item.TrackRangeBegin(axis); i < item.TrackRangeEnd(axis); i++)
		{
			const OptionalFloat track_limit = axis_tracks[i].max_track_sizing_function.DefiniteValue(axis_parent_size);
			if (!track_limit)
				return std::nullopt;
			limit += *track_limit;
		}
		return limit;
	}

	float FormattedItemBaseline(Element* element, LayoutBox* layout_box)
	{
		const Box& box = element->GetBox();
		const float border_height = box.GetSize(BoxArea::Border).y;
		float baseline = 0.f;
		if (layout_box && layout_box->GetBaselineOfLastLine(baseline))
			return baseline;
		const LayoutBox::Type type = (layout_box ? layout_box->GetType() : LayoutBox::Type::BlockContainer);
		if (type == LayoutBox::Type::FlexContainer || type == LayoutBox::Type::GridContainer)
		{
			// Flex and grid containers give their element the baseline of their first item with one, from the bottom margin
			// edge; without one, that of the first item's bottom border edge (CSS Flexbox 9.8, CSS Grid 2 11.8).
			const float container_baseline = border_height + box.GetEdge(BoxArea::Margin, BoxEdge::Bottom) - element->GetBaseline();
			if (container_baseline > 0.f)
				return container_baseline;
			ScopedLayoutChildList children;
			LayoutDetails::GetLayoutChildren(element, *children, true);
			for (Element* child : *children)
			{
				const ComputedValues& computed = child->GetComputedValues();
				if (computed.display() == Style::Display::None || LayoutDetails::IsCollapsibleWhiteSpaceText(child) ||
					computed.position() == Style::Position::Absolute || computed.position() == Style::Position::Fixed)
					continue;
				return child->GetRelativeOffset(BoxArea::Border).y + child->GetBox().GetSize(BoxArea::Border).y;
			}
		}
		return border_height;
	}

	void MeasureItemBaseline(GridItem& item, SizeOptional inner_node_size, float& baseline, float& height)
	{
		Element* element = item.element;
		const Vector2f containing_block(inner_node_size.width ? *inner_node_size.width : -1.f, inner_node_size.height ? *inner_node_size.height : -1.f);

		// Laid out under a min-content width constraint (as Taffy does), its auto height.
		Box box;
		LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::UnalignedBlock);
		if (box.GetSize().x < 0.f)
		{
			const float edges = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
			const float width = LayoutDetails::GetMinContentWidth(element, containing_block);
			box.SetContent({std::max(width - edges, 0.f), box.GetSize().y});
		}
		RootBox root(Math::Max(containing_block, Vector2f(0.f)));
		UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block);
		height = element->GetBox().GetSize(BoxArea::Border).y;
		baseline = FormattedItemBaseline(element, layout_box.get());
	}

} // namespace GridLayout
} // namespace Rml
