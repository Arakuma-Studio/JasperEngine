#pragma once

#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "ElementStyle.h"
#include "PropertyShorthandDefinition.h"
#include <variant>

namespace Rml {

// An iterator for local properties defined on an element: the values of its transitions and animations (the animation
// layers, phase 3), its inline style and its definition, each property once, from the first source in that order that has it.
// Note: Modifying the underlying style invalidates the iterator.
class PlainPropertiesIterator {
public:
	using ValueType = Pair<PropertyId, const Property&>;
	using PropertyIt = PropertyMap::const_iterator;

	PlainPropertiesIterator(const PropertyMap& inline_properties, const PropertyMap* definition_properties,
		const PropertyMap* transition_properties = nullptr, const PropertyMap* animated_properties = nullptr)
	{
		for (const PropertyMap* properties : {transition_properties, animated_properties, &inline_properties, definition_properties})
		{
			if (properties && !properties->empty())
				ranges[num_ranges++] = Range{properties->begin(), properties->end()};
		}
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
	};
	Range ranges[4];
	int num_ranges = 0;
	int current = 0;
	PropertyIdSet visited_properties;

	bool IsFirstVisit(PropertyId id)
	{
		if (!visited_properties.Contains(id))
		{
			visited_properties.Insert(id);
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

	bool HasVisitedCurrent() const
	{
		if (phase != Phase::DefinitionProperties)
			return false;

		if (it_plain != it_plain_end)
			return inline_properties.GetProperties().count(it_plain->first);
		if (it_custom != it_custom_end)
			return inline_properties.GetCustomProperties().count(it_custom->first);

		RMLUI_ASSERT(it_shorthand != it_shorthand_end);
		return inline_properties.GetVarShorthands().count(it_shorthand->first);
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
