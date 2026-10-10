/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports mod.rs (compute_grid_layout), and the grid parts of compute/oof.rs.
*/

#include "GridFormattingContext.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/ElementScroll.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/Profiling.h"
#include "../GridStyleValues.h"
#include "ContainerBox.h"
#include "GridAlignment.h"
#include "GridExplicit.h"
#include "GridItemMeasure.h"
#include "GridPlacement.h"
#include "GridTrackSizing.h"
#include "LayoutDetails.h"
#include <algorithm>
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "../Calculation.h"
#endif

namespace Rml {

using namespace GridLayout;

namespace {

	const GridLineValue* AsGridLine(const StyleValuePtr& value)
	{
		return static_cast<const GridLineValue*>(value.get());
	}

	// A gap as the fixed sizing function of the gutters.
	TrackSizingFunction GapFunction(const Style::LengthPercentage& gap)
	{
		TrackSizingFunction result;
		result.type = TrackSizingFunction::Type::Length;
		switch (gap.type)
		{
		case Style::LengthPercentage::Length:
			result.length.type = TrackLength::Type::Px;
			result.length.value = gap.value;
			break;
		case Style::LengthPercentage::Percentage:
			result.length.type = TrackLength::Type::Percent;
			result.length.value = gap.value * 0.01f;
			break;
#ifdef RMLUI_MATH_EXPRESSIONS
		case Style::LengthPercentage::Calculation:
			result.length.type = TrackLength::Type::Calc;
			result.length.calculation = gap.calculation.get();
			result.length.calculation_uses_percentage = gap.calculation && Any(gap.calculation->GetDependencyMask() & Unit::PERCENT);
			break;
#endif
		}
		return result;
	}

	OptionalFloat ToOptional(float value)
	{
		return value >= 0.f ? OptionalFloat(value) : std::nullopt;
	}

	float SumBaseSizes(const GridTrackVec& tracks)
	{
		float sum = 0.f;
		for (const GridTrack& track : tracks)
			sum += track.base_size;
		return sum;
	}

	// The self alignment of an absolutely positioned child for its static position: 'auto' takes the container's '*-items'.
	SelfAlign AbsoluteSelfAlign(const ComputedValues& computed, Style::AlignItems container_align_items, bool horizontal,
		Style::JustifyItems container_justify_items, bool& safe)
	{
		if (horizontal)
		{
			Style::JustifySelf value = computed.justify_self();
			Style::OverflowAlignment overflow = computed.justify_self_overflow();
			if (value == Style::JustifySelf::Auto)
				value = (container_justify_items == Style::JustifyItems::Legacy ? Style::JustifySelf::Normal
																				: static_cast<Style::JustifySelf>(int(container_justify_items) + 1));
			safe = (overflow == Style::OverflowAlignment::Safe);
			switch (value)
			{
			case Style::JustifySelf::Center: return SelfAlign::Center;
			case Style::JustifySelf::FlexEnd:
			case Style::JustifySelf::End:
			case Style::JustifySelf::SelfEnd:
			case Style::JustifySelf::Right: return SelfAlign::End;
			default: return SelfAlign::Start;
			}
		}
		Style::AlignSelf value = computed.align_self();
		Style::OverflowAlignment overflow = computed.align_self_overflow();
		if (value == Style::AlignSelf::Auto)
			value = static_cast<Style::AlignSelf>(int(container_align_items) + 1);
		safe = (overflow == Style::OverflowAlignment::Safe);
		switch (value)
		{
		case Style::AlignSelf::Center: return SelfAlign::Center;
		case Style::AlignSelf::FlexEnd:
		case Style::AlignSelf::End:
		case Style::AlignSelf::SelfEnd: return SelfAlign::End;
		default: return SelfAlign::Start;
		}
	}

	enum class RunMode { ComputeWidth, PerformLayout };

	/*
	    One run of the grid layout algorithm (Taffy's compute_grid_layout) for an element: its size only (the width of its
	    columns), or its layout (with its items formatted and positioned).
	*/
	struct GridRun {
		// Input.
		Element* element = nullptr;
		GridContainer* container_box = nullptr; // PerformLayout
		RunMode run_mode = RunMode::PerformLayout;
		Box box;                                         // the container's edges, and its content size when definite
		Vector2f containing_block;                       // negative: indefinite
		GridLayout::AvailableSpace measure_width;        // ComputeWidth: a min-content or max-content constraint
		Vector2f scrollbar_size;                         // the vertical scrollbar's width and the horizontal one's height

		// Output.
		float content_width = 0.f; // ComputeWidth: the width of the content box
		Vector2f border_box_size;
		Vector2f visible_overflow_size;
		Vector2f scrollable_content_size;
		float baseline = 0.f; // from the border box's top
		bool has_baseline = false;

		void Run();

	private:
		void LayoutItem(GridItem& item, const GridTrackVec& columns, const GridTrackVec& rows);
		void PlaceAbsoluteChild(Element* child, const AbsoluteAreaResolver& resolver, Style::AlignItems align_items,
			Style::JustifyItems justify_items, Line<float> border_h, Line<float> border_v, Line<float> padding_h, Line<float> padding_v);
	};

	void GridRun::Run()
	{
		const ComputedValues& computed = element->GetComputedValues();

		// 1. The available grid space (CSS Grid 2, 12.1).
		const Line<float> padding_h = {box.GetEdge(BoxArea::Padding, BoxEdge::Left), box.GetEdge(BoxArea::Padding, BoxEdge::Right)};
		const Line<float> padding_v = {box.GetEdge(BoxArea::Padding, BoxEdge::Top), box.GetEdge(BoxArea::Padding, BoxEdge::Bottom)};
		const Line<float> border_h = {box.GetEdge(BoxArea::Border, BoxEdge::Left), box.GetEdge(BoxArea::Border, BoxEdge::Right)};
		const Line<float> border_v = {box.GetEdge(BoxArea::Border, BoxEdge::Top), box.GetEdge(BoxArea::Border, BoxEdge::Bottom)};
		const AxisPair<float> padding_border_size = {padding_h.start + padding_h.end + border_h.start + border_h.end,
			padding_v.start + padding_v.end + border_v.start + border_v.end};
		// The scrollbars take space at the right and bottom of the content box.
		const AxisPair<float> content_box_inset = {padding_border_size.width + scrollbar_size.x, padding_border_size.height + scrollbar_size.y};

		// The minimum and maximum sizes, as border-box sizes.
		const bool content_box_sizing = (computed.box_sizing() == Style::BoxSizing::ContentBox);
		const OptionalFloat cb_width = ToOptional(containing_block.x), cb_height = ToOptional(containing_block.y);
		auto ToBorderBox = [content_box_sizing](OptionalFloat value, float padding_border) {
			return content_box_sizing ? MaybeAdd(value, padding_border) : value;
		};
		const SizeOptional min_size = {ToBorderBox(MaybeResolve(computed.min_width(), cb_width), padding_border_size.width),
			ToBorderBox(MaybeResolve(computed.min_height(), cb_height), padding_border_size.height)};
		const SizeOptional max_size = {ToBorderBox(MaybeResolveMax(computed.max_width(), cb_width), padding_border_size.width),
			ToBorderBox(MaybeResolveMax(computed.max_height(), cb_height), padding_border_size.height)};

		// The container's size, when definite (its box: the preferred size, or what its parent gives it).
		SizeOptional outer_node_size = {};
		if (box.GetSize().x >= 0.f)
			outer_node_size.width = box.GetSize().x + padding_border_size.width;
		if (box.GetSize().y >= 0.f)
			outer_node_size.height = box.GetSize().y + padding_border_size.height;
		outer_node_size.width = MaybeMax(MaybeClamp(outer_node_size.width, min_size.width, max_size.width), padding_border_size.width);
		outer_node_size.height = MaybeMax(MaybeClamp(outer_node_size.height, min_size.height, max_size.height), padding_border_size.height);

		AxisPair<GridLayout::AvailableSpace> available_grid_space;
		available_grid_space.width =
			outer_node_size.width ? GridLayout::AvailableSpace::Definite(*outer_node_size.width - content_box_inset.width) : measure_width;
		// The block size of a box in normal flow is indefinite: a max-content constraint.
		available_grid_space.height = outer_node_size.height
			? GridLayout::AvailableSpace::Definite(*outer_node_size.height - content_box_inset.height)
			: GridLayout::AvailableSpace::MaxContent();

		const SizeOptional inner_min_size = {MaybeSub(min_size.width, content_box_inset.width), MaybeSub(min_size.height, content_box_inset.height)};
		const SizeOptional inner_max_size = {MaybeSub(max_size.width, content_box_inset.width), MaybeSub(max_size.height, content_box_inset.height)};
		SizeOptional inner_node_size = {MaybeSub(outer_node_size.width, content_box_inset.width),
			MaybeSub(outer_node_size.height, content_box_inset.height)};

		// The container's style. Its values stay alive through the layout.
		const ContentAlignment align_content = ToContentAlignment(computed.align_content(), computed.align_content_overflow());
		const ContentAlignment justify_content = ToContentAlignment(computed.justify_content(), computed.justify_content_overflow());
		const Style::AlignItems align_items = computed.align_items();
		const Style::JustifyItems justify_items = computed.justify_items();
		const StyleValuePtr template_columns_value = computed.grid_template_columns();
		const StyleValuePtr template_rows_value = computed.grid_template_rows();
		const StyleValuePtr template_areas_value = computed.grid_template_areas();
		const StyleValuePtr auto_columns_value = computed.grid_auto_columns();
		const StyleValuePtr auto_rows_value = computed.grid_auto_rows();
		const auto* template_areas = static_cast<const GridTemplateAreasValue*>(template_areas_value.get());

		GridAxisStyle column_style, row_style;
		column_style.template_list = static_cast<const GridTrackListValue*>(template_columns_value.get());
		column_style.auto_tracks = static_cast<const GridAutoTracksValue*>(auto_columns_value.get());
		column_style.gap = GapFunction(computed.column_gap());
		column_style.element = element;
		row_style.template_list = static_cast<const GridTrackListValue*>(template_rows_value.get());
		row_style.auto_tracks = static_cast<const GridAutoTracksValue*>(auto_rows_value.get());
		row_style.gap = GapFunction(computed.row_gap());
		row_style.element = element;

		// 2. The explicit grid. Auto repetitions fill the container's size, else its maximum size, else its minimum size.
		auto AutoFitContainerSize = [&](Axis axis) -> OptionalFloat {
			OptionalFloat size = outer_node_size.Get(axis);
			if (!size)
				size = max_size.Get(axis);
			if (!size)
				size = min_size.Get(axis);
			size = MaybeMax(MaybeClamp(size, min_size.Get(axis), max_size.Get(axis)), padding_border_size.Get(axis));
			return MaybeSub(size, content_box_inset.Get(axis));
		};
		auto Strategy = [&](Axis axis) {
			return (outer_node_size.Get(axis) || max_size.Get(axis)) ? AutoRepeatStrategy::MaxRepetitionsThatDoNotOverflow
																   : AutoRepeatStrategy::MinRepetitionsThatDoOverflow;
		};
		int column_auto_repetitions = 0, template_column_count = 0;
		int row_auto_repetitions = 0, template_row_count = 0;
		ComputeExplicitGridSizeInAxis(column_style, AutoFitContainerSize(Axis::Horizontal), Strategy(Axis::Horizontal), column_auto_repetitions,
			template_column_count);
		ComputeExplicitGridSizeInAxis(row_style, AutoFitContainerSize(Axis::Vertical), Strategy(Axis::Vertical), row_auto_repetitions,
			template_row_count);

		NamedLineResolver names(column_style.template_list, row_style.template_list, template_areas, column_auto_repetitions, row_auto_repetitions);
		// The explicit grid is clamped to the track limit.
		const int explicit_column_count = std::min(std::max(template_column_count, names.AreaColumnCount()), MaxGridTracks);
		const int explicit_row_count = std::min(std::max(template_row_count, names.AreaRowCount()), MaxGridTracks);
		names.SetExplicitColumnCount(explicit_column_count);
		names.SetExplicitRowCount(explicit_row_count);

		// The grid items: the in-flow children in order-modified document order (absolutely positioned ones take no part).
		ScopedLayoutChildList children;
		LayoutDetails::GetLayoutChildren(element, *children, true);
		Vector<GridItem> items;
		Vector<ItemPlacementStyle> placements;
		Vector<StyleValuePtr> placement_values; // keeps the placement values alive
		Vector<Element*> absolute_children;
		items.reserve(children->size());
		placements.reserve(children->size());
		int source_order = 0;
		for (Element* child : *children)
		{
			const ComputedValues& child_computed = child->GetComputedValues();
			// White space between grid items makes no item (CSS Grid 2, 6).
			if (child_computed.display() == Style::Display::None || LayoutDetails::IsCollapsibleWhiteSpaceText(child))
				continue;
			const Style::Position position = child_computed.position();
			if (position == Style::Position::Absolute || position == Style::Position::Fixed)
			{
				absolute_children.push_back(child);
				continue;
			}
			if (run_mode == RunMode::PerformLayout && (position == Style::Position::Relative || position == Style::Position::Sticky))
				container_box->AddRelativeElement(child);

			GridItem item;
			item.element = child;
			item.source_order = source_order++;
			ReadGridItemStyle(item, align_items, justify_items);

			ItemPlacementStyle placement;
			if (!item.is_text)
			{
				const size_t first = placement_values.size();
				placement_values.push_back(child_computed.grid_column_start());
				placement_values.push_back(child_computed.grid_column_end());
				placement_values.push_back(child_computed.grid_row_start());
				placement_values.push_back(child_computed.grid_row_end());
				placement.column_start = AsGridLine(placement_values[first]);
				placement.column_end = AsGridLine(placement_values[first + 1]);
				placement.row_start = AsGridLine(placement_values[first + 2]);
				placement.row_end = AsGridLine(placement_values[first + 3]);
			}
			items.push_back(std::move(item));
			placements.push_back(placement);
		}

		// 3. The size of the implicit grid, estimated for the placement.
		TrackCounts estimated_column_counts, estimated_row_counts;
		ComputeGridSizeEstimate(explicit_column_count, explicit_row_count, placements, estimated_column_counts, estimated_row_counts);

		// 4. Placement.
		CellOccupancyMatrix cell_occupancy_matrix(estimated_column_counts, estimated_row_counts);
		PlaceGridItems(cell_occupancy_matrix, items, placements, computed.grid_auto_flow(), names);
		const TrackCounts final_column_counts = cell_occupancy_matrix.GetTrackCounts(Axis::Horizontal);
		const TrackCounts final_row_counts = cell_occupancy_matrix.GetTrackCounts(Axis::Vertical);

		// 5. The tracks and gutters, with their sizing functions.
		GridTrackVec columns, rows;
		InitializeGridTracks(columns, final_column_counts, column_style, column_auto_repetitions,
			[&](int index) { return cell_occupancy_matrix.ColumnIsOccupied(index); });
		InitializeGridTracks(rows, final_row_counts, row_style, row_auto_repetitions,
			[&](int index) { return cell_occupancy_matrix.RowIsOccupied(index); });

		// 6. Track sizing.
		ResolveItemTrackIndexes(items, final_column_counts, final_row_counts);
		DetermineIfItemCrossesFlexibleOrIntrinsicTracks(items, columns, rows);
		const bool has_baseline_aligned_item =
			std::any_of(items.begin(), items.end(), [](const GridItem& item) { return item.ParticipatesInBaselineAlignment(); });

		// The columns, estimating the rows by their definite maximums.
		TrackSizingAlgorithm(Axis::Horizontal, inner_min_size.width, inner_max_size.width, justify_content, align_content, available_grid_space,
			inner_node_size, columns, rows, items, TrackSizeEstimate::MaxTrackSizingFunction, has_baseline_aligned_item);
		const float initial_column_sum = SumBaseSizes(columns);
		if (!inner_node_size.width)
			inner_node_size.width = initial_column_sum;

		// Only the width requested: the rows need no sizing (column sizing is not re-run for a size-only request).
		if (run_mode == RunMode::ComputeWidth)
		{
			const float width = std::max(MaybeClamp(initial_column_sum + content_box_inset.width, min_size.width, max_size.width),
				padding_border_size.width);
			content_width = width - padding_border_size.width;
			return;
		}

		for (GridItem& item : items)
			item.grid_area_size_cache.reset();

		// The rows, with the columns' base sizes.
		TrackSizingAlgorithm(Axis::Vertical, inner_min_size.height, inner_max_size.height, align_content, justify_content, available_grid_space,
			inner_node_size, rows, columns, items, TrackSizeEstimate::BaseSize, false);
		const float initial_row_sum = SumBaseSizes(rows);

		// 7. The container's size.
		AxisPair<float> container_border_box = {
			outer_node_size.width ? *outer_node_size.width
								  : std::max(MaybeClamp(initial_column_sum + content_box_inset.width, min_size.width, max_size.width),
										padding_border_size.width),
			outer_node_size.height ? *outer_node_size.height
								   : std::max(MaybeClamp(initial_row_sum + content_box_inset.height, min_size.height, max_size.height),
										 padding_border_size.height),
		};
		AxisPair<float> container_content_box = {std::max(0.f, container_border_box.width - content_box_inset.width),
			std::max(0.f, container_border_box.height - content_box_inset.height)};

		// Percentages now resolve against the content box when the tracks are sized again.
		inner_node_size = {container_content_box.width, container_content_box.height};

		// The columns are sized again (once) when the container's width was indefinite and a column has a percentage, or when
		// the min-content contribution of an item crossing an intrinsic column changed.
		const bool has_percentage_column = std::any_of(columns.begin(), columns.end(), [](const GridTrack& track) { return track.UsesPercentage(); });
		const bool has_percentage_row = std::any_of(rows.begin(), rows.end(), [](const GridTrack& track) { return track.UsesPercentage(); });
		bool rerun_column_sizing = (!outer_node_size.width && has_percentage_column);
		bool intrinsic_column_contribution_changed = false;

		if (!rerun_column_sizing)
		{
			// Every item is visited: its caches are updated.
			for (GridItem& item : items)
			{
				if (!item.crosses_intrinsic_column)
					continue;
				const SizeOptional grid_area_size = GridAreaSize(item, Axis::Horizontal, columns, rows, inner_node_size, TrackSizeEstimate::BaseSize);
				SizeOptional available_space = grid_area_size;
				available_space.width = std::nullopt;
				const float new_min_content_contribution = MinContentContribution(item, Axis::Horizontal, grid_area_size, available_space);
				const bool has_changed = (!item.min_content_contribution_cache.width || *item.min_content_contribution_cache.width != new_min_content_contribution);
				item.grid_area_size_cache = grid_area_size;
				item.min_content_contribution_cache.width = new_min_content_contribution;
				item.max_content_contribution_cache.width.reset();
				item.minimum_contribution_cache.width.reset();
				intrinsic_column_contribution_changed |= has_changed;
			}
			rerun_column_sizing = intrinsic_column_contribution_changed;
		}
		else
		{
			for (GridItem& item : items)
			{
				item.grid_area_size_cache.reset();
				item.min_content_contribution_cache.width.reset();
				item.max_content_contribution_cache.width.reset();
				item.minimum_contribution_cache.width.reset();
			}
		}

		// The rows are sized again (once) when the container's height was indefinite and a row has a percentage, or when the
		// columns were sized again and the min-content contribution of an item crossing an intrinsic row changed.
		bool rerun_row_sizing = (!outer_node_size.height && has_percentage_row);
		bool intrinsic_row_contribution_changed = false;
		if (rerun_row_sizing)
		{
			for (GridItem& item : items)
			{
				item.grid_area_size_cache.reset();
				item.min_content_contribution_cache.height.reset();
				item.max_content_contribution_cache.height.reset();
				item.minimum_contribution_cache.height.reset();
			}
		}

		if (rerun_column_sizing)
		{
			TrackSizingAlgorithm(Axis::Horizontal, inner_min_size.width, inner_max_size.width, justify_content, align_content, available_grid_space,
				inner_node_size, columns, rows, items, TrackSizeEstimate::BaseSize, has_baseline_aligned_item);

			// The column widths may have changed the items' height contributions.
			if (!rerun_row_sizing)
			{
				for (GridItem& item : items)
				{
					if (!item.crosses_intrinsic_row)
						continue;
					const SizeOptional grid_area_size = GridAreaSize(item, Axis::Vertical, rows, columns, inner_node_size, TrackSizeEstimate::BaseSize);
					SizeOptional available_space = grid_area_size;
					available_space.height = std::nullopt;
					const float new_min_content_contribution = MinContentContribution(item, Axis::Vertical, grid_area_size, available_space);
					const bool has_changed =
						(!item.min_content_contribution_cache.height || *item.min_content_contribution_cache.height != new_min_content_contribution);
					item.grid_area_size_cache = grid_area_size;
					item.min_content_contribution_cache.height = new_min_content_contribution;
					item.max_content_contribution_cache.height.reset();
					item.minimum_contribution_cache.height.reset();
					intrinsic_row_contribution_changed |= has_changed;
				}
				rerun_row_sizing = intrinsic_row_contribution_changed;
			}
		}

		if (rerun_row_sizing)
			TrackSizingAlgorithm(Axis::Vertical, inner_min_size.height, inner_max_size.height, align_content, justify_content, available_grid_space,
				inner_node_size, rows, columns, items, TrackSizeEstimate::BaseSize, false);

		if ((intrinsic_column_contribution_changed && !has_percentage_column) || (intrinsic_row_contribution_changed && !has_percentage_row))
		{
			if (intrinsic_column_contribution_changed && !has_percentage_column)
			{
				container_border_box.width = outer_node_size.width
					? *outer_node_size.width
					: std::max(MaybeClamp(SumBaseSizes(columns) + content_box_inset.width, min_size.width, max_size.width), padding_border_size.width);
				container_content_box.width = std::max(0.f, container_border_box.width - content_box_inset.width);
			}
			if (intrinsic_row_contribution_changed && !has_percentage_row)
			{
				container_border_box.height = outer_node_size.height
					? *outer_node_size.height
					: std::max(MaybeClamp(SumBaseSizes(rows) + content_box_inset.height, min_size.height, max_size.height), padding_border_size.height);
				container_content_box.height = std::max(0.f, container_border_box.height - content_box_inset.height);
			}
		}

		border_box_size = Vector2f(container_border_box.width, container_border_box.height);

		// 8. Track alignment. The vertical scrollbar takes its space at the end of the columns.
		const float inline_size_without_scrollbar = std::max(container_border_box.width - padding_border_size.width, 0.f);
		const float inline_scrollbar_gutter_for_alignment = std::min(scrollbar_size.x, inline_size_without_scrollbar);
		AlignTracks(container_content_box.width, {padding_h.start, padding_h.end + inline_scrollbar_gutter_for_alignment}, border_h, columns,
			justify_content);
		AlignTracks(container_content_box.height, padding_v, border_v, rows, align_content);

		// The container's box takes its size before its items are formatted (their percentages, its absolute children).
		Box& sized_box = container_box->GetBox();
		sized_box.SetContent(Vector2f(container_border_box.width - padding_border_size.width, container_border_box.height - padding_border_size.height));

		// 9. Size, align and position the items.
		for (GridItem& item : items)
			LayoutItem(item, columns, rows);

		// The grid areas of the absolutely positioned boxes this container is the containing block of: its children, and
		// descendants whose containing block it is (ContainerBox::GetAbsoluteContainingBlock).
		auto AxisTracksOf = [](const TrackCounts& counts, const GridTrackVec& tracks, float padding_start, float padding_end) {
			AbsoluteAreaResolver::AxisTracks result;
			result.counts = counts;
			for (const GridTrack& track : tracks)
			{
				if (track.kind == TrackKind::Track)
					result.positions.push_back({track.offset, track.offset + track.base_size});
			}
			if (result.positions.empty() && !tracks.empty())
				result.empty_axis_line = tracks.front().offset;
			result.padding_start = padding_start;
			result.padding_end = padding_end;
			return result;
		};
		auto resolver = MakeUnique<AbsoluteAreaResolver>(names,
			AxisTracksOf(final_column_counts, columns, border_h.start, container_border_box.width - border_h.end - scrollbar_size.x),
			AxisTracksOf(final_row_counts, rows, border_v.start, container_border_box.height - border_v.end - scrollbar_size.y));
		for (Element* child : absolute_children)
			PlaceAbsoluteChild(child, *resolver, align_items, justify_items, border_h, border_v, padding_h, padding_v);
		container_box->SetAbsoluteAreaResolver(std::move(resolver));

		// 10. The container's baseline: the first item of the first row that takes part in baseline alignment, else the first
		// item of the first row.
		if (!items.empty())
		{
			int first_row = items[0].row_indexes.start;
			for (const GridItem& item : items)
				first_row = std::min(first_row, item.row_indexes.start);
			const GridItem* chosen = nullptr;
			for (const GridItem& item : items)
			{
				if (item.row_indexes.start != first_row)
					continue;
				if (!chosen)
					chosen = &item;
				if (item.ParticipatesInBaselineAlignment())
				{
					chosen = &item;
					break;
				}
			}
			baseline = chosen->y_position + (chosen->has_baseline ? chosen->baseline : chosen->height);
			has_baseline = true;
		}
	}

	void GridRun::LayoutItem(GridItem& item, const GridTrackVec& columns, const GridTrackVec& rows)
	{
		Element* item_element = item.element;
		const float area_left = columns[item.column_indexes.start + 1].offset;
		const float area_right = columns[item.column_indexes.end].offset;
		const float area_top = rows[item.row_indexes.start + 1].offset;
		const float area_bottom = rows[item.row_indexes.end].offset;
		const float area_width = std::max(area_right - area_left, 0.f);
		const float area_height = std::max(area_bottom - area_top, 0.f);

		// The item's sizes in its grid area (border box).
		const AxisPair<float> padding_border = PaddingBorderSize(item, area_width);
		const AxisPair<float> box_sizing_adjustment =
			(item.box_sizing == Style::BoxSizing::ContentBox ? padding_border : AxisPair<float>{0.f, 0.f});
		const OptionalFloat inherent_width = MaybeAdd(MaybeResolve(item.size_width, area_width), box_sizing_adjustment.width);
		const OptionalFloat inherent_height = MaybeAdd(MaybeResolve(item.size_height, area_height), box_sizing_adjustment.height);
		OptionalFloat min_width = MaybeAdd(MaybeResolve(item.min_width, area_width), box_sizing_adjustment.width);
		OptionalFloat min_height = MaybeAdd(MaybeResolve(item.min_height, area_height), box_sizing_adjustment.height);
		min_width = min_width ? std::max(*min_width, padding_border.width) : padding_border.width;
		min_height = min_height ? std::max(*min_height, padding_border.height) : padding_border.height;
		const OptionalFloat max_width = MaybeAdd(MaybeResolveMax(item.max_width, area_width), box_sizing_adjustment.width);
		const OptionalFloat max_height = MaybeAdd(MaybeResolveMax(item.max_height, area_height), box_sizing_adjustment.height);

		// 'normal' self alignment: 'stretch' without a preferred size, else 'start' (CSS Grid 2, 6.2).
		SelfAlignment horizontal = item.justify_self;
		if (horizontal.keyword == SelfAlign::Normal)
			horizontal.keyword = (inherent_width ? SelfAlign::Start : SelfAlign::Stretch);
		SelfAlignment vertical = item.align_self;
		if (vertical.keyword == SelfAlign::Normal)
			vertical.keyword = (inherent_height ? SelfAlign::Start : SelfAlign::Stretch);

		// Both margins resolve against the width of the grid area; 'auto' ones are absent.
		const Line<OptionalFloat> margin_h = {MaybeResolve(item.margin[3], area_width), MaybeResolve(item.margin[1], area_width)};
		const Line<OptionalFloat> margin_v = {MaybeResolve(item.margin[0], area_width), MaybeResolve(item.margin[2], area_width)};
		const float area_minus_margins_width = std::max(area_width - margin_h.start.value_or(0.f) - margin_h.end.value_or(0.f), 0.f);
		const float area_minus_margins_height =
			std::max(area_height - margin_v.start.value_or(0.f) - margin_v.end.value_or(0.f) - item.baseline_shim, 0.f);

		OptionalFloat width = inherent_width;
		if (!width && margin_h.start && margin_h.end && horizontal.keyword == SelfAlign::Stretch)
			width = area_minus_margins_width;
		OptionalFloat height = inherent_height;
		if (!height && margin_v.start && margin_v.end && vertical.keyword == SelfAlign::Stretch)
			height = area_minus_margins_height;
		width = MaybeClamp(width, min_width, max_width);
		height = MaybeClamp(height, min_height, max_height);

		// An auto width fits the content in the grid area.
		const SizeOptional area_size = {area_width, area_height};
		if (!width)
			width = MaybeClamp(
				MeasureChildSize(item, SizeOptional{std::nullopt, height}, area_size,
					{GridLayout::AvailableSpace::Definite(area_minus_margins_width), GridLayout::AvailableSpace::Definite(area_minus_margins_height)},
					Axis::Horizontal),
				min_width, max_width);

		// Format the item in its box: the definite sizes as content sizes, an indefinite height from its content.
		Box item_box;
		LayoutDetails::BuildBox(item_box, Vector2f(area_width, area_height), item_element, BuildBoxMode::UnalignedBlock);
		const float edges_width = item_box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
		const float edges_height = item_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
		item_box.SetContent(Vector2f(std::max(*width - edges_width, 0.f), height ? std::max(*height - edges_height, 0.f) : -1.f));

		UniquePtr<LayoutBox> item_layout_box = FormattingContext::FormatIndependent(container_box, item_element, &item_box, FormattingContextType::Block);
		const Vector2f item_size = item_element->GetBox().GetSize(BoxArea::Border);
		const float final_width = item_size.x;
		const float final_height = height ? *height : item_size.y;

		Line<float> resolved_margin_h, resolved_margin_v;
		const float x = AlignItemWithinArea({area_left, area_right}, horizontal, final_width, margin_h, 0.f, resolved_margin_h);
		const float y = AlignItemWithinArea({area_top, area_bottom}, vertical, final_height, margin_v, item.baseline_shim, resolved_margin_v);

		// A text item (an anonymous item) keeps the offset of its first line in the anonymous block it was formatted in.
		Vector2f text_offset;
		if (item.is_text)
			text_offset = item_element->GetRelativeOffset(BoxArea::Border);
		item_element->SetOffset(Vector2f(x, y) + text_offset, element);

		item.y_position = y;
		item.height = final_height;
		if (!item.has_baseline)
		{
			item.baseline = FormattedItemBaseline(item_element, item_layout_box.get());
			item.has_baseline = true;
		}
		else
		{
			// The baseline of a baseline-aligned item was measured with its top margin.
			const OptionalFloat margin_top = MaybeResolve(item.margin[0], area_width);
			item.baseline -= margin_top.value_or(0.f);
		}

		// The item and its content may overflow: their border boxes extend the visible overflow, the item's margin box the
		// scrollable overflow, relative to the container's content box.
		const Vector2f content_offset = container_box->GetBox().GetPosition();
		const Vector2f item_offset = Vector2f(x, y) - content_offset;
		if (item_layout_box)
			visible_overflow_size = Math::Max(visible_overflow_size, item_offset + item_layout_box->GetVisibleOverflowSize());
		scrollable_content_size =
			Math::Max(scrollable_content_size, item_offset + Vector2f(final_width + resolved_margin_h.end, final_height + resolved_margin_v.end));
	}

	void GridRun::PlaceAbsoluteChild(Element* child, const AbsoluteAreaResolver& resolver, Style::AlignItems align_items,
		Style::JustifyItems justify_items, Line<float> border_h, Line<float> border_v, Line<float> padding_h, Line<float> padding_v)
	{
		const ComputedValues& child_computed = child->GetComputedValues();
		ContainerBox* containing_container = LayoutDetails::GetContainingBlock(container_box, child_computed.position()).container;
		const bool grid_is_containing_block = (containing_container == container_box);

		// The static-position rectangle (CSS Grid 2, 10.2): the grid area when the grid is the containing block (also its
		// containing block), else the grid's content box.
		Rectanglef area;
		if (grid_is_containing_block)
			area = resolver.Resolve(child);
		else
			area = Rectanglef::FromCorners(Vector2f(border_h.start + padding_h.start, border_v.start + padding_v.start),
				Vector2f(border_box_size.x - border_h.end - padding_h.end - scrollbar_size.x,
					border_box_size.y - border_v.end - padding_v.end - scrollbar_size.y));

		// With both insets of an axis 'auto' the box is aligned in the rectangle by its self alignment.
		Vector2f static_position = area.Position();
		const bool auto_insets_h = (child_computed.left().type == Style::LengthPercentageAuto::Auto && child_computed.right().type == Style::LengthPercentageAuto::Auto);
		const bool auto_insets_v = (child_computed.top().type == Style::LengthPercentageAuto::Auto && child_computed.bottom().type == Style::LengthPercentageAuto::Auto);
		bool safe_h = false, safe_v = false;
		const SelfAlign align_h = AbsoluteSelfAlign(child_computed, align_items, true, justify_items, safe_h);
		const SelfAlign align_v = AbsoluteSelfAlign(child_computed, align_items, false, justify_items, safe_v);
		const bool align_horizontally = (auto_insets_h && align_h != SelfAlign::Start);
		const bool align_vertically = (auto_insets_v && align_v != SelfAlign::Start);
		if (align_horizontally || align_vertically)
		{
			// The size of its margin box, as it will be laid out: its width fits its content, its height follows.
			const Vector2f block_size = (grid_is_containing_block ? area.Size() : LayoutDetails::GetContainingBlock(container_box, child_computed.position()).size);
			Box child_box;
			LayoutDetails::BuildBox(child_box, block_size, child, BuildBoxMode::UnalignedBlock);
			float content_width = child_box.GetSize().x;
			if (content_width < 0.f)
				content_width = LayoutDetails::GetShrinkToFitWidth(child, block_size);
			const float border_width = content_width + child_box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
			const float border_height = child_box.GetSize().y >= 0.f
				? child_box.GetSize().y + child_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)
				: LayoutDetails::GetContentHeightForWidth(child, border_width, block_size);
			const Vector2f margin_box_size = Vector2f(border_width + child_box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Margin, BoxArea::Margin),
				border_height + child_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Margin, BoxArea::Margin));

			auto Align = [](float start, float size, float margin_box, SelfAlign keyword, bool safe) {
				if (safe && margin_box > size)
					return start;
				if (keyword == SelfAlign::Center)
					return start + (size - margin_box) * 0.5f;
				if (keyword == SelfAlign::End)
					return start + size - margin_box;
				return start;
			};
			if (align_horizontally)
				static_position.x = Align(area.Left(), area.Width(), margin_box_size.x, align_h, safe_h);
			if (align_vertically)
				static_position.y = Align(area.Top(), area.Height(), margin_box_size.y, align_v, safe_v);
		}

		if (grid_is_containing_block)
			container_box->AddAbsoluteElement(child, static_position, element, area);
		else
			containing_container->AddAbsoluteElement(child, static_position, element);
	}

} // namespace

namespace GridLayout {

	AbsoluteAreaResolver::AbsoluteAreaResolver(const NamedLineResolver& names, AxisTracks columns, AxisTracks rows) :
		names(names), columns(std::move(columns)), rows(std::move(rows))
	{}

	Line<float> AbsoluteAreaResolver::ResolveAxis(Axis axis, const AxisTracks& tracks, Element* element) const
	{
		const ComputedValues& computed = element->GetComputedValues();
		const StyleValuePtr start_value = (axis == Axis::Horizontal ? computed.grid_column_start() : computed.grid_row_start());
		const StyleValuePtr end_value = (axis == Axis::Horizontal ? computed.grid_column_end() : computed.grid_row_end());

		// The placement's lines (swapped when reversed), lines outside the grid read as 'auto' (CSS Grid 2, 10.1).
		const int min_line = -tracks.counts.negative_implicit;
		const int max_line = tracks.counts.explicit_count + tracks.counts.positive_implicit;
		const PlacementLine oz = ToOriginZero(names.ResolveNames(axis, AsGridLine(start_value), AsGridLine(end_value)), tracks.counts.explicit_count);
		const Line<std::optional<int>> lines = ResolveAbsolutelyPositionedGridTracks(oz);
		auto TrackIndex = [&](std::optional<int> line) -> std::optional<int> {
			if (!line || *line < min_line || *line > max_line)
				return std::nullopt;
			const int index = TryIntoTrackVecIndex(*line, tracks.counts);
			if (index < 0)
				return std::nullopt;
			return index / 2;
		};
		const std::optional<int> start = TrackIndex(lines.start);
		const std::optional<int> end = TrackIndex(lines.end);

		// A start line is the start of the track after it, an end line the end of the track before it.
		float start_position = tracks.padding_start;
		if (start)
		{
			if (*start < (int)tracks.positions.size())
				start_position = tracks.positions[*start].start;
			else if (!tracks.positions.empty())
				start_position = tracks.positions.back().end;
			else if (tracks.empty_axis_line)
				start_position = *tracks.empty_axis_line;
		}
		float end_position = tracks.padding_end;
		if (end)
		{
			if (*end >= 1 && *end - 1 < (int)tracks.positions.size())
				end_position = tracks.positions[*end - 1].end;
			else if (!tracks.positions.empty())
				end_position = tracks.positions.front().start;
			else if (tracks.empty_axis_line)
				end_position = *tracks.empty_axis_line;
		}
		return {std::min(start_position, end_position), std::max(start_position, end_position)};
	}

	Rectanglef AbsoluteAreaResolver::Resolve(Element* element) const
	{
		const Line<float> horizontal = ResolveAxis(Axis::Horizontal, columns, element);
		const Line<float> vertical = ResolveAxis(Axis::Vertical, rows, element);
		return Rectanglef::FromCorners(Vector2f(horizontal.start, vertical.start), Vector2f(horizontal.end, vertical.end));
	}

} // namespace GridLayout

UniquePtr<LayoutBox> GridFormattingContext::Format(ContainerBox* parent_container, Element* element, const Box* override_initial_box)
{
	RMLUI_ZoneScopedC(0xAF4FAF);
	auto grid_container_box = MakeUnique<GridContainer>(element, parent_container);

	ElementScroll* element_scroll = element->GetElementScroll();
	const ComputedValues& computed = element->GetComputedValues();
	const Vector2f containing_block = LayoutDetails::GetContainingBlock(parent_container, element).size;

	// The initial box from the element's style, as a block box (its width is definite).
	Box& box = grid_container_box->GetBox();
	if (override_initial_box)
		box = *override_initial_box;
	else
		LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::Block);
	const Box initial_box = box;

	float min_height, max_height;
	LayoutDetails::GetMinMaxHeight(min_height, max_height, computed, initial_box, containing_block.y);

	// Start with any auto-scrollbars off.
	grid_container_box->ResetScrollbars(initial_box);
	const bool auto_height = (initial_box.GetSize().y < 0.f);

	for (int layout_iteration = 0; layout_iteration < 3; layout_iteration++)
	{
		box = initial_box;
		GridRun run;
		run.element = element;
		run.container_box = grid_container_box.get();
		run.run_mode = RunMode::PerformLayout;
		run.box = initial_box;
		run.containing_block = containing_block;
		run.scrollbar_size = Vector2f(element_scroll->GetScrollbarSize(ElementScroll::VERTICAL), element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL));
		run.Run();

		// Catch overflow before the height is fixed: an enabled scrollbar then extends the container's height instead of taking
		// space from its content, which could need a scrollbar in the other axis.
		if (auto_height && !grid_container_box->CatchOverflow(run.visible_overflow_size, run.scrollable_content_size, initial_box, max_height))
			continue;

		Box sized_box = initial_box;
		const float padding_border_width = initial_box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
		const float padding_border_height = initial_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
		sized_box.SetContent(Vector2f(run.border_box_size.x - padding_border_width, run.border_box_size.y - padding_border_height));
		box = sized_box;

		// The element's baseline is its distance from its bottom margin edge.
		const float border_height = sized_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border);
		const float baseline_from_top = (run.has_baseline ? run.baseline : border_height);
		const float element_baseline = border_height + sized_box.GetEdge(BoxArea::Margin, BoxEdge::Bottom) - baseline_from_top;

		// Close the box; when new scrollbars were enabled, format again.
		if (grid_container_box->Close(run.visible_overflow_size, run.scrollable_content_size, sized_box, element_baseline))
			break;
	}

	return grid_container_box;
}

static float ComputeIntrinsicWidth(Element* element, Vector2f containing_block, GridLayout::AvailableSpace constraint)
{
	GridRun run;
	run.element = element;
	run.run_mode = RunMode::ComputeWidth;
	run.measure_width = constraint;
	run.containing_block = containing_block;
	LayoutDetails::BuildBox(run.box, containing_block, element, BuildBoxMode::UnalignedBlock);
	// A definite width is its own intrinsic width.
	if (run.box.GetSize().x >= 0.f)
		return run.box.GetSize().x;
	// 'overflow: scroll' reserves its scrollbar's space whether it scrolls or not.
	const ComputedValues& computed = element->GetComputedValues();
	ElementScroll* element_scroll = element->GetElementScroll();
	if (computed.overflow_y() == Style::Overflow::Scroll)
		run.scrollbar_size.x = element_scroll->GetScrollbarSize(ElementScroll::VERTICAL);
	if (computed.overflow_x() == Style::Overflow::Scroll)
		run.scrollbar_size.y = element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL);
	run.Run();
	return run.content_width;
}

float GridFormattingContext::GetMaxContentWidth(Element* element, Vector2f containing_block)
{
	return ComputeIntrinsicWidth(element, containing_block, GridLayout::AvailableSpace::MaxContent());
}

float GridFormattingContext::GetMinContentWidth(Element* element, Vector2f containing_block)
{
	return ComputeIntrinsicWidth(element, containing_block, GridLayout::AvailableSpace::MinContent());
}

} // namespace Rml
