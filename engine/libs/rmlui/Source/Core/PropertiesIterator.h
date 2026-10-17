#pragma once

#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "Cascade.h"
#include "ElementStyle.h"
#include "PropertyShorthandDefinition.h"
#include <variant>

namespace Rml {

// An iterator for local properties defined on an element, each property once, with the value the cascade gives it
// (ElementStyle::GetLocalProperty): its running transition, else an important declaration (inline, then the
// definition's), else its running animation, else the inline declaration, else the definition's (phase 3, Cascade.h).
// Without important declarations the two importance ranges are left out.
// Note: Modifying the underlying style invalidates the iterator.
class PlainPropertiesIterator {
public:
	using ValueType = Pair<PropertyId, const Property&>;
	using PropertyIt = PropertyMap::const_iterator;

	PlainPropertiesIterator(const PropertyMap& inline_properties, const PropertyMap* definition_properties,
		const PropertyMap* transition_properties = nullptr, const PropertyMap* animated_properties = nullptr, bool any_important = false)
	{
		AddRange(transition_properties, false);
		if (any_important)
		{
			AddRange(&inline_properties, true);
			AddRange(definition_properties, true);
		}
		AddRange(animated_properties, false);
		AddRange(&inline_properties, false);
		AddRange(definition_properties, false);
		ProceedToNextValid();
	}

	PlainPropertiesIterator& operator++()
	{
		++ranges[current].it;
		ProceedToNextValid();
		return *this;
	}

	ValueType operator*() const { return {ranges[current].it->first, ranges[current].it->second}; }

	bool AtEnd() const { return current >= num_ranges; }

private:
	struct Range {
		PropertyIt it, end;
		bool only_important;
	};
	Range ranges[6];
	// The map of each range (for InEarlierRange; not in Range, which the loop below reads: kept as small as it was).
	const PropertyMap* range_maps[6];
	int num_ranges = 0;
	int current = 0;
	// The visited ids below PropertyIdSet's inline ids: every id RmlUi and the engine define (fork, phase 5: the
	// registered custom properties' ids above them are looked up in the earlier ranges instead, see IsFirstVisit; a
	// PropertyIdSet here, with its heap part for those ids, made the iterator bigger and its loop slower).
	static constexpr size_t NumVisitedIds = 256;
	uint64_t visited_ids[NumVisitedIds / 64] = {};

	void AddRange(const PropertyMap* properties, bool only_important)
	{
		if (properties && !properties->empty())
		{
			range_maps[num_ranges] = properties;
			ranges[num_ranges++] = Range{properties->begin(), properties->end(), only_important};
		}
	}

	bool IsFirstVisit(PropertyId id)
	{
		const size_t index = size_t(id);
		if (index < NumVisitedIds)
		{
			const uint64_t bit = uint64_t(1) << (index % 64);
			uint64_t& word = visited_ids[index / 64];
			if (word & bit)
				return false;
			word |= bit;
			return true;
		}
		return !InEarlierRange(id);
	}

	// A registered custom property: visited if an earlier range yielded it. (Out of line: the loop above stays as small
	// as it was.)
#if defined(_MSC_VER)
	__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
	__attribute__((noinline))
#endif
	bool InEarlierRange(PropertyId id) const
	{
		for (int i = 0; i < current; i++)
		{
			auto it = range_maps[i]->find(id);
			if (it != range_maps[i]->end() && (!ranges[i].only_important || it->second.important))
				return true;
		}
		return false;
	}

	void ProceedToNextValid()
	{
		for (; current < num_ranges; current++)
		{
			for (Range& range = ranges[current]; range.it != range.end; ++range.it)
			{
				if (range.only_important && !range.it->second.important)
					continue;
				if (IsFirstVisit(range.it->first))
					return;
			}
		}
	}
};

class AllPropertiesIterator {
public:
	using ValueType = Pair<String, const Property&>;
	using PlainIt = PropertyMap::const_iterator;
	using CustomIt = UnorderedMap<String, Property>::const_iterator;
	using ShorthandIt = UnorderedMap<ShorthandId, Property>::const_iterator;

	AllPropertiesIterator(const PropertyDictionary& inline_properties, const PropertyDictionary* definition_properties,
		ElementStyle* filter_inherited_by) :
		inline_properties{inline_properties}, definition_properties{definition_properties}, filter_inherited_by{filter_inherited_by}
	{
		SetIterators(inline_properties);
		UpdatePhase();
		SkipFilter();
	}

	AllPropertiesIterator& operator++()
	{
		Increment();
		UpdatePhase();
		SkipFilter();
		return *this;
	}

	ValueType operator*() const
	{
		if (it_plain != it_plain_end)
			return {StyleSheetSpecification::GetPropertyName(it_plain->first), it_plain->second};
		if (it_custom != it_custom_end)
			return {it_custom->first, it_custom->second};
		RMLUI_ASSERT(it_shorthand != it_shorthand_end);
		return {StyleSheetSpecification::GetShorthandName(it_shorthand->first), it_shorthand->second};
	}

	bool AtEnd() const { return phase == Phase::End; }

private:
	void Increment()
	{
		RMLUI_ASSERT(!AtEnd());
		if (it_plain != it_plain_end)
			++it_plain;
		else if (it_custom != it_custom_end)
			++it_custom;
		else if (it_shorthand != it_shorthand_end)
			++it_shorthand;
	}

	void SkipFilter()
	{
		while (true)
		{
			bool increment = false;
			if (HasVisitedCurrent())
			{
				increment = true;
			}
			else if (filter_inherited_by && !AtEnd())
			{
				if (it_plain != it_plain_end)
					increment = (filter_inherited_by->GetSpecifiedProperty(it_plain->first) != &it_plain->second);
				else if (it_custom != it_custom_end)
					increment = (filter_inherited_by->GetSpecifiedCustomProperty(it_custom->first) != &it_custom->second);
				else if (it_shorthand != it_shorthand_end)
					increment = (filter_inherited_by->GetSpecifiedShorthand(it_shorthand->first) != &it_shorthand->second);
			}

			if (!increment && it_plain != it_plain_end)
			{
				// Skip shorthand placeholders, they are defined directly by shorthands which are iterated over later.
				increment = (it_plain->second.unit == Unit::SHORTHAND_PLACEHOLDER);
			}

			if (increment)
			{
				Increment();
				UpdatePhase();
			}
			else
			{
				break;
			}
		}
	}

	void UpdatePhase()
	{
		if (it_plain != it_plain_end)
			return;
		if (it_custom != it_custom_end)
			return;
		if (it_shorthand != it_shorthand_end)
			return;

		if (phase == Phase::InlineProperties && definition_properties)
		{
			SetIterators(*definition_properties);
			phase = Phase::DefinitionProperties;
			UpdatePhase();
		}
		else
		{
			phase = Phase::End;
		}
	}

	// Skips the declaration of the current phase that the other source overrides (Cascade.h): in the inline phase an
	// inline declaration that an important definition declaration beats, in the definition phase one the inline style
	// beats.
	bool HasVisitedCurrent() const
	{
		if (phase == Phase::End)
			return false;
		const PropertyDictionary* other = (phase == Phase::InlineProperties ? definition_properties : &inline_properties);
		if (!other)
			return false;

		const Property* current_property = nullptr;
		const Property* other_property = nullptr;
		if (it_plain != it_plain_end)
		{
			current_property = &it_plain->second;
			other_property = other->GetProperty(it_plain->first);
		}
		else if (it_custom != it_custom_end)
		{
			current_property = &it_custom->second;
			other_property = other->GetCustomProperty(it_custom->first);
		}
		else
		{
			RMLUI_ASSERT(it_shorthand != it_shorthand_end);
			current_property = &it_shorthand->second;
			other_property = other->GetVarShorthand(it_shorthand->first);
		}
		if (!other_property)
			return false;

		if (phase == Phase::InlineProperties)
			return !Cascade::InlineWins(*current_property, *other_property);
		return Cascade::InlineWins(*other_property, *current_property);
	}

	void SetIterators(const PropertyDictionary& dictionary)
	{
		it_plain = dictionary.GetProperties().begin();
		it_plain_end = dictionary.GetProperties().end();
		it_custom = dictionary.GetCustomProperties().begin();
		it_custom_end = dictionary.GetCustomProperties().end();
		it_shorthand = dictionary.GetVarShorthands().begin();
		it_shorthand_end = dictionary.GetVarShorthands().end();
	}

	enum class Phase { InlineProperties, DefinitionProperties, End };
	Phase phase = Phase::InlineProperties;

	const PropertyDictionary& inline_properties;
	const PropertyDictionary* definition_properties;
	ElementStyle* filter_inherited_by;

	PlainIt it_plain, it_plain_end;
	CustomIt it_custom, it_custom_end;
	ShorthandIt it_shorthand, it_shorthand_end;
};

} // namespace Rml
