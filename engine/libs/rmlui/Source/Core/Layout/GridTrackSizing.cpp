/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports track_sizing.rs.
*/

#include "GridTrackSizing.h"
#include <algorithm>

namespace Rml {
namespace GridLayout {

	namespace {
		// The measurements of the items while the tracks of an axis are sized (Taffy: IntrinsicSizeMeasurer).
		struct IntrinsicSizeMeasurer {
			const GridTrackVec& other_axis_tracks;
			TrackSizeEstimate estimate;
			Axis axis;
			SizeOptional inner_node_size;

			SizeOptional GridAreaSize(GridItem& item, const GridTrackVec& axis_tracks) const
			{
				return GridAreaSizeCached(item, axis, axis_tracks, other_axis_tracks, inner_node_size, estimate);
			}
			float MinContentContribution(GridItem& item, const GridTrackVec& axis_tracks) const
			{
				const SizeOptional grid_area_size = GridAreaSize(item, axis_tracks);
				SizeOptional available_space = grid_area_size;
				available_space.Set(axis, std::nullopt);
				const AxisPair<float> margins = MarginsAxisSumsWithBaselineShims(item, available_space.width);
				return MinContentContributionCached(item, axis, grid_area_size, available_space) + margins.Get(axis);
			}
			float MaxContentContribution(GridItem& item, const GridTrackVec& axis_tracks) const
			{
				const SizeOptional grid_area_size = GridAreaSize(item, axis_tracks);
				SizeOptional available_space = grid_area_size;
				available_space.Set(axis, std::nullopt);
				const AxisPair<float> margins = MarginsAxisSumsWithBaselineShims(item, available_space.width);
				return MaxContentContributionCached(item, axis, grid_area_size, available_space) + margins.Get(axis);
			}
			float MinimumContribution(GridItem& item, const GridTrackVec& axis_tracks) const
			{
				const SizeOptional grid_area_size = GridAreaSize(item, axis_tracks);
				SizeOptional available_space = grid_area_size;
				available_space.Set(axis, std::nullopt);
				const AxisPair<float> margins = MarginsAxisSumsWithBaselineShims(item, available_space.width);
				return MinimumContributionCached(item, axis, axis_tracks, grid_area_size, inner_node_size) + margins.Get(axis);
			}
		};

		enum class IntrinsicContributionType { Minimum, Maximum };

		bool SpansTrackMatching(const GridItem& item, Axis axis, const GridTrackVec& tracks, bool (*predicate)(const GridTrack&))
		{
			for (int i = item.TrackRangeBegin(axis); i < item.TrackRangeEnd(axis); i++)
			{
				if (predicate(tracks[i]))
					return true;
			}
			return false;
		}
		template <typename Predicate>
		bool SpansTrackMatchingFn(const GridItem& item, Axis axis, const GridTrackVec& tracks, Predicate predicate)
		{
			for (int i = item.TrackRangeBegin(axis); i < item.TrackRangeEnd(axis); i++)
			{
				if (predicate(tracks[i]))
					return true;
			}
			return false;
		}

		void FlushPlannedBaseSizeIncreases(GridTrackVec& tracks)
		{
			for (GridTrack& track : tracks)
			{
				track.base_size += track.base_size_planned_increase;
				track.base_size_planned_increase = 0.f;
			}
		}

		void FlushPlannedGrowthLimitIncreases(GridTrackVec& tracks, bool set_infinitely_growable)
		{
			for (GridTrack& track : tracks)
			{
				if (track.growth_limit_planned_increase > 0.f)
				{
					track.growth_limit = (track.growth_limit == Infinity ? track.base_size + track.growth_limit_planned_increase
																		 : track.growth_limit + track.growth_limit_planned_increase);
					track.infinitely_growable = set_infinitely_growable;
				}
				else
					track.infinitely_growable = false;
				track.growth_limit_planned_increase = 0.f;
			}
		}

		// 12.4 Initialize each track's base size and growth limit.
		void InitializeTrackSizes(GridTrackVec& axis_tracks, OptionalFloat axis_inner_node_size)
		{
			for (GridTrack& track : axis_tracks)
			{
				// A fixed minimum is the base size, an intrinsic one 0.
				const OptionalFloat base_size = track.min_track_sizing_function.DefiniteValue(axis_inner_node_size);
				track.base_size = base_size ? *base_size : 0.f;
				// A fixed maximum is the growth limit, an intrinsic or flexible one infinity.
				const OptionalFloat growth_limit = track.max_track_sizing_function.DefiniteValue(axis_inner_node_size);
				track.growth_limit = growth_limit ? *growth_limit : Infinity;
				// The growth limit is at least the base size.
				if (track.growth_limit < track.base_size)
					track.growth_limit = track.base_size;
			}
		}

		// 12.5.1 Shims baseline-aligned items, so that their contributions reflect their alignment.
		void ResolveItemBaselines(Axis axis, Vector<GridItem>& items, SizeOptional inner_node_size)
		{
			// The items grouped by the track of the other axis (row) they start in.
			const Axis other_axis = OtherAxis(axis);
			Vector<GridItem*> sorted;
			sorted.reserve(items.size());
			for (GridItem& item : items)
				sorted.push_back(&item);
			std::stable_sort(sorted.begin(), sorted.end(),
				[other_axis](const GridItem* a, const GridItem* b) { return a->Placement(other_axis).start < b->Placement(other_axis).start; });

			size_t row_begin = 0;
			while (row_begin < sorted.size())
			{
				const int current_row = sorted[row_begin]->Placement(other_axis).start;
				size_t row_end = row_begin;
				while (row_end < sorted.size() && sorted[row_end]->Placement(other_axis).start == current_row)
					row_end++;

				// With one item or none participating, baseline alignment does nothing in the row.
				int row_baseline_item_count = 0;
				for (size_t i = row_begin; i < row_end; i++)
					row_baseline_item_count += int(sorted[i]->ParticipatesInBaselineAlignment());
				if (row_baseline_item_count > 1)
				{
					float row_max_baseline = 0.f;
					bool first = true;
					for (size_t i = row_begin; i < row_end; i++)
					{
						GridItem& item = *sorted[i];
						if (!item.ParticipatesInBaselineAlignment())
							continue;
						float baseline = 0.f, height = 0.f;
						const bool has_baseline = MeasureItemBaseline(item, inner_node_size, baseline, height);
						if (!has_baseline)
							baseline = height;
						// Scroll containers' baselines are clamped to their border box.
						if (item.scroll_container.height)
							baseline = std::max(std::min(baseline, height), 0.f);
						const OptionalFloat margin_top = MaybeResolve(item.margin[0], inner_node_size.width ? inner_node_size.width : OptionalFloat(0.f));
						item.baseline = baseline + (margin_top ? *margin_top : 0.f);
						item.has_baseline = true;
						row_max_baseline = first ? item.baseline : std::max(row_max_baseline, item.baseline);
						first = false;
					}
					for (size_t i = row_begin; i < row_end; i++)
					{
						GridItem& item = *sorted[i];
						if (item.ParticipatesInBaselineAlignment())
						{
							item.baseline_shim = row_max_baseline - item.baseline;
							item.known_dimensions_cache_area.reset();
						}
					}
				}
				row_begin = row_end;
			}
		}

		// Distributes space to tracks up to their limits, returns the space left (12.5.1).
		template <typename IsAffected, typename Proportion, typename AffectedProperty, typename Limit>
		float DistributeSpaceUpToLimits(float space_to_distribute, GridTrack* tracks, int num_tracks, IsAffected track_is_affected,
			Proportion track_distribution_proportion, AffectedProperty track_affected_property, Limit track_limit)
		{
			// Stops below a small amount instead of exactly zero: rounding errors would loop forever.
			constexpr float Threshold = 0.01f;
			// Each iteration freezes a track at its limit; the bound guards against non-finite values.
			const int max_iterations = num_tracks + 1;
			for (int iteration = 0; iteration < max_iterations; iteration++)
			{
				if (space_to_distribute <= Threshold)
					break;

				float proportion_sum = 0.f;
				float min_increase_limit = Infinity;
				bool any = false;
				for (int i = 0; i < num_tracks; i++)
				{
					const GridTrack& track = tracks[i];
					if (!(track_affected_property(track) + track.item_incurred_increase < track_limit(track)) || !track_is_affected(track))
						continue;
					const float proportion = track_distribution_proportion(track);
					proportion_sum += proportion;
					// A track without a share (a 0fr track among flexible ones) takes nothing: it does not limit the others.
					if (proportion <= 0.f)
						continue;
					const float increase_limit = (track_limit(track) - track_affected_property(track) - track.item_incurred_increase) / proportion;
					min_increase_limit = any ? std::min(min_increase_limit, increase_limit) : increase_limit;
					any = true;
				}
				if (proportion_sum == 0.f)
					break;

				const float iteration_item_incurred_increase = std::min(min_increase_limit, space_to_distribute / proportion_sum);
				for (int i = 0; i < num_tracks; i++)
				{
					GridTrack& track = tracks[i];
					if (!track_is_affected(track))
						continue;
					const float increase = iteration_item_incurred_increase * track_distribution_proportion(track);
					if (increase > 0.f && track_affected_property(track) + track.item_incurred_increase + increase <= track_limit(track) + Threshold)
					{
						track.item_incurred_increase += increase;
						space_to_distribute -= increase;
					}
				}
			}
			return space_to_distribute;
		}

		template <typename IsAffected, typename Proportion, typename Limit>
		void DistributeItemSpaceToBaseSizeInner(float space, GridTrack* tracks, int num_tracks, IsAffected track_is_affected,
			Proportion track_distribution_proportion, Limit track_limit, IntrinsicContributionType contribution_type)
		{
			bool any_affected = false;
			for (int i = 0; i < num_tracks && !any_affected; i++)
				any_affected = track_is_affected(tracks[i]);
			if (space == 0.f || !any_affected)
				return;

			auto get_base_size = [](const GridTrack& track) { return track.base_size; };

			// 1. The space to distribute.
			float track_sizes = 0.f;
			for (int i = 0; i < num_tracks; i++)
				track_sizes += tracks[i].base_size;
			float extra_space = std::max(0.f, space - track_sizes);

			// 2. Distribute up to the limits.
			constexpr float Threshold = 0.000001f;
			extra_space =
				DistributeSpaceUpToLimits(extra_space, tracks, num_tracks, track_is_affected, track_distribution_proportion, get_base_size, track_limit);

			// 3. Distribute the rest beyond the limits: to the affected tracks with an intrinsic maximum (for minimum and min-content
			// contributions) or a max-content maximum (for max-content contributions); without any, to all affected tracks.
			if (extra_space > Threshold)
			{
				auto filter = [contribution_type](const GridTrack& track) {
					if (contribution_type == IntrinsicContributionType::Minimum)
						return track.max_track_sizing_function.IsIntrinsic();
					return track.max_track_sizing_function.IsMaxOrFitContent();
				};
				int number_of_tracks = 0;
				for (int i = 0; i < num_tracks; i++)
					number_of_tracks += int(track_is_affected(tracks[i]) && filter(tracks[i]));
				const bool use_filter = (number_of_tracks > 0);
				// Growth limits are ignored now, the arguments of 'fit-content()' still cap the growth.
				DistributeSpaceUpToLimits(
					extra_space, tracks, num_tracks, [&](const GridTrack& track) { return track_is_affected(track) && (!use_filter || filter(track)); },
					track_distribution_proportion, get_base_size, [](const GridTrack& track) { return track.fit_content_limit; });
			}

			// 4. The planned increase of each track is the largest item-incurred increase.
			for (int i = 0; i < num_tracks; i++)
			{
				GridTrack& track = tracks[i];
				if (track.item_incurred_increase > track.base_size_planned_increase)
					track.base_size_planned_increase = track.item_incurred_increase;
				track.item_incurred_increase = 0.f;
			}
		}

		// 12.5.1 Distributing extra space across spanned tracks, to the base sizes.
		template <typename IsAffected, typename Limit>
		void DistributeItemSpaceToBaseSize(bool is_flex, float space, GridTrack* tracks, int num_tracks, IsAffected track_is_affected,
			Limit track_limit, IntrinsicContributionType contribution_type)
		{
			if (is_flex)
			{
				auto filter = [&](const GridTrack& track) { return track.IsFlexible() && track_is_affected(track); };
				// By the flex factors of the affected tracks when their sum is positive, else equally (as Chrome: the spec blends
				// the two below a sum of one and sums over every flexible track the item spans).
				float flex_factor_sum = 0.f;
				for (int i = 0; i < num_tracks; i++)
				{
					if (filter(tracks[i]))
						flex_factor_sum += tracks[i].FlexFactor();
				}
				if (flex_factor_sum > 0.f)
					DistributeItemSpaceToBaseSizeInner(
						space, tracks, num_tracks, filter, [](const GridTrack& track) { return track.FlexFactor(); }, track_limit, contribution_type);
				else
					DistributeItemSpaceToBaseSizeInner(space, tracks, num_tracks, filter, [](const GridTrack&) { return 1.f; }, track_limit,
						contribution_type);
			}
			else
				DistributeItemSpaceToBaseSizeInner(space, tracks, num_tracks, track_is_affected, [](const GridTrack&) { return 1.f; }, track_limit,
					contribution_type);
		}

		// 12.5.1 Distributing extra space across spanned tracks, to the growth limits.
		template <typename IsAffected>
		void DistributeItemSpaceToGrowthLimit(float space, GridTrack* tracks, int num_tracks, IsAffected track_is_affected)
		{
			bool any_affected = false;
			for (int i = 0; i < num_tracks && !any_affected; i++)
				any_affected = track_is_affected(tracks[i]);
			if (space == 0.f || !any_affected)
				return;

			auto growth_limit_or_base_size = [](const GridTrack& track) { return track.growth_limit == Infinity ? track.base_size : track.growth_limit; };
			auto fit_content_limit = [](const GridTrack& track) { return track.fit_content_limit; };

			// 1. The space to distribute.
			float track_sizes = 0.f;
			for (int i = 0; i < num_tracks; i++)
				track_sizes += growth_limit_or_base_size(tracks[i]);
			float extra_space = std::max(0.f, space - track_sizes);

			// 2. Up to the limits: only tracks that are infinitely growable (or still infinite) grow, up to their 'fit-content()'
			// argument.
			extra_space = DistributeSpaceUpToLimits(
				extra_space, tracks, num_tracks,
				[&](const GridTrack& track) { return track_is_affected(track) && (track.infinitely_growable || track.growth_limit == Infinity); },
				[](const GridTrack&) { return 1.f; }, growth_limit_or_base_size, fit_content_limit);

			// 3. Beyond the limits: every affected track.
			if (extra_space > 0.f)
				DistributeSpaceUpToLimits(extra_space, tracks, num_tracks, track_is_affected, [](const GridTrack&) { return 1.f; },
					growth_limit_or_base_size, fit_content_limit);

			// 4. The planned increase of each track is the largest item-incurred increase.
			for (int i = 0; i < num_tracks; i++)
			{
				GridTrack& track = tracks[i];
				if (track.item_incurred_increase > track.growth_limit_planned_increase)
					track.growth_limit_planned_increase = track.item_incurred_increase;
				track.item_incurred_increase = 0.f;
			}
		}

		float CrossedFlexFactorSum(const GridTrack* tracks, int num_tracks)
		{
			float sum = 0.f;
			for (int i = 0; i < num_tracks; i++)
			{
				if (tracks[i].IsFlexible())
					sum += tracks[i].FlexFactor();
			}
			return sum;
		}

		float InflexibleBaseSizes(const GridTrack* tracks, int num_tracks)
		{
			float sum = 0.f;
			for (int i = 0; i < num_tracks; i++)
			{
				if (!tracks[i].IsFlexible())
					sum += tracks[i].base_size;
			}
			return sum;
		}

		bool HasAutoMinTrackSizingFunction(const GridTrack& track)
		{
			// As Chrome: not with a min-content maximum (the maximum is floored by the minimum, CSS Grid 2, 7.2.4).
			return track.min_track_sizing_function.IsAuto() && !track.max_track_sizing_function.IsMinContent();
		}
		bool HasMaxContentMinTrackSizingFunction(const GridTrack& track)
		{
			return track.min_track_sizing_function.IsMaxContent();
		}
		bool HasMinOrMaxContentMinTrackSizingFunction(const GridTrack& track)
		{
			return track.min_track_sizing_function.IsMinOrMaxContent();
		}

		// 12.5 Resolve intrinsic track sizes.
		void ResolveIntrinsicTrackSizes(Axis axis, GridTrackVec& axis_tracks, const GridTrackVec& other_axis_tracks, Vector<GridItem>& items,
			AvailableSpace axis_available_grid_space, SizeOptional inner_node_size, TrackSizeEstimate estimate)
		{
			// Step 1 (baseline shims) is done.

			// The items in ascending order of their span, those that cross a flexible track last, in one batch whatever their span.
			Vector<GridItem*> sorted_items;
			sorted_items.reserve(items.size());
			for (GridItem& item : items)
			{
				if (!item.CrossesFlexibleTrack(axis))
					sorted_items.push_back(&item);
			}
			const size_t non_flex_item_count = sorted_items.size();
			for (GridItem& item : items)
			{
				if (item.CrossesFlexibleTrack(axis))
					sorted_items.push_back(&item);
			}
			std::stable_sort(sorted_items.begin(), sorted_items.begin() + (std::ptrdiff_t)non_flex_item_count,
				[axis](const GridItem* a, const GridItem* b) { return a->SpanIn(axis) < b->SpanIn(axis); });

			const OptionalFloat axis_inner_node_size = inner_node_size.Get(axis);
			const IntrinsicSizeMeasurer item_sizer{other_axis_tracks, estimate, axis, inner_node_size};
			GridTrack* const tracks_data = axis_tracks.data();

			// Steps 2, 3 and 4, batch by batch (Taffy: ItemBatcher).
			size_t batch_begin = 0;
			while (batch_begin < sorted_items.size())
			{
				const bool is_flex = sorted_items[batch_begin]->CrossesFlexibleTrack(axis);
				const int batch_span = sorted_items[batch_begin]->SpanIn(axis);
				size_t batch_end = batch_begin + 1;
				if (is_flex)
					batch_end = sorted_items.size();
				else
				{
					while (batch_end < sorted_items.size() && !sorted_items[batch_end]->CrossesFlexibleTrack(axis) &&
						sorted_items[batch_end]->SpanIn(axis) <= batch_span)
						batch_end++;
				}

				// 2. Tracks with an intrinsic sizing function (not flexible), for the items that span them alone.
				if (!is_flex && batch_span == 1)
				{
					for (size_t b = batch_begin; b < batch_end; b++)
					{
						GridItem& item = *sorted_items[b];
						const int track_index = item.PlacementIndexes(axis).start + 1;
						GridTrack& track = axis_tracks[track_index];

						// The base size.
						float new_base_size = track.base_size;
						if (track.min_track_sizing_function.IsMinContent())
							new_base_size = std::max(track.base_size, item_sizer.MinContentContribution(item, axis_tracks));
						else if (track.min_track_sizing_function.IsMaxContent())
							new_base_size = std::max(track.base_size, item_sizer.MaxContentContribution(item, axis_tracks));
						// A percentage of an indefinite size behaves as 'auto'.
						else if (track.min_track_sizing_function.BehavesAsAuto(axis_inner_node_size))
						{
							float space = 0.f;
							// As browsers: the limited min-content contribution under a min-content or max-content constraint, but not
							// for a scroll container (its automatic minimum, 0, wins).
							if (axis_available_grid_space.type != AvailableSpace::Type::Definite && !item.scroll_container.Get(axis))
							{
								const float axis_minimum_size = item_sizer.MinimumContribution(item, axis_tracks);
								const float axis_min_content_size = item_sizer.MinContentContribution(item, axis_tracks);
								const OptionalFloat limit = track.max_track_sizing_function.DefiniteLimit(axis_inner_node_size);
								space = std::max(MaybeMin(axis_min_content_size, limit), axis_minimum_size);
							}
							else
								space = item_sizer.MinimumContribution(item, axis_tracks);
							new_base_size = std::max(track.base_size, space);
						}
						axis_tracks[track_index].base_size = new_base_size;

						// The growth limit. Contributions measure the item, so they are only computed when needed; the min-content
						// contribution of every item in an intrinsic track is (the check for a re-run compares it).
						const GridTrack& sized = axis_tracks[track_index];
						const TrackSizingFunction max_function = sized.max_track_sizing_function;
						const float fit_content_limit = sized.fit_content_limit;
						OptionalFloat min_content_contribution;
						if (sized.HasIntrinsicSizingFunction())
							min_content_contribution = item_sizer.MinContentContribution(item, axis_tracks);

						OptionalFloat planned_increase;
						if (max_function.IsFitContent())
						{
							// At least the fit-content limited max-content contribution, and the min-content one for items that are
							// not scroll containers.
							const float max_content_contribution = std::min(item_sizer.MaxContentContribution(item, axis_tracks), fit_content_limit);
							if (min_content_contribution && !item.scroll_container.Get(axis))
								planned_increase = std::max(*min_content_contribution, max_content_contribution);
							else
								planned_increase = max_content_contribution;
						}
						else if (max_function.IsMaxContentAlike() || (max_function.UsesPercentage() && !axis_inner_node_size))
						{
							// A percentage of an indefinite size behaves as 'auto' (as Chrome).
							planned_increase = item_sizer.MaxContentContribution(item, axis_tracks);
						}
						else if (max_function.IsIntrinsic())
							planned_increase = min_content_contribution;

						if (planned_increase)
						{
							GridTrack& target = axis_tracks[track_index];
							target.growth_limit_planned_increase = std::max(target.growth_limit_planned_increase, *planned_increase);
						}
					}

					for (GridTrack& track : axis_tracks)
					{
						if (track.growth_limit_planned_increase > 0.f)
						{
							track.growth_limit = (track.growth_limit == Infinity ? track.growth_limit_planned_increase
																				 : std::max(track.growth_limit, track.growth_limit_planned_increase));
						}
						track.infinitely_growable = false;
						track.growth_limit_planned_increase = 0.f;
						if (track.growth_limit < track.base_size)
							track.growth_limit = track.base_size;
					}

					batch_begin = batch_end;
					continue;
				}

				// 3. and 4. Items spanning several tracks, then those crossing flexible tracks.

				// 1. For intrinsic minimums: distribute the items' minimum contributions to the tracks with an intrinsic minimum.
				for (size_t b = batch_begin; b < batch_end; b++)
				{
					GridItem& item = *sorted_items[b];
					if (!item.CrossesIntrinsicTrack(axis))
						continue;
					const int begin = item.TrackRangeBegin(axis), end = item.TrackRangeEnd(axis);
					float space = 0.f;
					// As browsers: the limited min-content contribution under a min-content or max-content constraint, not for a
					// scroll container.
					if (axis_available_grid_space.type != AvailableSpace::Type::Definite && !item.scroll_container.Get(axis))
					{
						const float axis_minimum_size = item_sizer.MinimumContribution(item, axis_tracks);
						const float axis_min_content_size = item_sizer.MinContentContribution(item, axis_tracks);
						const OptionalFloat limit = SpannedTrackLimit(item, axis, axis_tracks, axis_inner_node_size);
						const float limited_min_content = std::max(MaybeMin(axis_min_content_size, limit), axis_minimum_size);

						// Items crossing flexible tracks give the flexible tracks only the share of their content that the crossed
						// flex factor sum (at most one) gives them, beyond the inflexible tracks, at least their minimum
						// contribution (CSS Grid 2, 12.5: the 0fr collapse pattern depends on it).
						if (is_flex)
						{
							const float inflexible_sizes = InflexibleBaseSizes(tracks_data + begin, end - begin);
							const float scale = std::min(CrossedFlexFactorSum(tracks_data + begin, end - begin), 1.f);
							const float excess = std::max(limited_min_content - inflexible_sizes, 0.f);
							space = std::max(axis_minimum_size, inflexible_sizes + excess * scale);
						}
						else
							space = limited_min_content;
					}
					else
						space = item_sizer.MinimumContribution(item, axis_tracks);

					if (space > 0.f)
					{
						auto has_intrinsic_min = [axis_inner_node_size](const GridTrack& track) {
							return !track.min_track_sizing_function.DefiniteValue(axis_inner_node_size);
						};
						if (item.scroll_container.Get(axis))
							DistributeItemSpaceToBaseSize(is_flex, space, tracks_data + begin, end - begin, has_intrinsic_min,
								[](const GridTrack& track) { return track.FitContentLimitedGrowthLimit(); }, IntrinsicContributionType::Minimum);
						else
							DistributeItemSpaceToBaseSize(is_flex, space, tracks_data + begin, end - begin, has_intrinsic_min,
								[](const GridTrack& track) { return track.growth_limit; }, IntrinsicContributionType::Minimum);
					}
				}
				FlushPlannedBaseSizeIncreases(axis_tracks);

				// 2. For content-based minimums: the min-content contributions to the tracks with a min-content or max-content minimum.
				for (size_t b = batch_begin; b < batch_end; b++)
				{
					GridItem& item = *sorted_items[b];
					if (!SpansTrackMatching(item, axis, axis_tracks, &HasMinOrMaxContentMinTrackSizingFunction))
						continue;
					const int begin = item.TrackRangeBegin(axis), end = item.TrackRangeEnd(axis);
					const float space = item_sizer.MinContentContribution(item, axis_tracks);
					if (space > 0.f)
					{
						if (item.scroll_container.Get(axis))
							DistributeItemSpaceToBaseSize(is_flex, space, tracks_data + begin, end - begin, &HasMinOrMaxContentMinTrackSizingFunction,
								[](const GridTrack& track) { return track.FitContentLimitedGrowthLimit(); }, IntrinsicContributionType::Minimum);
						else
							DistributeItemSpaceToBaseSize(is_flex, space, tracks_data + begin, end - begin, &HasMinOrMaxContentMinTrackSizingFunction,
								[](const GridTrack& track) { return track.growth_limit; }, IntrinsicContributionType::Minimum);
					}
				}
				FlushPlannedBaseSizeIncreases(axis_tracks);

				// 3. For max-content minimums: under a max-content constraint, the limited max-content contributions to the tracks
				// with an 'auto' or max-content minimum (the max-content ones first, as Chrome and Firefox).
				if (axis_available_grid_space.type == AvailableSpace::Type::MaxContent)
				{
					for (size_t b = batch_begin; b < batch_end; b++)
					{
						GridItem& item = *sorted_items[b];
						if (!SpansTrackMatchingFn(item, axis, axis_tracks, [](const GridTrack& track) {
								return HasAutoMinTrackSizingFunction(track) || HasMaxContentMinTrackSizingFunction(track);
							}))
							continue;
						const int begin = item.TrackRangeBegin(axis), end = item.TrackRangeEnd(axis);
						const float axis_max_content_size = item_sizer.MaxContentContribution(item, axis_tracks);
						const OptionalFloat limit = SpannedTrackLimit(item, axis, axis_tracks, axis_inner_node_size);
						float space = MaybeMin(axis_max_content_size, limit);

						// The flex factor scaling of the intrinsic minimums above, anchored at the inflexible tracks.
						if (is_flex)
						{
							const float inflexible_sizes = InflexibleBaseSizes(tracks_data + begin, end - begin);
							const float scale = std::min(CrossedFlexFactorSum(tracks_data + begin, end - begin), 1.f);
							space = inflexible_sizes + std::max(space - inflexible_sizes, 0.f) * scale;
						}
						if (space > 0.f)
						{
							bool any_max_content_min = false;
							for (int i = begin; i < end; i++)
								any_max_content_min |= HasMaxContentMinTrackSizingFunction(axis_tracks[i]);
							if (any_max_content_min)
								DistributeItemSpaceToBaseSize(is_flex, space, tracks_data + begin, end - begin, &HasMaxContentMinTrackSizingFunction,
									[](const GridTrack&) { return Infinity; }, IntrinsicContributionType::Maximum);
							else
								DistributeItemSpaceToBaseSize(is_flex, space, tracks_data + begin, end - begin, &HasAutoMinTrackSizingFunction,
									[](const GridTrack& track) { return track.FitContentLimitedGrowthLimit(); }, IntrinsicContributionType::Maximum);
						}
					}
					FlushPlannedBaseSizeIncreases(axis_tracks);
				}

				// In all cases, the max-content contributions to the tracks with a max-content minimum.
				for (size_t b = batch_begin; b < batch_end; b++)
				{
					GridItem& item = *sorted_items[b];
					if (!SpansTrackMatching(item, axis, axis_tracks, &HasMaxContentMinTrackSizingFunction))
						continue;
					const int begin = item.TrackRangeBegin(axis), end = item.TrackRangeEnd(axis);
					const float space = item_sizer.MaxContentContribution(item, axis_tracks);
					if (space > 0.f)
						DistributeItemSpaceToBaseSize(is_flex, space, tracks_data + begin, end - begin, &HasMaxContentMinTrackSizingFunction,
							[](const GridTrack& track) { return track.growth_limit; }, IntrinsicContributionType::Maximum);
				}
				FlushPlannedBaseSizeIncreases(axis_tracks);

				// 4. A growth limit below the base size is raised to it.
				for (GridTrack& track : axis_tracks)
				{
					if (track.growth_limit < track.base_size)
						track.growth_limit = track.base_size;
				}

				// A flexible track has no intrinsic maximum: steps 5 and 6 do not apply to it.
				if (!is_flex)
				{
					// 5. For intrinsic maximums: the min-content contributions to the growth limits of tracks with an intrinsic maximum.
					auto has_intrinsic_max = [axis_inner_node_size](const GridTrack& track) {
						return !track.max_track_sizing_function.HasDefiniteValue(axis_inner_node_size);
					};
					for (size_t b = batch_begin; b < batch_end; b++)
					{
						GridItem& item = *sorted_items[b];
						if (!SpansTrackMatchingFn(item, axis, axis_tracks, has_intrinsic_max))
							continue;
						const int begin = item.TrackRangeBegin(axis), end = item.TrackRangeEnd(axis);
						const float space = item_sizer.MinContentContribution(item, axis_tracks);
						if (space > 0.f)
							DistributeItemSpaceToGrowthLimit(space, tracks_data + begin, end - begin, has_intrinsic_max);
					}
					// Tracks whose growth limit became finite in this step are infinitely growable for the next.
					FlushPlannedGrowthLimitIncreases(axis_tracks, true);

					// 6. For max-content maximums: the max-content contributions to the growth limits of tracks with a max-content
					// maximum, 'fit-content()' tracks limited by their argument.
					auto has_max_content_max = [axis_inner_node_size](const GridTrack& track) {
						return track.max_track_sizing_function.IsMaxContentAlike() ||
							(track.max_track_sizing_function.UsesPercentage() && !axis_inner_node_size);
					};
					for (size_t b = batch_begin; b < batch_end; b++)
					{
						GridItem& item = *sorted_items[b];
						if (!SpansTrackMatchingFn(item, axis, axis_tracks, has_max_content_max))
							continue;
						const int begin = item.TrackRangeBegin(axis), end = item.TrackRangeEnd(axis);
						const float space = item_sizer.MaxContentContribution(item, axis_tracks);
						if (space > 0.f)
							DistributeItemSpaceToGrowthLimit(space, tracks_data + begin, end - begin, has_max_content_max);
					}
					FlushPlannedGrowthLimitIncreases(axis_tracks, false);
				}

				batch_begin = batch_end;
			}

			// Step 5. A growth limit still infinite (a track without items, or a flexible track) is the base size: maximizing the
			// tracks must not grow flexible tracks.
			for (GridTrack& track : axis_tracks)
			{
				if (track.growth_limit == Infinity)
					track.growth_limit = track.base_size;
			}
		}

		// 12.6 Maximize tracks: free space to the tracks with finite growth limits, up to them.
		void MaximiseTracks(GridTrackVec& axis_tracks, AvailableSpace axis_available_grid_space)
		{
			float used_space = 0.f;
			for (const GridTrack& track : axis_tracks)
				used_space += track.base_size;
			const float free_space = axis_available_grid_space.ComputeFreeSpace(used_space);
			if (free_space == Infinity)
			{
				for (GridTrack& track : axis_tracks)
					track.base_size = track.growth_limit;
			}
			else if (free_space > 0.f)
			{
				DistributeSpaceUpToLimits(
					free_space, axis_tracks.data(), (int)axis_tracks.size(), [](const GridTrack&) { return true; },
					[](const GridTrack&) { return 1.f; }, [](const GridTrack& track) { return track.base_size; },
					[](const GridTrack& track) { return track.FitContentLimitedGrowthLimit(); });
				for (GridTrack& track : axis_tracks)
				{
					track.base_size += track.item_incurred_increase;
					track.item_incurred_increase = 0.f;
				}
			}
		}

		// 12.7.1 Find the size of an fr: the largest size an fr can be without exceeding the space to fill.
		float FindSizeOfFr(const GridTrack* tracks, int num_tracks, float space_to_fill)
		{
			// No space: this guards the loop too.
			if (space_to_fill == 0.f)
				return 0.f;

			// A flexible track whose share is below its base size makes the algorithm restart with it inflexible: the first
			// iteration's infinite size never does.
			float hypothetical_fr_size = Infinity;
			float previous_iter_hypothetical_fr_size = Infinity;
			// Each restart makes a track inflexible; the bound guards against non-finite values.
			const int max_iterations = num_tracks + 1;
			for (int iteration = 0; iteration < max_iterations; iteration++)
			{
				float used_space = 0.f;
				float naive_flex_factor_sum = 0.f;
				for (int i = 0; i < num_tracks; i++)
				{
					const GridTrack& track = tracks[i];
					if (track.max_track_sizing_function.IsFr() && track.max_track_sizing_function.fr * hypothetical_fr_size >= track.base_size)
						naive_flex_factor_sum += track.max_track_sizing_function.fr;
					else
						used_space += track.base_size;
				}
				const float leftover_space = space_to_fill - used_space;
				const float flex_factor = std::max(naive_flex_factor_sum, 1.f);

				previous_iter_hypothetical_fr_size = hypothetical_fr_size;
				hypothetical_fr_size = leftover_space / flex_factor;

				bool valid = true;
				for (int i = 0; i < num_tracks && valid; i++)
				{
					const GridTrack& track = tracks[i];
					if (track.max_track_sizing_function.IsFr())
					{
						const float factor = track.max_track_sizing_function.fr;
						valid = (factor * hypothetical_fr_size >= track.base_size || factor * previous_iter_hypothetical_fr_size < track.base_size);
					}
				}
				if (valid)
					break;
			}
			return hypothetical_fr_size;
		}

		// 12.7 Expand flexible tracks.
		void ExpandFlexibleTracks(Axis axis, GridTrackVec& axis_tracks, const GridTrackVec& other_axis_tracks, Vector<GridItem>& items,
			OptionalFloat axis_min_size, OptionalFloat axis_max_size, AvailableSpace axis_available_space_for_expansion,
			SizeOptional inner_node_size, TrackSizeEstimate estimate)
		{
			const IntrinsicSizeMeasurer item_sizer{other_axis_tracks, estimate, axis, inner_node_size};

			float flex_fraction = 0.f;
			switch (axis_available_space_for_expansion.type)
			{
			case AvailableSpace::Type::Definite:
			{
				// With definite free space: the size of an fr for all the tracks and the available grid space.
				const float available_space = axis_available_space_for_expansion.value;
				float used_space = 0.f;
				for (const GridTrack& track : axis_tracks)
					used_space += track.base_size;
				const float free_space = available_space - used_space;
				flex_fraction = (free_space <= 0.f ? 0.f : FindSizeOfFr(axis_tracks.data(), (int)axis_tracks.size(), available_space));
				break;
			}
			// Under a min-content constraint: zero.
			case AvailableSpace::Type::MinContent: flex_fraction = 0.f; break;
			case AvailableSpace::Type::MaxContent:
			{
				// With indefinite free space: the largest of each flexible track's base size per flex factor (above one), and of
				// each flexible item's max-content contribution over its tracks.
				float max_track_fraction = 0.f;
				bool any_track = false;
				for (const GridTrack& track : axis_tracks)
				{
					if (!track.max_track_sizing_function.IsFr())
						continue;
					const float flex_factor = track.FlexFactor();
					const float value = (flex_factor > 1.f ? track.base_size / flex_factor : track.base_size);
					max_track_fraction = any_track ? std::max(max_track_fraction, value) : value;
					any_track = true;
				}
				float max_item_fraction = 0.f;
				bool any_item = false;
				for (GridItem& item : items)
				{
					if (!item.CrossesFlexibleTrack(axis))
						continue;
					const float max_content_contribution = item_sizer.MaxContentContribution(item, axis_tracks);
					const int begin = item.TrackRangeBegin(axis), end = item.TrackRangeEnd(axis);
					const float value = FindSizeOfFr(axis_tracks.data() + begin, end - begin, max_content_contribution);
					max_item_fraction = any_item ? std::max(max_item_fraction, value) : value;
					any_item = true;
				}
				flex_fraction = std::max(max_track_fraction, max_item_fraction);

				// When the grid would then be smaller than its minimum size (larger than its maximum), redo this with a definite free
				// space of that size (the minimum wins).
				float hypothetical_grid_size = 0.f;
				for (const GridTrack& track : axis_tracks)
				{
					if (track.max_track_sizing_function.IsFr())
						hypothetical_grid_size += std::max(track.base_size, track.max_track_sizing_function.fr * flex_fraction);
					else
						hypothetical_grid_size += track.base_size;
				}
				const float min_size = axis_min_size ? *axis_min_size : 0.f;
				const float max_size = axis_max_size ? *axis_max_size : Infinity;
				if (hypothetical_grid_size < min_size)
					flex_fraction = FindSizeOfFr(axis_tracks.data(), (int)axis_tracks.size(), min_size);
				else if (hypothetical_grid_size > max_size)
					flex_fraction = FindSizeOfFr(axis_tracks.data(), (int)axis_tracks.size(), max_size);
				break;
			}
			}

			// A flexible track grows to its share of the flex fraction.
			for (GridTrack& track : axis_tracks)
			{
				if (track.max_track_sizing_function.IsFr())
					track.base_size = std::max(track.base_size, track.max_track_sizing_function.fr * flex_fraction);
			}
		}

		// 12.8 Stretch 'auto' tracks: definite free space shared by the tracks with an 'auto' maximum.
		void StretchAutoTracks(GridTrackVec& axis_tracks, OptionalFloat axis_min_size, AvailableSpace axis_available_space_for_expansion)
		{
			int num_auto_tracks = 0;
			float used_space = 0.f;
			for (const GridTrack& track : axis_tracks)
			{
				num_auto_tracks += int(track.max_track_sizing_function.IsAuto());
				used_space += track.base_size;
			}
			if (num_auto_tracks == 0)
				return;
			// With indefinite free space and a definite minimum size, the minimum size gives the free space.
			float free_space = 0.f;
			if (axis_available_space_for_expansion.IsDefinite())
				free_space = axis_available_space_for_expansion.ComputeFreeSpace(used_space);
			else if (axis_min_size)
				free_space = *axis_min_size - used_space;
			if (free_space > 0.f)
			{
				const float extra_space_per_auto_track = free_space / float(num_auto_tracks);
				for (GridTrack& track : axis_tracks)
				{
					if (track.max_track_sizing_function.IsAuto())
						track.base_size += extra_space_per_auto_track;
				}
			}
		}
	} // namespace

	float ComputeAlignmentGutterAdjustment(ContentAlignment alignment, OptionalFloat axis_inner_node_size, TrackSizeEstimate estimate,
		const GridTrackVec& tracks)
	{
		if (tracks.size() <= 1)
			return 0.f;

		// Items never cross the outer gutters: 'start' and 'end' are the same here.
		int outer_gutter_weight = 0, inner_gutter_weight = 0;
		switch (alignment.keyword)
		{
		case ContentAlign::Start:
		case ContentAlign::End:
		case ContentAlign::Center: outer_gutter_weight = 1; break;
		case ContentAlign::Stretch: break;
		case ContentAlign::SpaceBetween: inner_gutter_weight = 1; break;
		case ContentAlign::SpaceAround:
			outer_gutter_weight = 1;
			inner_gutter_weight = 2;
			break;
		case ContentAlign::SpaceEvenly:
			outer_gutter_weight = 1;
			inner_gutter_weight = 1;
			break;
		}
		if (inner_gutter_weight == 0 || !axis_inner_node_size)
			return 0.f;

		float track_size_sum = 0.f;
		bool definite = true;
		for (const GridTrack& track : tracks)
		{
			const OptionalFloat size = TrackSizeEstimateOf(track, axis_inner_node_size, estimate);
			if (!size)
			{
				definite = false;
				break;
			}
			track_size_sum += *size;
		}
		const float free_space = definite ? std::max(0.f, *axis_inner_node_size - track_size_sum) : 0.f;
		const int weighted_track_count = int((tracks.size() - 3) / 2) * inner_gutter_weight + 2 * outer_gutter_weight;
		if (weighted_track_count <= 0)
			return 0.f;
		return (free_space / float(weighted_track_count)) * float(inner_gutter_weight);
	}

	void ResolveItemTrackIndexes(Vector<GridItem>& items, const TrackCounts& column_counts, const TrackCounts& row_counts)
	{
		for (GridItem& item : items)
		{
			item.column_indexes = {TryIntoTrackVecIndex(item.column.start, column_counts), TryIntoTrackVecIndex(item.column.end, column_counts)};
			item.row_indexes = {TryIntoTrackVecIndex(item.row.start, row_counts), TryIntoTrackVecIndex(item.row.end, row_counts)};
			RMLUI_ASSERT(item.column_indexes.start >= 0 && item.column_indexes.end >= 0 && item.row_indexes.start >= 0 && item.row_indexes.end >= 0);
		}
	}

	void DetermineIfItemCrossesFlexibleOrIntrinsicTracks(Vector<GridItem>& items, const GridTrackVec& columns, const GridTrackVec& rows)
	{
		for (GridItem& item : items)
		{
			item.crosses_flexible_column = item.crosses_intrinsic_column = false;
			for (int i = item.TrackRangeBegin(Axis::Horizontal); i < item.TrackRangeEnd(Axis::Horizontal); i++)
			{
				item.crosses_flexible_column |= columns[i].IsFlexible();
				item.crosses_intrinsic_column |= columns[i].HasIntrinsicSizingFunction();
			}
			item.crosses_flexible_row = item.crosses_intrinsic_row = false;
			for (int i = item.TrackRangeBegin(Axis::Vertical); i < item.TrackRangeEnd(Axis::Vertical); i++)
			{
				item.crosses_flexible_row |= rows[i].IsFlexible();
				item.crosses_intrinsic_row |= rows[i].HasIntrinsicSizingFunction();
			}
		}
	}

	void TrackSizingAlgorithm(Axis axis, OptionalFloat axis_min_size, OptionalFloat axis_max_size, ContentAlignment axis_alignment,
		ContentAlignment other_axis_alignment, AxisPair<AvailableSpace> available_grid_space, SizeOptional inner_node_size,
		GridTrackVec& axis_tracks, GridTrackVec& other_axis_tracks, Vector<GridItem>& items, TrackSizeEstimate estimate, bool has_baseline_aligned_item)
	{
		// 12.4 Initialize the track sizes.
		const OptionalFloat percentage_basis = inner_node_size.Get(axis) ? inner_node_size.Get(axis) : axis_min_size;
		InitializeTrackSizes(axis_tracks, percentage_basis);

		// The 'fit-content()' limits, against the container's inner size, fixed for this pass.
		const OptionalFloat axis_inner_node_size = inner_node_size.Get(axis);
		for (GridTrack& track : axis_tracks)
			track.ResolveFitContentLimit(axis_inner_node_size);

		// 12.5.1 Shim the baselines.
		if (has_baseline_aligned_item)
			ResolveItemBaselines(axis, items, inner_node_size);

		// When every track has a fixed minimum and a base size equal to its growth limit the sizes are final. Tracks with an
		// intrinsic minimum can still outgrow a fixed growth limit ('minmax(auto, 0px)').
		const bool all_final = std::all_of(axis_tracks.begin(), axis_tracks.end(), [&](const GridTrack& track) {
			return track.base_size == track.growth_limit && track.min_track_sizing_function.DefiniteValue(percentage_basis);
		});
		if (all_final)
			return;

		// What content alignment adds to each inner gutter of the other axis, for the items' size estimates.
		const float gutter_alignment_adjustment =
			ComputeAlignmentGutterAdjustment(other_axis_alignment, inner_node_size.Get(OtherAxis(axis)), estimate, other_axis_tracks);
		if (other_axis_tracks.size() > 3)
		{
			for (size_t i = 2; i < other_axis_tracks.size(); i += 2)
				other_axis_tracks[i].content_alignment_adjustment = gutter_alignment_adjustment;
		}

		// 12.5 Resolve the intrinsic track sizes.
		ResolveIntrinsicTrackSizes(axis, axis_tracks, other_axis_tracks, items, available_grid_space.Get(axis), inner_node_size, estimate);

		// 12.6 Maximize the tracks.
		MaximiseTracks(axis_tracks, available_grid_space.Get(axis));

		// The last two steps only expand into the space the container's size gives (its preferred size, or its parent through
		// stretching), not any available space: a definite available space is a max-content one without a size.
		AvailableSpace axis_available_space_for_expansion;
		if (inner_node_size.Get(axis))
			axis_available_space_for_expansion = AvailableSpace::Definite(*inner_node_size.Get(axis));
		else if (available_grid_space.Get(axis).type == AvailableSpace::Type::MinContent)
			axis_available_space_for_expansion = AvailableSpace::MinContent();
		else
			axis_available_space_for_expansion = AvailableSpace::MaxContent();

		// 12.7 Expand the flexible tracks.
		ExpandFlexibleTracks(axis, axis_tracks, other_axis_tracks, items, axis_min_size, axis_max_size, axis_available_space_for_expansion,
			inner_node_size, estimate);

		// 12.8 Stretch the 'auto' tracks.
		if (axis_alignment.keyword == ContentAlign::Stretch)
			StretchAutoTracks(axis_tracks, axis_min_size, axis_available_space_for_expansion);
	}

} // namespace GridLayout
} // namespace Rml
