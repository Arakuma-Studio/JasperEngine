#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "Easing.h"
#include "PropertyParserAnimation.h"
#include "PropertyShorthandDefinition.h"
#include <limits>

namespace Rml {

/*
    The 'transition' and 'animation' shorthands (phase 3, docs/html-ui/phase-3-fork-patches.md 3.13.2): a comma-separated
    list of entries whose components may come in any order. Each component goes to the first longhand, in the order of
    the CSS grammar, that takes it and has no value yet (so in 'animation: 1s linear linear' the second 'linear' is the
    name); the first time is the duration, the second the delay. Every longhand is set, those an entry omits to their
    initial value.
 */

namespace {

	struct LonghandLists {
		Vector<AnimationListItem> lists[9];
	};

	void SetList(PropertyDictionary& dictionary, const PropertySpecification& specification, PropertyId id, AnimationListValue::Kind kind,
		Vector<AnimationListItem>&& items)
	{
		Property property(StyleValuePtr(MakeShared<AnimationListValue>(kind, std::move(items))), Unit::STYLE_VALUE);
		property.definition = specification.GetProperty(id);
		property.parser_index = 0;
		dictionary.SetProperty(id, property);
	}

	AnimationListItem TimeItem(float seconds)
	{
		AnimationListItem item;
		item.number = seconds;
		return item;
	}

	AnimationListItem EasingItem(const Tween& tween)
	{
		AnimationListItem item;
		item.tween = tween;
		return item;
	}

	AnimationListItem KeywordItem(PropertyId id, int keyword)
	{
		AnimationListItem item;
		item.keyword = keyword;
		for (const auto& [name, value] : GetAnimationKeywords(id))
			if (value == keyword)
				item.name = name;
		return item;
	}

	bool MatchKeyword(PropertyId id, const String& token, int& keyword)
	{
		const ParameterMap& keywords = GetAnimationKeywords(id);
		auto it = keywords.find(StringUtilities::ToLower(token));
		if (it == keywords.end())
			return false;
		keyword = it->second;
		return true;
	}

} // namespace

bool ParseTransitionShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& /*definition*/, const String& value,
	const PropertySpecification& specification)
{
	StringList entries;
	SplitCommaList(value, entries);
	if (entries.empty())
		return false;

	const PropertyParserAnimation property_parser(AnimationListValue::Kind::Property);
	Vector<AnimationListItem> properties, durations, easings, delays, behaviors;

	for (const String& entry : entries)
	{
		StringList tokens;
		SplitWhitespaceList(entry, tokens);
		if (tokens.empty())
			return false;

		Vector<AnimationListItem> names;
		bool duration_set = false, delay_set = false, easing_set = false, behavior_set = false;
		float duration = 0.f, delay = 0.f;
		Tween easing = Easing::Ease();
		int behavior = 0;

		for (const String& token : tokens)
		{
			float seconds = 0.f;
			Tween tween;
			int keyword = 0;
			AnimationListItem name;
			if (ParseAnimationTime(token, seconds))
			{
				if (!duration_set)
				{
					if (seconds < 0.f)
						return false;
					duration = seconds;
					duration_set = true;
				}
				else if (!delay_set)
				{
					delay = seconds;
					delay_set = true;
				}
				else
					return false;
			}
			else if (!easing_set && Easing::Parse(token, tween))
			{
				easing = tween;
				easing_set = true;
			}
			else if (!behavior_set && MatchKeyword(PropertyId::TransitionBehavior, token, keyword))
			{
				behavior = keyword;
				behavior_set = true;
			}
			else if (property_parser.ParseItem(name, token, ParameterMap()))
			{
				// RmlUi accepts several property names in one entry ('opacity width 1s'), each transitioned with its timing.
				names.push_back(std::move(name));
			}
			else
				return false;
		}

		if (names.empty())
		{
			AnimationListItem all;
			all.name = "all";
			all.all = true;
			names.push_back(std::move(all));
		}

		for (AnimationListItem& name : names)
		{
			// 'none' is valid only as the single entry. Several names in an entry (RmlUi's syntax) must all be properties.
			if (name.name == "none" && (entries.size() > 1 || names.size() > 1))
				return false;
			if (names.size() > 1 && (name.all || name.properties.Empty()))
				return false;
			properties.push_back(std::move(name));
			durations.push_back(TimeItem(duration));
			easings.push_back(EasingItem(easing));
			delays.push_back(TimeItem(delay));
			behaviors.push_back(KeywordItem(PropertyId::TransitionBehavior, behavior));
		}
	}

	using Kind = AnimationListValue::Kind;
	SetList(dictionary, specification, PropertyId::TransitionProperty, Kind::Property, std::move(properties));
	SetList(dictionary, specification, PropertyId::TransitionDuration, Kind::Time, std::move(durations));
	SetList(dictionary, specification, PropertyId::TransitionTimingFunction, Kind::Easing, std::move(easings));
	SetList(dictionary, specification, PropertyId::TransitionDelay, Kind::Time, std::move(delays));
	SetList(dictionary, specification, PropertyId::TransitionBehavior, Kind::Keyword, std::move(behaviors));
	return true;
}

bool ParseAnimationShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& /*definition*/, const String& value,
	const PropertySpecification& specification)
{
	StringList entries;
	SplitCommaList(value, entries);
	if (entries.empty())
		return false;

	const PropertyParserAnimation name_parser(AnimationListValue::Kind::Name);
	const PropertyParserAnimation count_parser(AnimationListValue::Kind::IterationCount);
	Vector<AnimationListItem> names, durations, easings, delays, counts, directions, fill_modes, play_states;

	for (const String& entry : entries)
	{
		StringList tokens;
		SplitWhitespaceList(entry, tokens);
		if (tokens.empty())
			return false;

		bool duration_set = false, delay_set = false, easing_set = false, count_set = false, direction_set = false, fill_set = false,
			 state_set = false, name_set = false;
		float duration = 0.f, delay = 0.f;
		Tween easing = Easing::Ease();
		AnimationListItem count;
		count.number = 1.f;
		AnimationListItem name;
		int direction = 0, fill_mode = 0, play_state = 0;

		for (const String& token : tokens)
		{
			float seconds = 0.f;
			Tween tween;
			int keyword = 0;
			AnimationListItem item;
			if (ParseAnimationTime(token, seconds))
			{
				if (!duration_set)
				{
					if (seconds < 0.f)
						return false;
					duration = seconds;
					duration_set = true;
				}
				else if (!delay_set)
				{
					delay = seconds;
					delay_set = true;
				}
				else
					return false;
			}
			else if (!easing_set && Easing::Parse(token, tween))
			{
				easing = tween;
				easing_set = true;
			}
			else if (!count_set && count_parser.ParseItem(item, token, ParameterMap()))
			{
				count = std::move(item);
				count_set = true;
			}
			else if (!direction_set && MatchKeyword(PropertyId::AnimationDirection, token, keyword))
			{
				direction = keyword;
				direction_set = true;
			}
			else if (!fill_set && MatchKeyword(PropertyId::AnimationFillMode, token, keyword))
			{
				fill_mode = keyword;
				fill_set = true;
			}
			else if (!state_set && MatchKeyword(PropertyId::AnimationPlayState, token, keyword))
			{
				play_state = keyword;
				state_set = true;
			}
			else if (!name_set && name_parser.ParseItem(item, token, ParameterMap()))
			{
				name = std::move(item);
				name_set = true;
			}
			else
				return false;
		}

		names.push_back(std::move(name));
		durations.push_back(TimeItem(duration));
		easings.push_back(EasingItem(easing));
		delays.push_back(TimeItem(delay));
		counts.push_back(std::move(count));
		directions.push_back(KeywordItem(PropertyId::AnimationDirection, direction));
		fill_modes.push_back(KeywordItem(PropertyId::AnimationFillMode, fill_mode));
		play_states.push_back(KeywordItem(PropertyId::AnimationPlayState, play_state));
	}

	using Kind = AnimationListValue::Kind;
	SetList(dictionary, specification, PropertyId::AnimationName, Kind::Name, std::move(names));
	SetList(dictionary, specification, PropertyId::AnimationDuration, Kind::Time, std::move(durations));
	SetList(dictionary, specification, PropertyId::AnimationTimingFunction, Kind::Easing, std::move(easings));
	SetList(dictionary, specification, PropertyId::AnimationDelay, Kind::Time, std::move(delays));
	SetList(dictionary, specification, PropertyId::AnimationIterationCount, Kind::IterationCount, std::move(counts));
	SetList(dictionary, specification, PropertyId::AnimationDirection, Kind::Keyword, std::move(directions));
	SetList(dictionary, specification, PropertyId::AnimationFillMode, Kind::Keyword, std::move(fill_modes));
	SetList(dictionary, specification, PropertyId::AnimationPlayState, Kind::Keyword, std::move(play_states));
	return true;
}

} // namespace Rml
