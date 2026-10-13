#include "FlexFormattingContext.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/ElementScroll.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/Profiling.h"
#include "../../../Include/RmlUi/Core/Types.h"
#include "ContainerBox.h"
#include "LayoutDetails.h"
#include <algorithm>
#include <cmath>
#include <float.h>
#include <optional>

namespace Rml {

/*
    A port of Taffy's flexbox algorithm (src/compute/flexbox.rs at 8409f3f8, MIT; see LICENSE-taffy.txt), structured as its
    functions are, with RmlUi's formatting contexts measuring and laying out the items. Sizes are border-box sizes ('inner'
    excludes the margins, 'outer' includes them) and positions are relative to the flex container's border box, as in Taffy.
    Taffy's right-to-left and writing-mode support is left out (RmlUi lays out flex containers left to right).
*/

namespace {

	using Opt = std::optional<float>;

	// ComputeWidth: only the container's width is requested (Taffy's ComputeSize with RequestedAxis::Horizontal).
	enum class RunMode { ComputeSize, ComputeWidth, PerformLayout };

	// Alignment of items and lines, Taffy's AlignItemsKeyword and AlignContentKeyword ('normal' resolved by the caller).
	enum class ItemAlign : uint8_t { Start, End, FlexStart, FlexEnd, Center, Baseline, LastBaseline, Stretch };
	enum class ContentAlign : uint8_t { Start, End, FlexStart, FlexEnd, Center, Stretch, SpaceBetween, SpaceEvenly, SpaceAround };

	// Edges in the order left, right, top, bottom.
	enum EdgeIndex { Left, Right, Top, Bottom };
	struct Edges {
		float v[4] = {};
		float Horizontal() const { return v[Left] + v[Right]; }
		float Vertical() const { return v[Top] + v[Bottom]; }
	};

	Opt Clamp(Opt value, Opt min, Opt max)
	{
		if (!value)
			return value;
		float result = *value;
		if (max)
			result = Math::Min(result, *max);
		if (min)
			result = Math::Max(result, *min);
		return result;
	}
	float Clamp(float value, Opt min, Opt max)
	{
		if (max)
			value = Math::Min(value, *max);
		if (min)
			value = Math::Max(value, *min);
		return value;
	}
	// Taffy's maybe_min / maybe_max: an empty left side stays empty, an empty right side changes nothing.
	Opt Min(Opt a, Opt b)
	{
		if (a && b)
			return Math::Min(*a, *b);
		return a;
	}
	Opt Max(Opt a, Opt b)
	{
		if (a && b)
			return Math::Max(*a, *b);
		return a;
	}
	Opt Sub(Opt a, float b)
	{
		return a ? Opt(*a - b) : a;
	}
	Opt Add(Opt a, float b)
	{
		return a ? Opt(*a + b) : a;
	}

	// A size of optional values, [0] = width, [1] = height.
	struct OptSize {
		Opt v[2];
		Opt& operator[](int i) { return v[i]; }
		const Opt& operator[](int i) const { return v[i]; }
	};

	// Applies an aspect ratio (width / height) to a size with one known axis.
	OptSize ApplyAspectRatio(OptSize size, float ratio)
	{
		if (ratio > 0.f)
		{
			if (size[0] && !size[1])
				size[1] = *size[0] / ratio;
			else if (!size[0] && size[1])
				size[0] = *size[1] * ratio;
		}
		return size;
	}

	struct FlexItem {
		Element* element = nullptr;
		size_t order = 0;           // Index in the container's layout children.

		OptSize size;               // Border-box size from the size properties (with the aspect ratio applied).
		Style::SizingKeyword size_keyword[2] = {Style::SizingKeyword::None, Style::SizingKeyword::None};
		OptSize min_size, max_size; // Border-box.
		float aspect_ratio = 0.f;   // Width / height, 0 for none.
		ItemAlign align_self = ItemAlign::Stretch;
		bool align_safe = false;
		bool is_scroll_container = false;
		float flex_shrink = 1.f, flex_grow = 0.f;
		bool flex_basis_is_definite = false;

		// The automatic minimum main size, measured when first needed (see ResolvedMinimumMainSize).
		bool resolved_minimum_known = false;
		float resolved_minimum_main_size = 0.f;
		float resolved_minimum_upper_bound = FLT_MAX; // A value no smaller than the resolved minimum.

		Edges margin;
		bool margin_auto[4] = {};
		Edges padding, border;

		float flex_basis = 0.f, inner_flex_basis = 0.f, violation = 0.f;
		bool frozen = false;
		float content_flex_fraction = 0.f;

		Vector2f hypothetical_inner_size, hypothetical_outer_size, target_size, outer_target_size;

		float baseline = 0.f;
		float offset_main = 0.f, offset_cross = 0.f;

		// The cross-axis available space and known size of the item while its main size is measured.
		float measure_cross_available = 0.f;
		bool measure_cross_available_definite = false;
		Opt measure_cross_known;

		Box box; // Edges as built for the element; the content size is set for the final layout.
	};

	struct FlexLine {
		size_t first = 0, count = 0; // Range of the items.
		float cross_size = 0.f;
		float offset_cross = 0.f;
	};

	class FlexAlgorithm {
	public:
		FlexAlgorithm(Element* element, FlexContainer* container_box, const Box& box, Vector2f containing_block) :
			element(element), container_box(container_box), box(box), containing_block(containing_block)
		{}

		struct Output {
			Vector2f size;                // Border-box size of the container.
			bool has_baseline = false;    // First baseline, from the container's border-box top.
			float first_baseline = 0.f;
			Vector2f visible_overflow;    // Border-box overflow of the items, from the container's border-box origin.
			Vector2f scrollable_overflow; // Margin-box extent of the items.
		};

		// The known height was given by a flex container and is not definite (LayoutDetails::IndefiniteHeightScope).
		void SetKnownHeightIndefinite(bool indefinite) { known_height_definite = !indefinite; }

		// Known border-box size of the container (from its box), available space, and the scrollbar gutter.
		Output Run(OptSize known, AvailableSpace available_width, AvailableSpace available_height, Vector2f scrollbar_gutter, RunMode run_mode);

	private:
		// -- Constants (Taffy's AlgoConstants) --
		bool row = true;      // Main axis horizontal.
		bool reverse = false; // row-reverse or column-reverse.
		bool is_wrap = false, is_wrap_reverse = false, is_balance = false;
		int line_count = 1;

		OptSize container_min_size, container_max_size;
		Edges container_border, content_box_inset;
		Vector2f gap;
		ItemAlign align_items = ItemAlign::Stretch;
		bool align_items_safe = false;
		ContentAlign align_content = ContentAlign::Stretch, justify_content = ContentAlign::FlexStart;
		bool align_content_safe = false, justify_content_safe = false;

		OptSize node_outer_size, node_inner_size;
		bool known_height_definite = true;
		bool known_main_size_is_definite = true, has_definite_main_size = false, has_definite_cross_size = false;
		bool cross_axis_available_space_is_definite = false;
		Vector2f container_size, inner_container_size;

		Vector<FlexItem> items;
		Vector<FlexLine> lines;
		Vector<Element*> absolute_children; // In order, with their index among the layout children.
		Vector<size_t> absolute_orders;

		// -- Axis helpers --
		int MainIndex() const { return row ? 0 : 1; }
		int CrossIndex() const { return row ? 1 : 0; }
		float Main(Vector2f v) const { return row ? v.x : v.y; }
		float Cross(Vector2f v) const { return row ? v.y : v.x; }
		void SetMain(Vector2f& v, float value) const { (row ? v.x : v.y) = value; }
		void SetCross(Vector2f& v, float value) const { (row ? v.y : v.x) = value; }
		Opt Main(const OptSize& s) const { return s[MainIndex()]; }
		Opt Cross(const OptSize& s) const { return s[CrossIndex()]; }
		float MainStart(const Edges& e) const { return e.v[row ? Left : Top]; }
		float MainEnd(const Edges& e) const { return e.v[row ? Right : Bottom]; }
		float CrossStart(const Edges& e) const { return e.v[row ? Top : Left]; }
		float CrossEnd(const Edges& e) const { return e.v[row ? Bottom : Right]; }
		float MainSum(const Edges& e) const { return row ? e.Horizontal() : e.Vertical(); }
		float CrossSum(const Edges& e) const { return row ? e.Vertical() : e.Horizontal(); }
		bool MainStartAuto(const FlexItem& item) const { return item.margin_auto[row ? Left : Top]; }
		bool MainEndAuto(const FlexItem& item) const { return item.margin_auto[row ? Right : Bottom]; }
		bool CrossStartAuto(const FlexItem& item) const { return item.margin_auto[row ? Top : Left]; }
		bool CrossEndAuto(const FlexItem& item) const { return item.margin_auto[row ? Bottom : Right]; }
		Vector2f FromMainCross(float main, float cross) const { return row ? Vector2f(main, cross) : Vector2f(cross, main); }

		bool ParticipatesInBaselineAlignment(const FlexItem& item) const
		{
			return (item.align_self == ItemAlign::Baseline || item.align_self == ItemAlign::LastBaseline) && !CrossStartAuto(item) &&
				!CrossEndAuto(item);
		}

		// The containing block of the items (the container's content box), negative where indefinite.
		Vector2f ItemContainingBlock() const
		{
			return Vector2f(node_inner_size[0] ? *node_inner_size[0] : -1.f, node_inner_size[1] ? *node_inner_size[1] : -1.f);
		}
		float DividedCrossSpace(float cross_available_space) const
		{
			if (is_wrap && line_count > 1)
				return (cross_available_space - float(line_count - 1) * Cross(gap)) / float(line_count);
			return cross_available_space;
		}

		// Whether the height the item is given is definite, for the percentage heights of its children (Taffy's
		// item_known_dimension_definiteness; widths are always definite here). The flexed main size is definite if the
		// container has a definite main size or the flex basis is definite; the cross size if it is stretched or specified.
		bool ItemHeightIsDefinite(const FlexItem& item) const
		{
			if (!row)
				return has_definite_main_size || item.flex_basis_is_definite;
			const bool stretched = !CrossStartAuto(item) && !CrossEndAuto(item) &&
				(item.size_keyword[1] == Style::SizingKeyword::Stretch ||
					(item.align_self == ItemAlign::Stretch && item.size_keyword[1] == Style::SizingKeyword::None));
			return stretched || item.size[1].has_value();
		}

		// -- Measuring (Taffy's measure_child_size, in content-size mode) --
		float MeasureChildSize(const FlexItem& item, OptSize known, AvailableSpace available_width, AvailableSpace available_height, int axis) const;
		float ResolvedMinimumMainSize(FlexItem& item);

		// -- Steps --
		void ComputeConstants(OptSize known, AvailableSpace available_width, AvailableSpace available_height, Vector2f scrollbar_gutter);
		void GenerateItems();
		void DetermineFlexBaseSize(const AvailableSpace available[2]);
		void CollectFlexLines(const AvailableSpace available[2]);
		void CollectBalancedFlexLines(const AvailableSpace available[2]);
		AvailableSpace LineBreakAvailableSpace(const AvailableSpace available[2]) const;
		void DetermineContainerMainSize(const AvailableSpace available[2]);
		void ResolveFlexibleLengths(FlexLine& line);
		void DetermineHypotheticalCrossSize(FlexLine& line, const AvailableSpace available[2]);
		void CalculateChildrenBaselines(const AvailableSpace available[2]);
		void CalculateCrossSize(OptSize known);
		void HandleAlignContentStretch(OptSize known);
		void DetermineUsedCrossSize();
		void DistributeRemainingFreeSpace();
		void ResolveCrossAxisAutoMargins();
		float AlignFlexItemAlongCrossAxis(const FlexItem& item, float free_space, float max_baseline, float max_baseline_to_bottom) const;
		float DetermineContainerCrossSize(OptSize known);
		void AlignFlexLinesPerAlignContent(float total_cross_size);
		void FinalLayoutPass(Output& output);
		void PlaceAbsoluteChildren();

		Element* element;
		FlexContainer* container_box;
		const Box& box;
		Vector2f containing_block;
	};

	ItemAlign ToItemAlign(Style::AlignSelf value)
	{
		using Style::AlignSelf;
		switch (value)
		{
		case AlignSelf::FlexStart: return ItemAlign::FlexStart;
		case AlignSelf::FlexEnd: return ItemAlign::FlexEnd;
		case AlignSelf::Center: return ItemAlign::Center;
		case AlignSelf::Baseline: return ItemAlign::Baseline;
		case AlignSelf::LastBaseline: return ItemAlign::LastBaseline;
		case AlignSelf::Start:
		case AlignSelf::SelfStart: return ItemAlign::Start;
		case AlignSelf::End:
		case AlignSelf::SelfEnd: return ItemAlign::End;
		case AlignSelf::Auto:
		case AlignSelf::Normal:
		case AlignSelf::Stretch: break;
		}
		return ItemAlign::Stretch;
	}

	ContentAlign ToContentAlign(Style::AlignContent value)
	{
		using Style::AlignContent;
		switch (value)
		{
		case AlignContent::FlexStart: return ContentAlign::FlexStart;
		case AlignContent::FlexEnd: return ContentAlign::FlexEnd;
		case AlignContent::Center: return ContentAlign::Center;
		case AlignContent::SpaceBetween: return ContentAlign::SpaceBetween;
		case AlignContent::SpaceAround: return ContentAlign::SpaceAround;
		case AlignContent::SpaceEvenly: return ContentAlign::SpaceEvenly;
		case AlignContent::Start: return ContentAlign::Start;
		case AlignContent::End: return ContentAlign::End;
		case AlignContent::Normal:
		case AlignContent::Stretch: break;
		}
		return ContentAlign::Stretch;
	}

	ContentAlign ToContentAlign(Style::JustifyContent value, bool row)
	{
		using Style::JustifyContent;
		switch (value)
		{
		case JustifyContent::FlexStart: return ContentAlign::FlexStart;
		case JustifyContent::FlexEnd: return ContentAlign::FlexEnd;
		case JustifyContent::Center: return ContentAlign::Center;
		case JustifyContent::SpaceBetween: return ContentAlign::SpaceBetween;
		case JustifyContent::SpaceAround: return ContentAlign::SpaceAround;
		case JustifyContent::SpaceEvenly: return ContentAlign::SpaceEvenly;
		case JustifyContent::Start: return ContentAlign::Start;
		case JustifyContent::End: return ContentAlign::End;
		// 'left' and 'right' are 'start' and 'end' on a horizontal main axis (left to right), 'start' otherwise.
		case JustifyContent::Left: return ContentAlign::Start;
		case JustifyContent::Right: return row ? ContentAlign::End : ContentAlign::Start;
		// 'normal' behaves as 'stretch', which behaves as 'flex-start' in the main axis.
		case JustifyContent::Normal:
		case JustifyContent::Stretch: break;
		}
		return ContentAlign::FlexStart;
	}

	// Taffy's apply_alignment_fallback: distributed keywords fall back with a single item or on overflow, 'safe' to 'start'.
	ContentAlign ApplyAlignmentFallback(float free_space, size_t num_items, ContentAlign mode, bool is_safe)
	{
		if (num_items <= 1 || free_space <= 0.f)
		{
			switch (mode)
			{
			case ContentAlign::Stretch:
			case ContentAlign::SpaceBetween:
				mode = ContentAlign::FlexStart;
				is_safe = true;
				break;
			case ContentAlign::SpaceAround:
			case ContentAlign::SpaceEvenly:
				mode = ContentAlign::Center;
				is_safe = true;
				break;
			default: break;
			}
		}
		if (free_space <= 0.f && is_safe)
			mode = ContentAlign::Start;
		return mode;
	}

	// Taffy's compute_alignment_offset.
	float ComputeAlignmentOffset(float free_space, size_t num_items, float gap, ContentAlign mode, bool layout_is_flex_reversed, bool is_first)
	{
		if (is_first)
		{
			switch (mode)
			{
			case ContentAlign::Start: return 0.f;
			case ContentAlign::FlexStart: return layout_is_flex_reversed ? free_space : 0.f;
			case ContentAlign::End: return free_space;
			case ContentAlign::FlexEnd: return layout_is_flex_reversed ? 0.f : free_space;
			case ContentAlign::Center: return free_space / 2.f;
			case ContentAlign::Stretch:
			case ContentAlign::SpaceBetween: return 0.f;
			case ContentAlign::SpaceAround: return free_space >= 0.f ? (free_space / float(num_items)) / 2.f : free_space / 2.f;
			case ContentAlign::SpaceEvenly: return free_space >= 0.f ? free_space / float(num_items + 1) : free_space / 2.f;
			}
			return 0.f;
		}

		free_space = Math::Max(free_space, 0.f);
		switch (mode)
		{
		case ContentAlign::SpaceBetween: return gap + free_space / float(num_items - 1);
		case ContentAlign::SpaceAround: return gap + free_space / float(num_items);
		case ContentAlign::SpaceEvenly: return gap + free_space / float(num_items + 1);
		default: break;
		}
		return gap;
	}

	float SumAxisGaps(float gap, size_t num_items)
	{
		return num_items <= 1 ? 0.f : gap * float(num_items - 1);
	}

	// -- Balanced line breaking for 'flex-wrap: balance' (Taffy's flexbox::balance, CSS Flexbox 2 §balancing) --
	namespace Balance {
		constexpr double Infeasible = HUGE_VAL;

		struct LineSizes {
			Vector<std::pair<double, bool>> sums; // Prefix sums of item size + gap, and whether the item is zero-sized.
			double gap_between_items = 0.0;
			double limit = 0.0;

			size_t ItemCount() const { return sums.size(); }
			bool IsZeroItem(size_t index) const { return sums[index].second; }
			double LineSize(size_t start, size_t end) const
			{
				const double start_sum = (start == 0 ? 0.0 : sums[start - 1].first);
				return sums[end].first - start_sum - gap_between_items;
			}
			double LineCost(size_t start, size_t end) const
			{
				const double size = LineSize(start, end);
				if (end > start && size > limit)
					return Infeasible;
				return size * size;
			}
			Vector<uint32_t> SuffixGreedyLineCounts() const
			{
				const size_t item_count = ItemCount();
				Vector<uint32_t> counts(item_count + 1, 0);
				size_t line_end = item_count;
				for (size_t start = item_count; start-- > 0;)
				{
					while (line_end > start + 1 && LineSize(start, line_end - 1) > limit)
						line_end--;
					counts[start] = 1 + counts[line_end];
				}
				return counts;
			}
			Vector<uint32_t> FitEnds() const
			{
				const size_t item_count = ItemCount();
				Vector<uint32_t> fit_ends;
				fit_ends.reserve(item_count);
				size_t fit_end = 0;
				for (size_t start = 0; start < item_count; start++)
				{
					if (fit_end < start)
						fit_end = start;
					while (fit_end + 1 < item_count && LineSize(start, fit_end + 1) <= limit)
						fit_end++;
					fit_ends.push_back(uint32_t(fit_end));
				}
				return fit_ends;
			}
		};

		struct Row {
			const LineSizes* sizes;
			const Vector<uint32_t>* fit_ends;
			const Vector<double>* prev;
			const Vector<uint32_t>* nonzero_ends;
			size_t max_end;
		};

		void FillRow(const Row& row, Vector<double>& cur, uint32_t* opts, size_t start_lo, size_t start_hi, size_t col_lo, size_t col_hi)
		{
			if (start_lo > start_hi)
				return;
			const size_t start = start_lo + (start_hi - start_lo) / 2;
			const Vector<uint32_t>& nonzero_ends = *row.nonzero_ends;

			size_t first_col = col_lo;
			while (first_col < col_hi && size_t(nonzero_ends[first_col]) < start)
				first_col++;

			double min_cost = Infeasible;
			std::optional<size_t> min_col;
			for (size_t col = first_col; col < col_hi; col++)
			{
				const size_t end = nonzero_ends[col];
				const double line_cost = row.sizes->LineCost(start, end);
				if (line_cost == Infeasible)
					break;
				const double cost = line_cost + (*row.prev)[end + 1];
				if (cost <= min_cost)
				{
					min_cost = cost;
					min_col = col;
				}
			}

			std::optional<size_t> min_end;
			if (min_col)
				min_end = size_t(nonzero_ends[*min_col]);
			const size_t zero_end = Math::Min(size_t((*row.fit_ends)[start]), row.max_end);
			if (row.sizes->IsZeroItem(zero_end + 1))
			{
				const double cost = row.sizes->LineCost(start, zero_end) + (*row.prev)[zero_end + 1];
				if (cost < min_cost || (cost == min_cost && (!min_end || *min_end < zero_end)))
				{
					min_cost = cost;
					min_end = zero_end;
				}
			}

			cur[start] = min_cost;
			opts[start] = uint32_t(min_end ? *min_end : start);

			if (start > start_lo)
				FillRow(row, cur, opts, start_lo, start - 1, col_lo, min_col ? *min_col + 1 : col_hi);
			if (start < start_hi)
				FillRow(row, cur, opts, start + 1, start_hi, min_col ? *min_col : first_col, col_hi);
		}

		// The number of items on each line that balances the items across lines (at least 'min_line_count' lines).
		Vector<size_t> BalancedLineItemCounts(const Vector<float>& item_sizes, float line_limit, float gap_between_items, size_t min_line_count)
		{
			const size_t item_count = item_sizes.size();
			RMLUI_ASSERT(item_count > 0);
			auto ToSize = [](float value) { return Math::Clamp(double(value), 0.0, double(FLT_MAX)); };

			LineSizes sizes;
			sizes.gap_between_items = ToSize(gap_between_items);
			sizes.limit = double(line_limit);
			sizes.sums.reserve(item_count);
			double sum = 0.0;
			for (float item_size : item_sizes)
			{
				const double size = ToSize(item_size);
				sum += size + sizes.gap_between_items;
				sizes.sums.emplace_back(sum, size == 0.0);
			}

			const Vector<uint32_t> suffix_greedy = sizes.SuffixGreedyLineCounts();
			const size_t line_count = Math::Max(size_t(suffix_greedy[0]), Math::Clamp(min_line_count, size_t(1), item_count));

			Vector<size_t> item_counts;
			item_counts.reserve(line_count);
			if (line_count == item_count)
			{
				item_counts.assign(item_count, 1);
				return item_counts;
			}

			const Vector<uint32_t> fit_ends = sizes.FitEnds();
			Vector<uint32_t> nonzero_ends;
			for (size_t end = 0; end + 1 < item_count; end++)
			{
				if (!sizes.IsZeroItem(end + 1))
					nonzero_ends.push_back(uint32_t(end));
			}

			Vector<double> prev(item_count), cur(item_count, Infeasible);
			for (size_t start = 0; start < item_count; start++)
				prev[start] = sizes.LineCost(start, item_count - 1);
			Vector<uint32_t> opts((line_count - 1) * item_count, 0);

			for (size_t lines = 2; lines <= line_count; lines++)
			{
				const size_t max_end = item_count - lines;
				size_t first_start = 0;
				while (first_start < max_end && size_t(suffix_greedy[first_start]) > lines)
				{
					cur[first_start] = Infeasible;
					first_start++;
				}
				size_t col_hi = 0;
				while (col_hi < nonzero_ends.size() && size_t(nonzero_ends[col_hi]) <= max_end)
					col_hi++;
				const Row row{&sizes, &fit_ends, &prev, &nonzero_ends, max_end};
				FillRow(row, cur, opts.data() + (lines - 2) * item_count, first_start, max_end, 0, col_hi);
				std::swap(prev, cur);
			}

			size_t start = 0;
			for (size_t lines = line_count; lines >= 2; lines--)
			{
				const size_t end = opts[(lines - 2) * item_count + start];
				item_counts.push_back(end - start + 1);
				start = end + 1;
			}
			item_counts.push_back(item_count - start);
			return item_counts;
		}
	} // namespace Balance

	float MeasureWidth(Element* element, AvailableSpace available, Vector2f containing_block, Opt known_height)
	{
		return LayoutDetails::GetContentWidth(element, available, containing_block, known_height ? *known_height : -1.f);
	}

	float FlexAlgorithm::MeasureChildSize(const FlexItem& item, OptSize known, AvailableSpace available_width, AvailableSpace available_height,
		int axis) const
	{
		(void)available_height;
		if (known[axis])
			return *known[axis];

		const Vector2f item_containing_block = ItemContainingBlock();
		if (axis == 0)
			return MeasureWidth(item.element, available_width, item_containing_block, known[1]);

		const float width = known[0] ? *known[0] : MeasureWidth(item.element, available_width, item_containing_block, Opt());
		return LayoutDetails::GetContentHeight(item.element, width, item_containing_block);
	}

	// The automatic minimum size of a flex item in the main axis (CSS Flexbox §4.5), with Taffy's clamps: the min-content
	// size, clamped by the specified size and the (transferred) maximum, floored by the padding and border. Measured on
	// first use: callers only need it where the value they clamp is below its upper bound.
	float FlexAlgorithm::ResolvedMinimumMainSize(FlexItem& item)
	{
		if (item.resolved_minimum_known)
			return item.resolved_minimum_main_size;

		const int main = MainIndex();
		OptSize known;
		known[CrossIndex()] = item.measure_cross_known;
		AvailableSpace available[2];
		available[main] = AvailableSpace::MakeMinContent();
		available[CrossIndex()] =
			item.measure_cross_available_definite ? AvailableSpace::MakeDefinite(item.measure_cross_available) : AvailableSpace::MakeMaxContent();

		const float min_content_main_size = MeasureChildSize(item, known, available[0], available[1], main);
		const OptSize transferred_max_size = ApplyAspectRatio(item.max_size, item.aspect_ratio);
		float clamped = min_content_main_size;
		if (item.size[main])
			clamped = Math::Min(clamped, *item.size[main]);
		if (transferred_max_size[main])
			clamped = Math::Min(clamped, *transferred_max_size[main]);
		const float padding_border =
			(main == 0 ? item.padding.Horizontal() + item.border.Horizontal() : item.padding.Vertical() + item.border.Vertical());
		clamped = Math::Max(clamped, padding_border);

		item.resolved_minimum_known = true;
		item.resolved_minimum_main_size = clamped;
		return clamped;
	}

	void FlexAlgorithm::ComputeConstants(OptSize known, AvailableSpace available_width, AvailableSpace available_height, Vector2f scrollbar_gutter)
	{
		const ComputedValues& computed = element->GetComputedValues();
		const Style::FlexDirection direction = computed.flex_direction();
		row = (direction == Style::FlexDirection::Row || direction == Style::FlexDirection::RowReverse);
		reverse = (direction == Style::FlexDirection::RowReverse || direction == Style::FlexDirection::ColumnReverse);
		const Style::FlexWrap flex_wrap = computed.flex_wrap();
		is_wrap = (flex_wrap != Style::FlexWrap::Nowrap);
		is_wrap_reverse = (flex_wrap == Style::FlexWrap::WrapReverse || flex_wrap == Style::FlexWrap::BalanceReverse);
		is_balance = (flex_wrap == Style::FlexWrap::Balance || flex_wrap == Style::FlexWrap::BalanceReverse);
		line_count = (is_wrap ? Math::Max(computed.flex_line_count(), 1) : 1);

		container_border.v[Left] = box.GetEdge(BoxArea::Border, BoxEdge::Left);
		container_border.v[Right] = box.GetEdge(BoxArea::Border, BoxEdge::Right);
		container_border.v[Top] = box.GetEdge(BoxArea::Border, BoxEdge::Top);
		container_border.v[Bottom] = box.GetEdge(BoxArea::Border, BoxEdge::Bottom);
		content_box_inset = container_border;
		content_box_inset.v[Left] += box.GetEdge(BoxArea::Padding, BoxEdge::Left);
		content_box_inset.v[Right] += box.GetEdge(BoxArea::Padding, BoxEdge::Right) + scrollbar_gutter.x;
		content_box_inset.v[Top] += box.GetEdge(BoxArea::Padding, BoxEdge::Top);
		content_box_inset.v[Bottom] += box.GetEdge(BoxArea::Padding, BoxEdge::Bottom) + scrollbar_gutter.y;

		// The container's min/max sizes, border-box (sizing keywords of the container are resolved by its box).
		const Vector2f padding_border = {box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
			box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)};
		float min_width, max_width, min_height, max_height;
		LayoutDetails::GetMinMaxWidth(min_width, max_width, computed, box, containing_block.x);
		LayoutDetails::GetMinMaxHeight(min_height, max_height, computed, box, containing_block.y);
		if (LayoutDetails::IsMeasuringContentOf(element))
		{
			// Its content size alone while it is measured (LayoutDetails::GetContentWidth, GetContentHeight).
			min_width = min_height = 0.f;
			max_width = max_height = FLT_MAX;
		}
		container_min_size[0] = (min_width > 0.f ? Opt(min_width + padding_border.x) : Opt());
		container_min_size[1] = (min_height > 0.f ? Opt(min_height + padding_border.y) : Opt());
		container_max_size[0] = (max_width < FLT_MAX ? Opt(max_width + padding_border.x) : Opt());
		container_max_size[1] = (max_height < FLT_MAX ? Opt(max_height + padding_border.y) : Opt());

		// 'normal' behaves as 'stretch' for flex items and lines, and as 'flex-start' in the main axis.
		align_items = ToItemAlign(static_cast<Style::AlignSelf>(static_cast<int>(computed.align_items()) + 1));
		align_items_safe = (computed.align_items_overflow() == Style::OverflowAlignment::Safe);
		align_content = ToContentAlign(computed.align_content());
		align_content_safe = (computed.align_content_overflow() == Style::OverflowAlignment::Safe);
		justify_content = ToContentAlign(computed.justify_content(), row);
		justify_content_safe = (computed.justify_content_overflow() == Style::OverflowAlignment::Safe);

		node_outer_size = known;
		node_inner_size[0] = Sub(known[0], content_box_inset.Horizontal());
		node_inner_size[1] = Sub(known[1], content_box_inset.Vertical());
		// A known height that is not definite: Taffy's known_dimensions_are_definite.
		known_main_size_is_definite = (row || known_height_definite || !Main(known));
		has_definite_main_size = known_main_size_is_definite && Main(known).has_value();
		has_definite_cross_size = (!row || known_height_definite) && Cross(known).has_value();
		const AvailableSpace cross_available = (row ? available_height : available_width);
		cross_axis_available_space_is_definite = has_definite_cross_size || cross_available.IsDefinite();

		// Gaps: percentages of the inner size (indefinite: 0).
		const float inner_width = node_inner_size[0] ? *node_inner_size[0] : -1.f;
		const float inner_height = node_inner_size[1] ? *node_inner_size[1] : -1.f;
		gap.x = Math::Max(0.f, ResolveValueOr(computed.column_gap(), inner_width, 0.f));
		gap.y = Math::Max(0.f, ResolveValueOr(computed.row_gap(), inner_height, 0.f));

		container_size = Vector2f(0.f);
		inner_container_size = Vector2f(0.f);
	}

	void FlexAlgorithm::GenerateItems()
	{
		const ComputedValues& computed_flex = element->GetComputedValues();
		ScopedLayoutChildList children;
		LayoutDetails::GetLayoutChildren(element, *children, true);
		items.reserve(children->size());

		// Percentages of the items resolve against the container's inner size, if definite.
		OptSize percent_resolution_size = node_inner_size;
		if (!known_main_size_is_definite)
			percent_resolution_size[MainIndex()] = Opt();
		const float inner_width = node_inner_size[0] ? *node_inner_size[0] : -1.f;
		const Vector2f item_containing_block = ItemContainingBlock();

		for (size_t index = 0; index < children->size(); index++)
		{
			Element* child = (*children)[index];
			const ComputedValues& computed = child->GetComputedValues();

			// White space between flex items is not rendered (CSS Flexbox §4).
			if (computed.display() == Style::Display::None || LayoutDetails::IsCollapsibleWhiteSpaceText(child))
				continue;
			if (computed.position() == Style::Position::Absolute || computed.position() == Style::Position::Fixed)
			{
				absolute_children.push_back(child);
				absolute_orders.push_back(index);
				continue;
			}

			// A relatively positioned item is offset once the container is sized (ContainerBox::ClosePositionedElements).
			if (computed.position() == Style::Position::Relative || computed.position() == Style::Position::Sticky)
				container_box->AddRelativeElement(child);

			FlexItem item;
			item.element = child;
			item.order = index;
			LayoutDetails::BuildBox(item.box, item_containing_block, child, BuildBoxMode::UnalignedBlock);

			auto edge = [&](BoxArea area, BoxEdge box_edge) { return item.box.GetEdge(area, box_edge); };
			const BoxEdge box_edges[4] = {BoxEdge::Left, BoxEdge::Right, BoxEdge::Top, BoxEdge::Bottom};
			const Style::Margin margins[4] = {computed.margin_left(), computed.margin_right(), computed.margin_top(), computed.margin_bottom()};
			for (int i = 0; i < 4; i++)
			{
				item.padding.v[i] = edge(BoxArea::Padding, box_edges[i]);
				item.border.v[i] = edge(BoxArea::Border, box_edges[i]);
				item.margin_auto[i] = (margins[i].type == Style::Margin::Auto);
				// Percentage margins resolve against the container's inner width, in both axes.
				item.margin.v[i] = (item.margin_auto[i] ? 0.f : ResolveValue(margins[i], inner_width));
			}

			const Vector2f padding_border = {item.padding.Horizontal() + item.border.Horizontal(), item.padding.Vertical() + item.border.Vertical()};
			const Vector2f box_sizing_adjustment = (computed.box_sizing() == Style::BoxSizing::ContentBox ? padding_border : Vector2f(0.f));

			// The preferred aspect ratio: a replaced element's natural ratio unless 'aspect-ratio' gives one without 'auto'.
			Vector2f intrinsic_size(-1.f);
			float intrinsic_ratio = -1.f;
			const bool replaced = child->GetIntrinsicDimensions(intrinsic_size, intrinsic_ratio);
			item.aspect_ratio = computed.aspect_ratio();
			if (replaced && intrinsic_ratio > 0.f && (computed.aspect_ratio_auto() || item.aspect_ratio <= 0.f))
				item.aspect_ratio = intrinsic_ratio;

			auto resolve_size = [](Style::LengthPercentageAuto value, Opt basis) -> Opt {
				if (value.type == Style::LengthPercentageAuto::Auto)
					return Opt();
				if (value.type == Style::LengthPercentageAuto::Percentage && !basis)
					return Opt();
#ifdef RMLUI_MATH_EXPRESSIONS
				if (value.type == Style::LengthPercentageAuto::Calculation && !basis)
					return Opt();
#endif
				return ResolveValue(value, basis ? *basis : 0.f);
			};
			auto resolve_max = [](Style::LengthPercentage value, Opt basis) -> Opt {
				if (value.type == Style::LengthPercentage::Length && value.value >= FLT_MAX)
					return Opt();
				if (value.type != Style::LengthPercentage::Length && !basis)
					return Opt();
				return ResolveValue(value, basis ? *basis : 0.f);
			};

			OptSize size, min_size, max_size;
			size[0] = resolve_size(computed.width(), percent_resolution_size[0]);
			size[1] = resolve_size(computed.height(), percent_resolution_size[1]);
			min_size[0] = resolve_size(computed.min_width(), percent_resolution_size[0]);
			min_size[1] = resolve_size(computed.min_height(), percent_resolution_size[1]);
			max_size[0] = resolve_max(computed.max_width(), percent_resolution_size[0]);
			max_size[1] = resolve_max(computed.max_height(), percent_resolution_size[1]);
			size = ApplyAspectRatio(size, item.aspect_ratio);
			for (int axis = 0; axis < 2; axis++)
			{
				const float adjustment = (axis == 0 ? box_sizing_adjustment.x : box_sizing_adjustment.y);
				item.size[axis] = Add(size[axis], adjustment);
				item.min_size[axis] = Add(min_size[axis], adjustment);
				item.max_size[axis] = Add(max_size[axis], adjustment);
			}
			item.size_keyword[0] = computed.width_keyword();
			item.size_keyword[1] = computed.height_keyword();

			Style::AlignSelf align_self = computed.align_self();
			if (align_self == Style::AlignSelf::Auto)
			{
				item.align_self = align_items;
				item.align_safe = align_items_safe;
			}
			else
			{
				item.align_self = ToItemAlign(align_self);
				item.align_safe = (computed.align_self_overflow() == Style::OverflowAlignment::Safe);
			}
			(void)computed_flex;

			item.is_scroll_container = LayoutDetails::IsScrollContainer(computed.overflow_x(), computed.overflow_y());
			item.flex_grow = computed.flex_grow();
			item.flex_shrink = computed.flex_shrink();

			items.push_back(std::move(item));
		}
	}

	void FlexAlgorithm::DetermineFlexBaseSize(const AvailableSpace available[2])
	{
		const int main = MainIndex(), cross = CrossIndex();

		for (FlexItem& child : items)
		{
			const ComputedValues& computed = child.element->GetComputedValues();
			const Opt cross_axis_parent_size = Cross(node_inner_size);

			const OptSize transferred_min_size = ApplyAspectRatio(child.min_size, child.aspect_ratio);
			const OptSize transferred_max_size = ApplyAspectRatio(child.max_size, child.aspect_ratio);
			const Opt child_min_cross = transferred_min_size[cross];
			const Opt child_max_cross = transferred_max_size[cross];

			// The available space for measuring the item in the cross axis, clamped by its min and max cross sizes.
			AvailableSpace cross_axis_available_space = available[cross];
			if (cross_axis_available_space.IsDefinite())
			{
				const float value = DividedCrossSpace(cross_axis_parent_size ? *cross_axis_parent_size : cross_axis_available_space.value);
				cross_axis_available_space.value = Clamp(Math::Max(value - CrossSum(child.margin), 0.f), child_min_cross, child_max_cross);
			}
			else if (cross_axis_available_space.type == AvailableSpace::MinContent)
			{
				if (child_min_cross)
					cross_axis_available_space = AvailableSpace::MakeDefinite(*child_min_cross);
			}
			else if (child_max_cross)
				cross_axis_available_space = AvailableSpace::MakeDefinite(*child_max_cross);

			// The known cross size: the definite cross size, or the stretched one.
			bool child_cross_size_is_definite = child.size[cross].has_value();
			Opt known_cross = Clamp(child.size[cross], transferred_min_size[cross], transferred_max_size[cross]);
			if (child.align_self == ItemAlign::Stretch && !CrossStartAuto(child) && !CrossEndAuto(child) && !known_cross)
			{
				known_cross = (cross_axis_available_space.IsDefinite() ? Opt(cross_axis_available_space.value) : Opt());
				child_cross_size_is_definite = !is_wrap && has_definite_cross_size && cross_axis_parent_size.has_value();
			}
			child.measure_cross_known = known_cross;
			child.measure_cross_available_definite = cross_axis_available_space.IsDefinite();
			child.measure_cross_available = cross_axis_available_space.value;

			const float main_padding_border = MainSum(child.padding) + MainSum(child.border);
			const float box_sizing_adjustment = (computed.box_sizing() == Style::BoxSizing::ContentBox ? main_padding_border : 0.f);
			const Opt percent_resolution_main_size = (known_main_size_is_definite ? Main(node_inner_size) : Opt());

			// The flex basis, or the main size property when it is 'auto'.
			const Style::FlexBasis flex_basis_style = computed.flex_basis();
			const Style::SizingKeyword flex_basis_keyword = computed.flex_basis_keyword();
			Opt flex_basis;
			if (flex_basis_style.type != Style::FlexBasis::Auto)
			{
				const bool indefinite_percentage = (flex_basis_style.type != Style::FlexBasis::Length && !percent_resolution_main_size);
				if (!indefinite_percentage)
					flex_basis =
						ResolveValue(flex_basis_style, percent_resolution_main_size ? *percent_resolution_main_size : 0.f) + box_sizing_adjustment;
			}

			bool measured_max_content = false;
			child.flex_basis = [&]() -> float {
				// A sizing keyword in place of the main size: 'stretch' an exact size, the others the measuring constraint.
				const Opt main_stretch_size = Max(Sub(percent_resolution_main_size, MainSum(child.margin)), Opt(0.f));
				std::optional<AvailableSpace> keyword_main_available_space;
				auto resolve_keyword = [&](Style::SizingKeyword keyword, bool fit_content_length, float& out_exact) -> int {
					switch (keyword)
					{
					case Style::SizingKeyword::MinContent: keyword_main_available_space = AvailableSpace::MakeMinContent(); return 1;
					case Style::SizingKeyword::MaxContent: keyword_main_available_space = AvailableSpace::MakeMaxContent(); return 1;
					case Style::SizingKeyword::FitContent:
						if (main_stretch_size)
						{
							keyword_main_available_space = AvailableSpace::MakeDefinite(*main_stretch_size);
							return 1;
						}
						return 0;
					case Style::SizingKeyword::FitContentLength:
					{
						if (!fit_content_length)
							return 0;
						const Style::LengthPercentage argument = (main == 0 ? computed.fit_content_width() : computed.fit_content_height());
						if (argument.type == Style::LengthPercentage::Percentage && !percent_resolution_main_size)
							return 0;
						keyword_main_available_space =
							AvailableSpace::MakeDefinite(ResolveValue(argument, percent_resolution_main_size ? *percent_resolution_main_size : 0.f));
						return 1;
					}
					case Style::SizingKeyword::Stretch:
						if (main_stretch_size)
						{
							out_exact = *main_stretch_size;
							return 2;
						}
						return 0;
					default: break;
					}
					return 0;
				};

				float exact = 0.f;
				if (flex_basis_keyword == Style::SizingKeyword::Content)
				{
					// 'content': measured, ignoring the main size property.
				}
				else if (flex_basis_keyword != Style::SizingKeyword::None)
				{
					if (resolve_keyword(flex_basis_keyword, false, exact) == 2)
					{
						child.flex_basis_is_definite = true;
						return exact;
					}
				}
				else
				{
					if (const Opt basis = (flex_basis ? flex_basis : child.size[main]))
					{
						child.flex_basis_is_definite = true;
						return *basis;
					}
					if (resolve_keyword(child.size_keyword[main], true, exact) == 2)
					{
						child.flex_basis_is_definite = true;
						return exact;
					}
				}

				// An aspect ratio and a definite cross size give the flex base size (case B).
				if (child_cross_size_is_definite && child.aspect_ratio > 0.f && known_cross)
				{
					child.flex_basis_is_definite = true;
					return row ? *known_cross * child.aspect_ratio : *known_cross / child.aspect_ratio;
				}

				// Otherwise the item is sized into the available space with its flex basis as its main size (case E).
				AvailableSpace child_available[2];
				child_available[main] = keyword_main_available_space
					? *keyword_main_available_space
					: (available[main].type == AvailableSpace::MinContent ? AvailableSpace::MakeMinContent() : AvailableSpace::MakeMaxContent());
				child_available[cross] = cross_axis_available_space;
				measured_max_content = (child_available[main].type == AvailableSpace::MaxContent);
				OptSize known;
				known[cross] = known_cross;
				return MeasureChildSize(child, known, child_available[0], child_available[1], main);
			}();

			// The flex basis is floored by the padding and border (as in browsers).
			child.flex_basis = Math::Max(child.flex_basis, main_padding_border);
			child.inner_flex_basis = child.flex_basis - main_padding_border;

			// The automatic minimum size: a specified min size, 0 for a scroll container, otherwise measured when needed.
			if (child.min_size[main])
			{
				child.resolved_minimum_known = true;
				child.resolved_minimum_main_size = *child.min_size[main];
			}
			else if (child.is_scroll_container)
			{
				child.resolved_minimum_known = true;
				child.resolved_minimum_main_size = 0.f;
			}
			else
			{
				// The min-content size is at most the max-content size measured for the flex basis, and the resolved
				// minimum at most the specified and maximum sizes.
				float upper_bound = FLT_MAX;
				if (measured_max_content)
					upper_bound = child.flex_basis;
				if (child.size[main])
					upper_bound = Math::Min(upper_bound, *child.size[main]);
				if (transferred_max_size[main])
					upper_bound = Math::Min(upper_bound, *transferred_max_size[main]);
				child.resolved_minimum_upper_bound = Math::Max(upper_bound, main_padding_border);
			}

			// The hypothetical main size: the flex base size clamped by the min and max main sizes. Sizes transferred
			// through the aspect ratio clamp it too.
			float hypothetical_inner_min_main = Math::Max(main_padding_border, transferred_min_size[main] ? *transferred_min_size[main] : 0.f);
			if (child.flex_basis < child.resolved_minimum_upper_bound || child.resolved_minimum_known)
				hypothetical_inner_min_main = Math::Max(hypothetical_inner_min_main, ResolvedMinimumMainSize(child));
			const float hypothetical_inner_size = Clamp(child.flex_basis, Opt(hypothetical_inner_min_main), transferred_max_size[main]);
			SetMain(child.hypothetical_inner_size, hypothetical_inner_size);
			SetMain(child.hypothetical_outer_size, hypothetical_inner_size + MainSum(child.margin));
		}
	}

	AvailableSpace FlexAlgorithm::LineBreakAvailableSpace(const AvailableSpace available[2]) const
	{
		const int main = MainIndex();
		if (const Opt max_size = Main(container_max_size))
		{
			float value = (available[main].IsDefinite() ? available[main].value : *max_size);
			if (!has_definite_main_size)
				value = Math::Min(value, *max_size);
			if (const Opt min_size = Main(container_min_size))
				value = Math::Max(value, *min_size);
			return AvailableSpace::MakeDefinite(value);
		}
		// A column container's automatic height is content-based: definite available space does not make lines wrap.
		if (!row && !has_definite_main_size && available[main].IsDefinite())
			return AvailableSpace::MakeMaxContent();
		return available[main];
	}

	void FlexAlgorithm::CollectFlexLines(const AvailableSpace available[2])
	{
		lines.clear();
		auto single_line = [&]() { lines.push_back(FlexLine{0, items.size()}); };

		if (!is_wrap || !known_main_size_is_definite)
		{
			single_line();
			return;
		}

		const AvailableSpace main_axis_available_space = LineBreakAvailableSpace(available);
		if (main_axis_available_space.type == AvailableSpace::MaxContent)
		{
			single_line();
		}
		else if (main_axis_available_space.type == AvailableSpace::MinContent)
		{
			for (size_t i = 0; i < items.size(); i++)
				lines.push_back(FlexLine{i, 1});
		}
		else
		{
			const float main_axis_gap = Main(gap);
			size_t start = 0;
			while (start < items.size())
			{
				float line_length = 0.f;
				size_t end = start;
				for (; end < items.size(); end++)
				{
					const float gap_contribution = (end == start ? 0.f : main_axis_gap);
					line_length += Main(items[end].hypothetical_outer_size) + gap_contribution;
					if (line_length > main_axis_available_space.value && end != start)
						break;
				}
				lines.push_back(FlexLine{start, end - start});
				start = end;
			}
		}
		if (lines.empty())
			single_line();
	}

	void FlexAlgorithm::CollectBalancedFlexLines(const AvailableSpace available[2])
	{
		lines.clear();
		if (items.empty())
		{
			lines.push_back(FlexLine{0, 0});
			return;
		}

		const AvailableSpace main_axis_available_space =
			(known_main_size_is_definite ? LineBreakAvailableSpace(available) : AvailableSpace::MakeMaxContent());
		if (main_axis_available_space.type == AvailableSpace::MinContent)
		{
			for (size_t i = 0; i < items.size(); i++)
				lines.push_back(FlexLine{i, 1});
			return;
		}

		const float line_break_size = (main_axis_available_space.IsDefinite() ? main_axis_available_space.value : HUGE_VALF);
		Vector<float> item_sizes;
		item_sizes.reserve(items.size());
		for (const FlexItem& item : items)
			item_sizes.push_back(Main(item.hypothetical_outer_size));
		const Vector<size_t> item_counts = Balance::BalancedLineItemCounts(item_sizes, line_break_size, Main(gap), size_t(line_count));

		size_t start = 0;
		for (size_t count : item_counts)
		{
			lines.push_back(FlexLine{start, count});
			start += count;
		}
	}

	void FlexAlgorithm::DetermineContainerMainSize(const AvailableSpace available[2])
	{
		const int main = MainIndex(), cross = CrossIndex();
		const float main_content_box_inset = MainSum(content_box_inset);

		float outer_main_size = 0.f;
		if (const Opt known_main = Main(node_outer_size))
		{
			outer_main_size = *known_main;
		}
		else if (available[main].IsDefinite())
		{
			const float main_axis_gap = Main(gap);
			auto item_main_length = [&](const FlexItem& child) {
				const float padding_border_sum = MainSum(child.padding) + MainSum(child.border);
				return Math::Max(*Max(Opt(child.flex_basis), child.min_size[main]) + MainSum(child.margin), padding_border_sum);
			};
			float longest_line_length = 0.f;
			for (const FlexLine& line : lines)
			{
				float total = SumAxisGaps(main_axis_gap, line.count);
				for (size_t i = line.first; i < line.first + line.count; i++)
					total += item_main_length(items[i]);
				longest_line_length = Math::Max(longest_line_length, total);
			}
			const float size = longest_line_length + main_content_box_inset;

			if (is_balance && !items.empty())
			{
				// fit-content sizing of a balanced container: its max-content size is the longest line when the items are
				// balanced across the minimum line count without a size limit.
				Vector<float> item_lengths;
				item_lengths.reserve(items.size());
				for (const FlexItem& child : items)
					item_lengths.push_back(item_main_length(child));
				const Vector<size_t> item_counts = Balance::BalancedLineItemCounts(item_lengths, HUGE_VALF, main_axis_gap, size_t(line_count));
				float widest_line_length = 0.f;
				size_t index = 0;
				for (size_t count : item_counts)
				{
					float line_length = SumAxisGaps(main_axis_gap, count);
					for (size_t i = index; i < index + count; i++)
						line_length += item_lengths[i];
					widest_line_length = Math::Max(widest_line_length, line_length);
					index += count;
				}
				const float max_content_size = widest_line_length + main_content_box_inset;
				outer_main_size = Math::Max(size, Math::Min(max_content_size, available[main].value));
			}
			else
				outer_main_size = (lines.size() > 1 ? Math::Max(size, available[main].value) : size);
		}
		else if (available[main].type == AvailableSpace::MinContent && is_wrap)
		{
			float longest_line_length = 0.f;
			for (const FlexLine& line : lines)
			{
				float total = SumAxisGaps(Main(gap), line.count);
				for (size_t i = line.first; i < line.first + line.count; i++)
				{
					const FlexItem& child = items[i];
					const float padding_border_sum = MainSum(child.padding) + MainSum(child.border);
					total += Math::Max(*Max(Opt(child.flex_basis), child.min_size[main]) + MainSum(child.margin), padding_border_sum);
				}
				longest_line_length = Math::Max(longest_line_length, total);
			}
			outer_main_size = longest_line_length + main_content_box_inset;
		}
		else
		{
			// The intrinsic main size: the largest sum of the items' content contributions within a line.
			float main_size = 0.f;
			for (const FlexLine& line : lines)
			{
				for (size_t i = line.first; i < line.first + line.count; i++)
				{
					FlexItem& item = items[i];
					const Opt style_min = item.min_size[main];
					const Opt style_preferred = item.size[main];
					const Opt style_max = item.max_size[main];

					const Opt clamping_basis = (row ? Max(Opt(item.flex_basis), style_preferred) : Opt(item.flex_basis));
					const Opt flex_basis_min = (item.flex_shrink == 0.f ? clamping_basis : Opt());
					const Opt flex_basis_max = (item.flex_grow == 0.f ? clamping_basis : Opt());

					const Opt max_main_opt = (style_max ? Min(style_max, flex_basis_max) : flex_basis_max);
					const float max_main_size = (max_main_opt ? *max_main_opt : HUGE_VALF);
					// The resolved minimum is only needed to compare it with a finite maximum.
					float min_main_size = 0.f;
					if (max_main_size < HUGE_VALF)
					{
						const float resolved_min = ResolvedMinimumMainSize(item);
						const Opt min_opt = (style_min ? Max(style_min, flex_basis_min) : flex_basis_min);
						min_main_size = Math::Max(min_opt ? *min_opt : resolved_min, resolved_min);
					}

					const float margin_sum = MainSum(item.margin);
					float content_contribution = 0.f;
					if (style_preferred && (max_main_size <= min_main_size || max_main_size <= *style_preferred))
						content_contribution = Math::Max(Math::Min(*style_preferred, max_main_size), min_main_size) + margin_sum;
					else if (max_main_size <= min_main_size)
						content_contribution = min_main_size + margin_sum;
					else if (item.is_scroll_container)
						content_contribution = item.flex_basis + margin_sum;
					else if (style_preferred)
					{
						const float item_pb_main = MainSum(item.padding) + MainSum(item.border);
						const float inner_main_size = Math::Max(*style_preferred, item_pb_main);
						if (row)
							content_contribution = Clamp(inner_main_size + margin_sum, style_min, style_max);
						else
							content_contribution = Clamp(Math::Max(inner_main_size, item.flex_basis) + margin_sum, style_min, style_max);
					}
					else
					{
						const Opt cross_axis_parent_size = Cross(node_inner_size);
						AvailableSpace cross_axis_available_space = available[cross];
						if (cross_axis_available_space.IsDefinite())
						{
							const float value =
								DividedCrossSpace(cross_axis_parent_size ? *cross_axis_parent_size : cross_axis_available_space.value);
							cross_axis_available_space.value =
								Clamp(Math::Max(value - CrossSum(item.margin), 0.f), item.min_size[cross], item.max_size[cross]);
						}
						AvailableSpace child_available[2];
						child_available[main] = available[main];
						child_available[cross] = cross_axis_available_space;

						const OptSize transferred_min_size = ApplyAspectRatio(item.min_size, item.aspect_ratio);
						const OptSize transferred_max_size = ApplyAspectRatio(item.max_size, item.aspect_ratio);
						OptSize known;
						known[cross] = Clamp(item.size[cross], transferred_min_size[cross], transferred_max_size[cross]);
						if (item.align_self == ItemAlign::Stretch && !known[cross] && cross_axis_available_space.IsDefinite())
							known[cross] = cross_axis_available_space.value;

						const float measured_main_size = MeasureChildSize(item, known, child_available[0], child_available[1], main);
						float inner_main_size = measured_main_size;
						if (item.aspect_ratio > 0.f && known[cross])
							inner_main_size = Math::Max(inner_main_size, row ? *known[cross] * item.aspect_ratio : *known[cross] / item.aspect_ratio);

						if (row)
							content_contribution = Clamp(inner_main_size + margin_sum, style_min, style_max);
						else
							content_contribution = Clamp(Math::Max(inner_main_size, item.flex_basis) + margin_sum, style_min, style_max);
					}

					const float diff = content_contribution - item.flex_basis;
					if (diff > 0.f)
						item.content_flex_fraction = diff / Math::Max(1.f, item.flex_grow);
					else if (diff < 0.f)
					{
						const float scaled_shrink_factor = Math::Max(1.f, item.flex_shrink) * item.inner_flex_basis;
						item.content_flex_fraction = (scaled_shrink_factor != 0.f ? diff / scaled_shrink_factor : 0.f);
					}
					else
						item.content_flex_fraction = 0.f;
				}

				float item_main_size_sum = 0.f;
				for (size_t i = line.first; i < line.first + line.count; i++)
				{
					FlexItem& item = items[i];
					const float flex_fraction = item.content_flex_fraction;
					float flex_contribution = 0.f;
					if (flex_fraction > 0.f)
						flex_contribution = Math::Max(1.f, item.flex_grow) * flex_fraction;
					else if (flex_fraction < 0.f)
					{
						const float scaled_shrink_factor = Math::Max(1.f, item.flex_shrink) * item.inner_flex_basis;
						flex_contribution = (scaled_shrink_factor == 0.f ? 0.f : scaled_shrink_factor * flex_fraction);
					}
					const float size = item.flex_basis + flex_contribution;
					SetMain(item.outer_target_size, size);
					SetMain(item.target_size, size);
					item_main_size_sum += size;
				}
				main_size = Math::Max(main_size, item_main_size_sum + SumAxisGaps(Main(gap), line.count));
			}
			outer_main_size = main_size + main_content_box_inset;
		}

		const float main_scrollbar_gutter =
			(row ? content_box_inset.v[Right] - container_border.v[Right] - box.GetEdge(BoxArea::Padding, BoxEdge::Right)
				 : content_box_inset.v[Bottom] - container_border.v[Bottom] - box.GetEdge(BoxArea::Padding, BoxEdge::Bottom));
		outer_main_size =
			Math::Max(Clamp(outer_main_size, Main(container_min_size), Main(container_max_size)), main_content_box_inset - main_scrollbar_gutter);
		const float inner_main_size = Math::Max(outer_main_size - main_content_box_inset, 0.f);
		SetMain(container_size, outer_main_size);
		SetMain(inner_container_size, inner_main_size);
		node_inner_size[main] = inner_main_size;
		node_outer_size[main] = outer_main_size;
	}

	void FlexAlgorithm::ResolveFlexibleLengths(FlexLine& line)
	{
		const int main = MainIndex();
		FlexItem* begin = items.data() + line.first;
		FlexItem* end = begin + line.count;
		const float total_main_axis_gap = SumAxisGaps(Main(gap), line.count);

		// 1. The used flex factor.
		float total_hypothetical_outer_main_size = 0.f;
		for (FlexItem* child = begin; child != end; child++)
			total_hypothetical_outer_main_size += Main(child->hypothetical_outer_size);
		const float used_flex_factor = total_main_axis_gap + total_hypothetical_outer_main_size;
		const float node_inner_main = (Main(node_inner_size) ? *Main(node_inner_size) : 0.f);
		const bool growing = used_flex_factor < node_inner_main;
		const bool shrinking = used_flex_factor > node_inner_main;
		const bool exactly_sized = !growing && !shrinking;

		// 2. Size inflexible items.
		for (FlexItem* child = begin; child != end; child++)
		{
			const float inner_target_size = Main(child->hypothetical_inner_size);
			SetMain(child->target_size, inner_target_size);
			child->frozen = false;
			if (exactly_sized || (child->flex_grow == 0.f && child->flex_shrink == 0.f) ||
				(growing && child->flex_basis > Main(child->hypothetical_inner_size)) ||
				(shrinking && child->flex_basis < Main(child->hypothetical_inner_size)))
			{
				child->frozen = true;
				SetMain(child->outer_target_size, inner_target_size + MainSum(child->margin));
			}
		}

		if (exactly_sized)
			return;

		// 3. The initial free space.
		auto used_space = [&]() {
			float space = total_main_axis_gap;
			for (FlexItem* child = begin; child != end; child++)
				space += (child->frozen ? Main(child->outer_target_size) : child->flex_basis + MainSum(child->margin));
			return space;
		};
		const Opt node_inner_main_opt = Main(node_inner_size);
		const float initial_free_space = node_inner_main_opt ? *node_inner_main_opt - used_space() : 0.f;

		// 4. Loop until every item is frozen.
		for (int iteration = 0; iteration < 1000; iteration++)
		{
			bool all_frozen = true;
			for (FlexItem* child = begin; child != end; child++)
				all_frozen &= child->frozen;
			if (all_frozen)
				break;

			const float space = used_space();
			float sum_flex_grow = 0.f, sum_flex_shrink = 0.f;
			for (FlexItem* child = begin; child != end; child++)
			{
				if (!child->frozen)
				{
					sum_flex_grow += child->flex_grow;
					sum_flex_shrink += child->flex_shrink;
				}
			}

			float free_space = 0.f;
			if (growing && sum_flex_grow < 1.f)
			{
				free_space = initial_free_space * sum_flex_grow - total_main_axis_gap;
				if (node_inner_main_opt)
					free_space = Math::Min(free_space, *node_inner_main_opt - space);
			}
			else if (shrinking && sum_flex_shrink < 1.f)
			{
				free_space = initial_free_space * sum_flex_shrink - total_main_axis_gap;
				if (node_inner_main_opt)
					free_space = Math::Max(free_space, *node_inner_main_opt - space);
			}
			else
				free_space = (node_inner_main_opt ? *node_inner_main_opt - space : used_flex_factor - space);

			// c. Distribute the free space proportional to the flex factors.
			if (std::isnormal(free_space))
			{
				if (growing && sum_flex_grow > 0.f)
				{
					for (FlexItem* child = begin; child != end; child++)
					{
						if (!child->frozen)
							SetMain(child->target_size, child->flex_basis + free_space * (child->flex_grow / sum_flex_grow));
					}
				}
				else if (shrinking && sum_flex_shrink > 0.f)
				{
					float sum_scaled_shrink_factor = 0.f;
					for (FlexItem* child = begin; child != end; child++)
					{
						if (!child->frozen)
							sum_scaled_shrink_factor += child->inner_flex_basis * child->flex_shrink;
					}
					if (sum_scaled_shrink_factor > 0.f)
					{
						for (FlexItem* child = begin; child != end; child++)
						{
							if (!child->frozen)
								SetMain(child->target_size,
									child->flex_basis + free_space * (child->inner_flex_basis * child->flex_shrink / sum_scaled_shrink_factor));
						}
					}
				}
			}

			// d. Fix min/max violations.
			float total_violation = 0.f;
			for (FlexItem* child = begin; child != end; child++)
			{
				if (child->frozen)
					continue;
				const float target = Main(child->target_size);
				Opt resolved_min_main;
				if (child->resolved_minimum_known || target < child->resolved_minimum_upper_bound)
					resolved_min_main = ResolvedMinimumMainSize(*child);
				const float clamped = Math::Max(Clamp(target, resolved_min_main, child->max_size[main]), 0.f);
				child->violation = clamped - target;
				SetMain(child->target_size, clamped);
				SetMain(child->outer_target_size, clamped + MainSum(child->margin));
				total_violation += child->violation;
			}

			// e. Freeze over-flexed items.
			for (FlexItem* child = begin; child != end; child++)
			{
				if (child->frozen)
					continue;
				if (total_violation > 0.f)
					child->frozen = (child->violation > 0.f);
				else if (total_violation < 0.f)
					child->frozen = (child->violation < 0.f);
				else
					child->frozen = true;
			}
		}
	}

	void FlexAlgorithm::DetermineHypotheticalCrossSize(FlexLine& line, const AvailableSpace available[2])
	{
		const int cross = CrossIndex();
		for (size_t i = line.first; i < line.first + line.count; i++)
		{
			FlexItem& child = items[i];
			const float padding_border_sum = CrossSum(child.padding) + CrossSum(child.border);
			const AvailableSpace child_available_main = AvailableSpace::MakeDefinite(Math::Max(Main(container_size) - MainSum(child.margin), 0.f));

			const OptSize transferred_min_size = ApplyAspectRatio(child.min_size, child.aspect_ratio);
			const OptSize transferred_max_size = ApplyAspectRatio(child.max_size, child.aspect_ratio);
			const Opt transferred_min_cross = transferred_min_size[cross];
			const Opt transferred_max_cross = transferred_max_size[cross];

			const Opt child_cross = Max(Clamp(child.size[cross], transferred_min_cross, transferred_max_cross), Opt(padding_border_sum));

			AvailableSpace child_available_cross = available[cross];
			if (child_available_cross.IsDefinite())
				child_available_cross.value = Math::Max(Clamp(Math::Max(DividedCrossSpace(child_available_cross.value) - CrossSum(child.margin), 0.f),
															transferred_min_cross, transferred_max_cross),
					padding_border_sum);

			// A sizing keyword as the cross size gives the measuring constraint ('stretch' stretches to the line, later).
			const ComputedValues& computed = child.element->GetComputedValues();
			const Opt cross_stretch_size =
				Max(Sub(Cross(node_inner_size) ? Opt(DividedCrossSpace(*Cross(node_inner_size))) : Opt(), CrossSum(child.margin)), Opt(0.f));
			switch (child.size_keyword[cross])
			{
			case Style::SizingKeyword::MinContent: child_available_cross = AvailableSpace::MakeMinContent(); break;
			case Style::SizingKeyword::MaxContent: child_available_cross = AvailableSpace::MakeMaxContent(); break;
			case Style::SizingKeyword::FitContent:
				if (cross_stretch_size)
					child_available_cross = AvailableSpace::MakeDefinite(*cross_stretch_size);
				break;
			case Style::SizingKeyword::FitContentLength:
			{
				const Style::LengthPercentage argument = (cross == 0 ? computed.fit_content_width() : computed.fit_content_height());
				const Opt basis = Cross(node_inner_size);
				if (argument.type != Style::LengthPercentage::Percentage || basis)
					child_available_cross = AvailableSpace::MakeDefinite(ResolveValue(argument, basis ? *basis : 0.f));
			}
			break;
			default: break;
			}

			// An item stretched in a single-line container of definite cross size gets the line's cross size, the container's
			// (9.4 8., 11.): its hypothetical cross size is not used, so it is not measured.
			const bool stretched_to_definite_line = !is_wrap && has_definite_cross_size && cross_stretch_size && !CrossStartAuto(child) &&
				!CrossEndAuto(child) && child.align_self == ItemAlign::Stretch && !child.size[cross] &&
				(child.size_keyword[cross] == Style::SizingKeyword::None || child.size_keyword[cross] == Style::SizingKeyword::Stretch);

			float child_inner_cross = 0.f;
			if (child_cross)
				child_inner_cross = *child_cross;
			else if (stretched_to_definite_line)
				child_inner_cross = Math::Max(Clamp(*cross_stretch_size, transferred_min_cross, transferred_max_cross), padding_border_sum);
			else
			{
				OptSize known;
				known[MainIndex()] = Main(child.target_size);
				const AvailableSpace available_width = (row ? child_available_main : child_available_cross);
				const AvailableSpace available_height = (row ? child_available_cross : child_available_main);
				child_inner_cross = Math::Max(
					Clamp(MeasureChildSize(child, known, available_width, available_height, cross), transferred_min_cross, transferred_max_cross),
					padding_border_sum);
			}
			SetCross(child.hypothetical_inner_size, child_inner_cross);
			SetCross(child.hypothetical_outer_size, child_inner_cross + CrossSum(child.margin));
		}
	}

	// The first baseline of a box formatted at a size, from its border-box top (its border-box height if it has none).
	float FormatForBaseline(Element* element, const Box& item_box, Vector2f size, Vector2f containing_block, bool last)
	{
		Box box = item_box;
		box.SetContent(Math::Max(size -
				Vector2f(box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
					box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)),
			Vector2f(0.f)));
		RootBox root(Math::Max(containing_block, Vector2f(0.f)));
		UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block);
		float baseline = 0.f;
		if (layout_box && (last ? layout_box->GetBaselineOfLastLine(baseline) : layout_box->GetBaselineOfFirstLine(baseline)))
			return baseline;
		return size.y;
	}

	void FlexAlgorithm::CalculateChildrenBaselines(const AvailableSpace available[2])
	{
		(void)available;
		// Baselines are only aligned in rows (the cross axis is then the inline axis's block axis).
		if (!row)
			return;

		for (FlexLine& line : lines)
		{
			size_t baseline_child_count = 0;
			for (size_t i = line.first; i < line.first + line.count; i++)
				baseline_child_count += ParticipatesInBaselineAlignment(items[i]) ? 1 : 0;
			if (baseline_child_count <= 1)
				continue;

			for (size_t i = line.first; i < line.first + line.count; i++)
			{
				FlexItem& child = items[i];
				if (!ParticipatesInBaselineAlignment(child))
					continue;
				const Vector2f size = {child.target_size.x, child.hypothetical_inner_size.y};
				const LayoutDetails::IndefiniteHeightScope height_scope(child.element, !ItemHeightIsDefinite(child));
				float baseline =
					FormatForBaseline(child.element, child.box, size, ItemContainingBlock(), child.align_self == ItemAlign::LastBaseline);
				if (child.is_scroll_container)
					baseline = Math::Clamp(baseline, 0.f, size.y);
				// A last-baseline item aligns its last baseline, measured from its bottom margin edge (the group is end-aligned).
				if (child.align_self == ItemAlign::LastBaseline)
					child.baseline = size.y - baseline + child.margin.v[Bottom];
				else
					child.baseline = baseline + child.margin.v[Top];
			}
		}
	}

	void FlexAlgorithm::CalculateCrossSize(OptSize known)
	{
		const float cross_axis_padding_border = CrossSum(content_box_inset);
		if (!is_wrap && Cross(known))
		{
			lines[0].cross_size =
				Math::Max(*Clamp(Cross(known), Cross(container_min_size), Cross(container_max_size)) - cross_axis_padding_border, 0.f);
			return;
		}

		for (FlexLine& line : lines)
		{
			float max_baseline = 0.f, max_last_baseline = 0.f;
			for (size_t i = line.first; i < line.first + line.count; i++)
			{
				if (!ParticipatesInBaselineAlignment(items[i]))
					continue;
				if (items[i].align_self == ItemAlign::LastBaseline)
					max_last_baseline = Math::Max(max_last_baseline, items[i].baseline);
				else
					max_baseline = Math::Max(max_baseline, items[i].baseline);
			}
			float cross_size = 0.f;
			for (size_t i = line.first; i < line.first + line.count; i++)
			{
				const FlexItem& child = items[i];
				float value = Cross(child.hypothetical_outer_size);
				if (ParticipatesInBaselineAlignment(child))
				{
					if (child.align_self == ItemAlign::LastBaseline)
						value = max_last_baseline - child.baseline + value;
					else
						value = max_baseline - child.baseline + value;
				}
				cross_size = Math::Max(cross_size, value);
			}
			line.cross_size = cross_size;
		}

		if (!is_wrap)
			lines[0].cross_size = Clamp(lines[0].cross_size, Sub(Cross(container_min_size), cross_axis_padding_border),
				Sub(Cross(container_max_size), cross_axis_padding_border));
	}

	void FlexAlgorithm::HandleAlignContentStretch(OptSize known)
	{
		if (align_content != ContentAlign::Stretch)
			return;
		const float cross_axis_padding_border = CrossSum(content_box_inset);
		const Opt cross_min = Cross(container_min_size), cross_max = Cross(container_max_size);
		const Opt container_cross = Cross(known) ? Cross(known) : cross_min;
		const float container_min_inner_cross =
			container_cross ? Math::Max(*Clamp(container_cross, cross_min, cross_max) - cross_axis_padding_border, 0.f) : 0.f;

		float lines_total_cross = SumAxisGaps(Cross(gap), lines.size());
		for (const FlexLine& line : lines)
			lines_total_cross += line.cross_size;

		if (lines_total_cross < container_min_inner_cross)
		{
			const float addition = (container_min_inner_cross - lines_total_cross) / float(lines.size());
			for (FlexLine& line : lines)
				line.cross_size += addition;
		}
	}

	void FlexAlgorithm::DetermineUsedCrossSize()
	{
		const int cross = CrossIndex();
		for (FlexLine& line : lines)
		{
			for (size_t i = line.first; i < line.first + line.count; i++)
			{
				FlexItem& child = items[i];
				const ComputedValues& computed = child.element->GetComputedValues();
				const bool cross_is_auto = (cross == 0 ? computed.width().type == Style::LengthPercentageAuto::Auto
													   : computed.height().type == Style::LengthPercentageAuto::Auto);
				float target_cross;
				if (!CrossStartAuto(child) && !CrossEndAuto(child) &&
					(child.size_keyword[cross] == Style::SizingKeyword::Stretch ||
						(child.align_self == ItemAlign::Stretch && cross_is_auto && child.size_keyword[cross] == Style::SizingKeyword::None)))
				{
					// The max size is not transferred through the aspect ratio here (as in browsers).
					target_cross = Clamp(Math::Max(line.cross_size - CrossSum(child.margin), 0.f), child.min_size[cross], child.max_size[cross]);
				}
				else
					target_cross = Cross(child.hypothetical_inner_size);
				SetCross(child.target_size, target_cross);
				SetCross(child.outer_target_size, target_cross + CrossSum(child.margin));
			}
		}
	}

	void FlexAlgorithm::DistributeRemainingFreeSpace()
	{
		for (FlexLine& line : lines)
		{
			const float total_main_axis_gap = SumAxisGaps(Main(gap), line.count);
			float used_space = total_main_axis_gap;
			int num_auto_margins = 0;
			for (size_t i = line.first; i < line.first + line.count; i++)
			{
				used_space += Main(items[i].outer_target_size);
				num_auto_margins += int(MainStartAuto(items[i])) + int(MainEndAuto(items[i]));
			}
			float free_space = Main(inner_container_size) - used_space;

			if (free_space > 0.f && num_auto_margins > 0)
			{
				const float margin = free_space / float(num_auto_margins);
				for (size_t i = line.first; i < line.first + line.count; i++)
				{
					FlexItem& child = items[i];
					if (MainStartAuto(child))
						child.margin.v[row ? Left : Top] = margin;
					if (MainEndAuto(child))
						child.margin.v[row ? Right : Bottom] = margin;
				}
				free_space = 0.f;
			}

			const ContentAlign mode = ApplyAlignmentFallback(free_space, line.count, justify_content, justify_content_safe);
			for (size_t n = 0; n < line.count; n++)
			{
				// In reverse, the offsets are computed from the last item.
				const size_t i = (reverse ? line.first + line.count - 1 - n : line.first + n);
				items[i].offset_main = ComputeAlignmentOffset(free_space, line.count, Main(gap), mode, reverse, n == 0);
			}
		}
	}

	float FlexAlgorithm::AlignFlexItemAlongCrossAxis(const FlexItem& child, float free_space, float max_baseline, float max_baseline_to_bottom) const
	{
		// 'safe' alignment of an overflowing item falls back to 'start'.
		ItemAlign align = child.align_self;
		if (child.align_safe && free_space < 0.f)
			align = ItemAlign::Start;

		switch (align)
		{
		case ItemAlign::Start: return 0.f;
		case ItemAlign::FlexStart: return is_wrap_reverse ? free_space : 0.f;
		case ItemAlign::End: return free_space;
		case ItemAlign::FlexEnd: return is_wrap_reverse ? 0.f : free_space;
		case ItemAlign::Center: return free_space / 2.f;
		case ItemAlign::Baseline:
			if (row)
			{
				if (is_wrap_reverse)
				{
					const float line_cross_size = free_space + Cross(child.outer_target_size);
					return line_cross_size - max_baseline_to_bottom - child.baseline;
				}
				return max_baseline - child.baseline;
			}
			return is_wrap_reverse ? free_space : 0.f;
		case ItemAlign::LastBaseline:
			if (row)
				return free_space - (max_baseline_to_bottom - child.baseline);
			return is_wrap_reverse ? 0.f : free_space;
		case ItemAlign::Stretch: return is_wrap_reverse ? free_space : 0.f;
		}
		return 0.f;
	}

	void FlexAlgorithm::ResolveCrossAxisAutoMargins()
	{
		for (FlexLine& line : lines)
		{
			const float line_cross_size = line.cross_size;
			float max_baseline = 0.f, max_baseline_to_bottom = 0.f, max_last_baseline = 0.f;
			for (size_t i = line.first; i < line.first + line.count; i++)
			{
				const FlexItem& child = items[i];
				if (child.align_self == ItemAlign::LastBaseline)
				{
					if (ParticipatesInBaselineAlignment(child))
						max_last_baseline = Math::Max(max_last_baseline, child.baseline);
					continue;
				}
				max_baseline = Math::Max(max_baseline, child.baseline);
				if (ParticipatesInBaselineAlignment(child))
					max_baseline_to_bottom = Math::Max(max_baseline_to_bottom, Cross(child.outer_target_size) - child.baseline);
			}

			for (size_t i = line.first; i < line.first + line.count; i++)
			{
				FlexItem& child = items[i];
				const float free_space = line_cross_size - Cross(child.outer_target_size);
				const int cross_start = (row ? Top : Left), cross_end = (row ? Bottom : Right);
				if (CrossStartAuto(child) && CrossEndAuto(child))
				{
					child.margin.v[cross_start] = free_space / 2.f;
					child.margin.v[cross_end] = free_space / 2.f;
				}
				else if (CrossStartAuto(child))
					child.margin.v[cross_start] = free_space;
				else if (CrossEndAuto(child))
					child.margin.v[cross_end] = free_space;
				else
					child.offset_cross = AlignFlexItemAlongCrossAxis(child, free_space, max_baseline,
						child.align_self == ItemAlign::LastBaseline ? max_last_baseline : max_baseline_to_bottom);
			}
		}
	}

	float FlexAlgorithm::DetermineContainerCrossSize(OptSize known)
	{
		const float total_cross_axis_gap = SumAxisGaps(Cross(gap), lines.size());
		float total_line_cross_size = 0.f;
		for (const FlexLine& line : lines)
			total_line_cross_size += line.cross_size;

		const float padding_border_sum = CrossSum(content_box_inset);
		const float cross_scrollbar_gutter =
			(row ? content_box_inset.v[Bottom] - container_border.v[Bottom] - box.GetEdge(BoxArea::Padding, BoxEdge::Bottom)
				 : content_box_inset.v[Right] - container_border.v[Right] - box.GetEdge(BoxArea::Padding, BoxEdge::Right));
		const float outer = Cross(known) ? *Cross(known) : total_line_cross_size + total_cross_axis_gap + padding_border_sum;
		const float outer_container_size =
			Math::Max(Clamp(outer, Cross(container_min_size), Cross(container_max_size)), padding_border_sum - cross_scrollbar_gutter);
		SetCross(container_size, outer_container_size);
		SetCross(inner_container_size, Math::Max(outer_container_size - padding_border_sum, 0.f));
		return total_line_cross_size;
	}

	void FlexAlgorithm::AlignFlexLinesPerAlignContent(float total_cross_size)
	{
		const size_t num_lines = lines.size();
		const float line_gap = Cross(gap);
		const float free_space = Cross(inner_container_size) - total_cross_size - SumAxisGaps(line_gap, num_lines);
		const ContentAlign mode = ApplyAlignmentFallback(free_space, num_lines, align_content, align_content_safe);
		for (size_t n = 0; n < num_lines; n++)
		{
			const size_t i = (is_wrap_reverse ? num_lines - 1 - n : n);
			lines[i].offset_cross = ComputeAlignmentOffset(free_space, num_lines, line_gap, mode, is_wrap_reverse, n == 0);
		}
	}

	void FlexAlgorithm::FinalLayoutPass(Output& output)
	{
		const int main_start = (row ? Left : Top), cross_start = (row ? Top : Left);
		const bool row_direction = row;
		float total_offset_cross = CrossStart(content_box_inset);

		bool baseline_set = false;
		auto layout_line = [&](FlexLine& line) {
			float total_offset_main = MainStart(content_box_inset);
			const float line_offset_cross = line.offset_cross;

			auto layout_item = [&](FlexItem& item) {
				// Format the item at its final size.
				Box item_box = item.box;
				const Vector2f size = item.target_size;
				item_box.SetContent(
					Math::Max(size - Vector2f(item.padding.Horizontal() + item.border.Horizontal(), item.padding.Vertical() + item.border.Vertical()),
						Vector2f(0.f)));
				item_box.SetEdge(BoxArea::Margin, BoxEdge::Left, item.margin.v[Left]);
				item_box.SetEdge(BoxArea::Margin, BoxEdge::Right, item.margin.v[Right]);
				item_box.SetEdge(BoxArea::Margin, BoxEdge::Top, item.margin.v[Top]);
				item_box.SetEdge(BoxArea::Margin, BoxEdge::Bottom, item.margin.v[Bottom]);

				const LayoutDetails::IndefiniteHeightScope height_scope(item.element, !ItemHeightIsDefinite(item));
				UniquePtr<LayoutBox> layout_box =
					FormattingContext::FormatIndependent(container_box, item.element, &item_box, FormattingContextType::Block);

				const float offset_main = total_offset_main + item.offset_main + item.margin.v[main_start];
				const float offset_cross = total_offset_cross + item.offset_cross + line_offset_cross + item.margin.v[cross_start];
				const Vector2f location = FromMainCross(offset_main, offset_cross);

				// A text item (an anonymous flex item) keeps the offset of its first line in the anonymous block box.
				Vector2f text_offset;
				if (rmlui_dynamic_cast<ElementText*>(item.element))
					text_offset = item.element->GetRelativeOffset(BoxArea::Border);
				item.element->SetOffset(location + text_offset, element);

				float inner_baseline = size.y;
				if (layout_box)
				{
					float baseline = 0.f;
					if (layout_box->GetBaselineOfFirstLine(baseline))
						inner_baseline = baseline;
				}
				if (item.is_scroll_container)
					inner_baseline = Math::Clamp(inner_baseline, 0.f, size.y);
				item.baseline = location.y + inner_baseline;

				if (layout_box)
				{
					output.visible_overflow = Math::Max(output.visible_overflow, location + layout_box->GetVisibleOverflowSize());
					const Vector2f margin_bottom_right = {item.margin.v[Right], item.margin.v[Bottom]};
					output.scrollable_overflow = Math::Max(output.scrollable_overflow, location + size + margin_bottom_right);
				}

				total_offset_main += item.offset_main + MainSum(item.margin) + Main(size);
			};

			if (reverse)
			{
				for (size_t n = line.count; n-- > 0;)
					layout_item(items[line.first + n]);
			}
			else
			{
				for (size_t n = 0; n < line.count; n++)
					layout_item(items[line.first + n]);
			}
			total_offset_cross += line_offset_cross + line.cross_size;
		};

		if (is_wrap_reverse)
		{
			for (size_t n = lines.size(); n-- > 0;)
				layout_line(lines[n]);
		}
		else
		{
			for (FlexLine& line : lines)
				layout_line(line);
		}

		// The container's first baseline (CSS Flexbox §8.5), from the startmost line.
		if (!lines.empty() && lines.front().count > 0)
		{
			const FlexLine& first_line = (is_wrap_reverse ? lines.back() : lines.front());
			if (first_line.count > 0)
			{
				const FlexItem* baseline_item = nullptr;
				if (!row_direction)
					baseline_item = &items[reverse ? first_line.first + first_line.count - 1 : first_line.first];
				else
				{
					for (size_t i = first_line.first; i < first_line.first + first_line.count && !baseline_item; i++)
					{
						if (ParticipatesInBaselineAlignment(items[i]) && items[i].align_self == ItemAlign::Baseline)
							baseline_item = &items[i];
					}
					if (!baseline_item)
						baseline_item = &items[first_line.first];
				}
				output.has_baseline = true;
				output.first_baseline = baseline_item->baseline;
				baseline_set = true;
			}
		}
		(void)baseline_set;
	}

	void FlexAlgorithm::PlaceAbsoluteChildren()
	{
		// The static position of an absolutely positioned child is that of a sole flex item (CSS Flexbox §4.1): its edge
		// per 'justify-content' in the main axis and per its 'align-self' in the cross axis, in the content box.
		for (size_t index = 0; index < absolute_children.size(); index++)
		{
			Element* child = absolute_children[index];
			const ComputedValues& computed = child->GetComputedValues();

			ContainerBox* containing_block_container = LayoutDetails::GetContainingBlock(container_box, computed.position()).container;

			// Main axis: 'justify-content' (no 'safe' fallback, as in browsers).
			ContainerBox::StaticAlignment main_alignment = ContainerBox::StaticAlignment::Start;
			switch (justify_content)
			{
			case ContentAlign::Start: main_alignment = ContainerBox::StaticAlignment::Start; break;
			case ContentAlign::End: main_alignment = ContainerBox::StaticAlignment::End; break;
			case ContentAlign::Stretch:
			case ContentAlign::SpaceBetween:
			case ContentAlign::FlexStart: main_alignment = reverse ? ContainerBox::StaticAlignment::End : ContainerBox::StaticAlignment::Start; break;
			case ContentAlign::FlexEnd: main_alignment = reverse ? ContainerBox::StaticAlignment::Start : ContainerBox::StaticAlignment::End; break;
			case ContentAlign::SpaceEvenly:
			case ContentAlign::SpaceAround:
			case ContentAlign::Center: main_alignment = ContainerBox::StaticAlignment::Center; break;
			}

			// Cross axis: 'align-self' ('stretch' does not apply to absolutely positioned boxes).
			ItemAlign align = (computed.align_self() == Style::AlignSelf::Auto ? align_items : ToItemAlign(computed.align_self()));
			const bool safe = (computed.align_self() == Style::AlignSelf::Auto ? align_items_safe
																			   : computed.align_self_overflow() == Style::OverflowAlignment::Safe);
			auto cross_edge = [this](ItemAlign value) {
				switch (value)
				{
				case ItemAlign::Start:
				case ItemAlign::Baseline:
				case ItemAlign::LastBaseline: return ContainerBox::StaticAlignment::Start;
				case ItemAlign::End: return ContainerBox::StaticAlignment::End;
				case ItemAlign::Stretch:
				case ItemAlign::FlexStart: return is_wrap_reverse ? ContainerBox::StaticAlignment::End : ContainerBox::StaticAlignment::Start;
				case ItemAlign::FlexEnd: return is_wrap_reverse ? ContainerBox::StaticAlignment::Start : ContainerBox::StaticAlignment::End;
				case ItemAlign::Center: return ContainerBox::StaticAlignment::Center;
				}
				return ContainerBox::StaticAlignment::Start;
			};

			ContainerBox::StaticPositionArea area;
			area.rectangle = Rectanglef::FromCorners(Vector2f(content_box_inset.v[Left], content_box_inset.v[Top]),
				container_size - Vector2f(content_box_inset.v[Right], content_box_inset.v[Bottom]));
			area.alignment[row ? 0 : 1] = main_alignment;
			area.alignment[row ? 1 : 0] = cross_edge(align);
			area.safe[row ? 1 : 0] = safe;

			containing_block_container->AddAbsoluteElement(child, area.rectangle.Position(), element, std::nullopt, area);
		}
	}

	FlexAlgorithm::Output FlexAlgorithm::Run(OptSize known, AvailableSpace available_width, AvailableSpace available_height,
		Vector2f scrollbar_gutter, RunMode run_mode)
	{
		Output output;
		ComputeConstants(known, available_width, available_height, scrollbar_gutter);

		// 9.1 Generate the flex items.
		GenerateItems();

		// 9.2 The available space for the items.
		AvailableSpace available[2] = {available_width, available_height};
		for (int axis = 0; axis < 2; axis++)
		{
			const float inset = (axis == 0 ? content_box_inset.Horizontal() : content_box_inset.Vertical());
			if (known[axis])
				available[axis] = AvailableSpace::MakeDefinite(Math::Max(*known[axis] - inset, 0.f));
			else if (available[axis].IsDefinite())
				available[axis].value = Math::Max(available[axis].value - inset, 0.f);
		}

		// 3. The flex base size and hypothetical main size of each item.
		DetermineFlexBaseSize(available);

		// 9.3 5. Collect the items into flex lines.
		if (is_balance)
			CollectBalancedFlexLines(available);
		else
			CollectFlexLines(available);

		// 4. The main size of the container.
		const int main = MainIndex();
		if (const Opt inner_main_size = Main(node_inner_size))
		{
			SetMain(inner_container_size, *inner_main_size);
			SetMain(container_size, *inner_main_size + MainSum(content_box_inset));
		}
		else
		{
			DetermineContainerMainSize(available);
			const ComputedValues& computed = element->GetComputedValues();
			const float inner_main = Main(inner_container_size);
			if (row)
				gap.x = Math::Max(0.f, ResolveValueOr(computed.column_gap(), inner_main, 0.f));
			else
				gap.y = Math::Max(0.f, ResolveValueOr(computed.row_gap(), inner_main, 0.f));
		}
		(void)main;

		// Only the width requested and it is the main size: nothing below changes it (this avoids measuring the cross size
		// of every item).
		if (run_mode == RunMode::ComputeWidth && row)
		{
			output.size = container_size;
			return output;
		}

		// 6. Resolve the flexible lengths.
		for (FlexLine& line : lines)
			ResolveFlexibleLengths(line);

		// 9.4 7. The hypothetical cross size of each item, and the baselines.
		for (FlexLine& line : lines)
			DetermineHypotheticalCrossSize(line, available);
		CalculateChildrenBaselines(available);

		// 8. The cross size of each line, 9. align-content: stretch.
		CalculateCrossSize(known);
		HandleAlignContentStretch(known);

		// 11. The used cross size of each item.
		DetermineUsedCrossSize();

		// 9.5 12. Distribute the remaining free space.
		DistributeRemainingFreeSpace();

		// 9.6 13., 14. Cross-axis auto margins and alignment.
		ResolveCrossAxisAutoMargins();

		// 15. The container's used cross size.
		const float total_line_cross_size = DetermineContainerCrossSize(known);
		output.size = container_size;
		if (run_mode != RunMode::PerformLayout)
			return output;

		// 16. Align the lines.
		AlignFlexLinesPerAlignContent(total_line_cross_size);

		// The final layout of the items.
		FinalLayoutPass(output);
		PlaceAbsoluteChildren();
		return output;
	}

	Vector2f GetScrollbarGutter(Element* element)
	{
		ElementScroll* element_scroll = element->GetElementScroll();
		return {element_scroll->GetScrollbarSize(ElementScroll::VERTICAL), element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL)};
	}

	// The intrinsic size of a flex container's content box under a min-content or max-content constraint.
	Vector2f ComputeIntrinsicContentSize(Element* element, AvailableSpace::Type constraint, float definite_content_height)
	{
		const Vector2f infinity(10000.f, 10000.f);
		RootBox root(infinity);
		FlexContainer flex_container_box(element, &root);
		Box box;
		LayoutDetails::BuildBox(box, Vector2f(-1.f), element, BuildBoxMode::UnalignedBlock);
		// A definite height that depends on a containing block (a percentage) is given by the box that was formatted: the
		// percentage heights of the items resolve against it.
		if (box.GetSize().y < 0.f && definite_content_height >= 0.f)
			box.SetContent({box.GetSize().x, definite_content_height});

		OptSize known;
		if (box.GetSize().y >= 0.f)
			known[1] = box.GetSize().y + box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
		const AvailableSpace available =
			(constraint == AvailableSpace::MinContent ? AvailableSpace::MakeMinContent() : AvailableSpace::MakeMaxContent());
		FlexAlgorithm algorithm(element, &flex_container_box, box, Vector2f(-1.f));
		const FlexAlgorithm::Output output = algorithm.Run(known, available, AvailableSpace::MakeMaxContent(), Vector2f(0.f), RunMode::ComputeWidth);
		const Vector2f edges = {box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
			box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)};
		return Math::Max(output.size - edges, Vector2f(0.f));
	}

} // namespace

UniquePtr<LayoutBox> FlexFormattingContext::Format(ContainerBox* parent_container, Element* element, const Box* override_initial_box)
{
	RMLUI_ZoneScopedC(0xAFAF4F);
	auto flex_container_box = MakeUnique<FlexContainer>(element, parent_container);

	const Vector2f containing_block = LayoutDetails::GetContainingBlock(parent_container, element).size;
	RMLUI_ASSERT(containing_block.x >= 0.f);

	// Build the initial box as specified by the flex container's style, as if it was a normal block element.
	Box& box = flex_container_box->GetBox();
	if (override_initial_box)
		box = *override_initial_box;
	else
		LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::Block);

	// Start with any auto-scrollbars off.
	flex_container_box->ResetScrollbars(box);

	const Vector2f box_content_size = box.GetSize();
	const bool auto_height = (box_content_size.y < 0.0f);
	const bool height_indefinite = (override_initial_box && !auto_height && LayoutDetails::IsHeightIndefinite(element));
	if (!auto_height && !height_indefinite)
		flex_container_box->SetDefiniteContentHeight(box_content_size.y);
	const Vector2f edges = {box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
		box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)};

	float min_height = 0.f, max_height = FLT_MAX;
	LayoutDetails::GetMinMaxHeight(min_height, max_height, element->GetComputedValues(), box, containing_block.y);
	if (LayoutDetails::IsMeasuringContentOf(element))
		max_height = FLT_MAX;

	for (int layout_iteration = 0; layout_iteration < 3; layout_iteration++)
	{
		// One or both scrollbars can be enabled between iterations: they take space between the padding and the border.
		const Vector2f scrollbar_size = GetScrollbarGutter(element);

		OptSize known;
		known[0] = box_content_size.x + edges.x;
		if (!auto_height)
			known[1] = box_content_size.y + edges.y;

		FlexAlgorithm algorithm(element, flex_container_box.get(), box, containing_block);
		algorithm.SetKnownHeightIndefinite(height_indefinite);
		const FlexAlgorithm::Output output = algorithm.Run(known, AvailableSpace::MakeDefinite(*known[0]),
			auto_height ? AvailableSpace::MakeMaxContent() : AvailableSpace::MakeDefinite(*known[1]), scrollbar_size, RunMode::PerformLayout);

		// The overflow of the items, relative to the content box.
		const Vector2f content_position = box.GetPosition();
		const Vector2f visible_overflow_size = Math::Max(output.visible_overflow - content_position, Vector2f(0.f));
		const Vector2f scrollable_content_size = Math::Max(output.scrollable_overflow - content_position, Vector2f(0.f));

		// Catch overflow before our height becomes fixed. This way the enabled scrollbar can extend the height of the flex
		// container, instead of eating into the content size, which could otherwise result in an unnecessary scrollbar on
		// the opposite axis.
		if (auto_height && !flex_container_box->CatchOverflow(visible_overflow_size, scrollable_content_size, box, max_height))
			continue;

		Vector2f formatted_content_size = box_content_size;
		if (auto_height)
			formatted_content_size.y = Math::Max(output.size.y - edges.y, 0.f);

		Box sized_box = box;
		sized_box.SetContent(formatted_content_size);

		// The element baseline is the distance from the bottom margin edge to the baseline.
		const float flex_baseline = (output.has_baseline ? output.first_baseline : output.size.y);
		const float element_baseline =
			sized_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border) + sized_box.GetEdge(BoxArea::Margin, BoxEdge::Bottom) - flex_baseline;
		flex_container_box->SetFirstBaseline(output.has_baseline, output.first_baseline);

		// Close the box, and break out of the loop if it did not produce any new scrollbars, otherwise format again.
		if (flex_container_box->Close(visible_overflow_size, scrollable_content_size, sized_box, element_baseline))
			break;
	}

	return flex_container_box;
}

Vector2f FlexFormattingContext::GetMaxContentSize(Element* element, float definite_content_height)
{
	return ComputeIntrinsicContentSize(element, AvailableSpace::MaxContent, definite_content_height);
}

Vector2f FlexFormattingContext::GetMinContentSize(Element* element, float definite_content_height)
{
	return {ComputeIntrinsicContentSize(element, AvailableSpace::MinContent, definite_content_height).x, 0.f};
}

} // namespace Rml
