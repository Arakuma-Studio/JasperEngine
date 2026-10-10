#include "PropertyParserAnimation.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/StyleSheetTypes.h"
#include "ControlledLifetimeResource.h"
#include "Easing.h"
#include "ElementStyle.h"
#include "IndividualTransforms.h"
#include "PropertyShorthandDefinition.h"
#include <limits>
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "CalculationParser.h"
#endif

namespace Rml {

namespace {

	bool IsIdentifierStart(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (unsigned char)c >= 0x80;
	}

	bool IsIdentifierCharacter(char c)
	{
		return IsIdentifierStart(c) || (c >= '0' && c <= '9') || c == '-';
	}

	// A CSS identifier (escapes aside): '-'? [start] [char]*, or '--' [char]*.
	bool IsIdentifier(const String& text)
	{
		size_t i = 0;
		if (text.size() >= 2 && text[0] == '-' && text[1] == '-')
			i = 2;
		else
		{
			if (i < text.size() && text[i] == '-')
				i++;
			if (i >= text.size() || !IsIdentifierStart(text[i]))
				return false;
			i++;
		}
		for (; i < text.size(); i++)
			if (!IsIdentifierCharacter(text[i]))
				return false;
		return true;
	}

	bool IsCssWideKeyword(const String& lower)
	{
		return lower == "initial" || lower == "inherit" || lower == "unset" || lower == "revert" || lower == "revert-layer" || lower == "default";
	}

	// A quoted string: its content, with backslash escapes of single characters removed.
	bool ParseQuotedString(const String& text, String& result)
	{
		if (text.size() < 2 || (text[0] != '"' && text[0] != '\'') || text.back() != text[0])
			return false;
		result.clear();
		for (size_t i = 1; i + 1 < text.size(); i++)
		{
			char c = text[i];
			if (c == '\\' && i + 2 < text.size())
				c = text[++i];
			else if (c == text[0])
				return false;
			result += c;
		}
		return true;
	}

	String QuoteIfNeeded(const String& name)
	{
		if (IsIdentifier(name) && !IsCssWideKeyword(StringUtilities::ToLower(name)) && StringUtilities::ToLower(name) != "none")
			return name;
		String result = "\"";
		for (char c : name)
		{
			if (c == '"' || c == '\\')
				result += '\\';
			result += c;
		}
		return result + '"';
	}

	String FormatNumber(float value)
	{
		String result = CreateString("%g", value);
		return result == "-0" ? String("0") : result;
	}

#ifdef RMLUI_MATH_EXPRESSIONS
	bool BeginsMathFunction(const String& argument)
	{
		const String lower = StringUtilities::ToLower(argument);
		for (const char* name : {"calc(", "min(", "max(", "clamp("})
			if (lower.rfind(name, 0) == 0)
				return true;
		return false;
	}

	bool ParseMathArgument(const String& argument, bool allow_number, CalculationTypeMask& final_type, float& value)
	{
		CalculationPtr calculation;
		CalculationParseTarget target = MakeCalculationParseTarget(CalculationFinalType::Time);
		if (allow_number)
			target.allowed_final_types |= CalculationTypeMask(CalculationFinalType::Number);
		if (!ParseCalculation(argument, target, calculation))
			return false;
		final_type = GetCalculationFinalType(*calculation);
		if (final_type == CalculationTypeMask(CalculationFinalType::Time))
			return EvaluateCalculationTime(*calculation, value);
		if (final_type == CalculationTypeMask(CalculationFinalType::Number))
		{
			CalculationConstantValue constant;
			if (!EvaluateCalculation(*calculation, constant) || constant.unit != Unit::NUMBER)
				return false;
			value = constant.value;
			return true;
		}
		return false;
	}
#endif

	struct KeywordTable {
		PropertyId id;
		const char* keywords;
	};

	// The keywords of the keyword longhands, in the order of their values.
	const KeywordTable keyword_tables[] = {
		{PropertyId::TransitionBehavior, "normal, allow-discrete"},
		{PropertyId::AnimationDirection, "normal, reverse, alternate, alternate-reverse"},
		{PropertyId::AnimationFillMode, "none, forwards, backwards, both"},
		{PropertyId::AnimationPlayState, "running, paused"},
		{PropertyId::AnimationComposition, "replace, add, accumulate"},
	};

	struct AnimationParserData {
		PropertyParserAnimation transition_property{AnimationListValue::Kind::Property};
		PropertyParserAnimation duration{AnimationListValue::Kind::Time, true};
		PropertyParserAnimation time{AnimationListValue::Kind::Time};
		PropertyParserAnimation easing{AnimationListValue::Kind::Easing};
		PropertyParserAnimation iteration_count{AnimationListValue::Kind::IterationCount};
		PropertyParserAnimation animation_name{AnimationListValue::Kind::Name};
		PropertyParserAnimation keyword{AnimationListValue::Kind::Keyword};

		UnorderedMap<PropertyId, ParameterMap> keywords;
		ParameterMap no_keywords;
		PropertyIdSet transition_ids;
		PropertyIdSet animation_ids;

		AnimationParserData()
		{
			for (const KeywordTable& table : keyword_tables)
			{
				StringList list;
				StringUtilities::ExpandString(list, table.keywords);
				ParameterMap& map = keywords[table.id];
				for (int i = 0; i < (int)list.size(); i++)
					map[list[i]] = i;
			}
			for (PropertyId id : {PropertyId::TransitionProperty, PropertyId::TransitionDuration, PropertyId::TransitionTimingFunction,
					 PropertyId::TransitionDelay, PropertyId::TransitionBehavior})
				transition_ids.Insert(id);
			for (PropertyId id : {PropertyId::AnimationName, PropertyId::AnimationDuration, PropertyId::AnimationTimingFunction,
					 PropertyId::AnimationDelay, PropertyId::AnimationIterationCount, PropertyId::AnimationDirection, PropertyId::AnimationFillMode,
					 PropertyId::AnimationPlayState, PropertyId::AnimationComposition})
				animation_ids.Insert(id);
		}
	};

	ControlledLifetimeResource<AnimationParserData> parser_data;

	const char* KeywordsOf(PropertyId id)
	{
		for (const KeywordTable& table : keyword_tables)
			if (table.id == id)
				return table.keywords;
		return "";
	}

	// The items of a longhand of the style, or of its initial value.
	const Vector<AnimationListItem>* GetItems(const ElementStyle& style, PropertyId id)
	{
		const Property* property = style.GetLocalPropertyWithResolvedVariables(id);
		if (!property || property->unit != Unit::STYLE_VALUE)
		{
			const PropertyDefinition* definition = StyleSheetSpecification::GetProperty(id);
			property = (definition ? definition->GetDefaultValue() : nullptr);
		}
		if (!property || property->unit != Unit::STYLE_VALUE)
			return nullptr;
		const StyleValuePtr value = property->value.Get<StyleValuePtr>();
		if (!value)
			return nullptr;
		return &static_cast<const AnimationListValue&>(*value).GetItems();
	}

	// The item of a list at an index, repeating the list (CSS list-length rules).
	const AnimationListItem* ItemAt(const Vector<AnimationListItem>* items, size_t index)
	{
		if (!items || items->empty())
			return nullptr;
		return &(*items)[index % items->size()];
	}

} // namespace

bool AnimationListItem::operator==(const AnimationListItem& other) const
{
	// The properties of a transition-property item follow from its name.
	return number == other.number && keyword == other.keyword && tween == other.tween && name == other.name && all == other.all;
}

AnimationListValue::AnimationListValue(Kind kind, Vector<AnimationListItem> items) : kind(kind), items(std::move(items)) {}

String AnimationListValue::ToString() const
{
	String result;
	for (size_t i = 0; i < items.size(); i++)
	{
		if (i > 0)
			result += ", ";
		const AnimationListItem& item = items[i];
		switch (kind)
		{
		case Kind::Property: result += item.name; break;
		case Kind::Time: result += FormatNumber(item.number) + "s"; break;
		case Kind::Easing: result += item.tween.to_string(); break;
		case Kind::IterationCount: result += (std::isinf(item.number) ? String("infinite") : FormatNumber(item.number)); break;
		case Kind::Name: result += (item.name.empty() ? String("none") : QuoteIfNeeded(item.name)); break;
		case Kind::Keyword: result += item.name; break;
		}
	}
	return result;
}

bool AnimationListValue::Equals(const StyleValue& other) const
{
	const AnimationListValue& value = static_cast<const AnimationListValue&>(other);
	return kind == value.kind && items == value.items;
}

bool ParseAnimationTime(const String& value, float& seconds)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (BeginsMathFunction(value))
	{
		CalculationTypeMask final_type = 0;
		return ParseMathArgument(value, false, final_type, seconds) && final_type == CalculationTypeMask(CalculationFinalType::Time);
	}
#endif
	const String lower = StringUtilities::ToLower(value);
	float scale = 1.f;
	size_t unit_length = 0;
	if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "ms") == 0)
	{
		scale = 0.001f;
		unit_length = 2;
	}
	else if (lower.size() > 1 && lower.back() == 's')
		unit_length = 1;
	else
		return false;

	float number = 0.f;
	if (!Easing::ParseNumber(lower.substr(0, lower.size() - unit_length), number))
		return false;
	seconds = number * scale;
	return true;
}

const ParameterMap& GetAnimationKeywords(PropertyId id)
{
	auto it = parser_data->keywords.find(id);
	return it != parser_data->keywords.end() ? it->second : parser_data->no_keywords;
}

PropertyParserAnimation::PropertyParserAnimation(Kind kind, bool non_negative) : kind(kind), non_negative(non_negative) {}

bool PropertyParserAnimation::ParseValue(Property& property, const String& value, const ParameterMap& parameters) const
{
	StringList list;
	SplitCommaList(value, list);
	if (list.empty())
		return false;

	Vector<AnimationListItem> items;
	items.reserve(list.size());
	for (const String& text : list)
	{
		AnimationListItem item;
		if (!ParseItem(item, text, parameters))
			return false;
		items.push_back(std::move(item));
	}

	// 'none' is valid in transition-property only as the whole list.
	if (kind == Kind::Property && items.size() > 1)
		for (const AnimationListItem& item : items)
			if (item.name == "none")
				return false;

	property.value = StyleValuePtr(MakeShared<AnimationListValue>(kind, std::move(items)));
	property.unit = Unit::STYLE_VALUE;
	return true;
}

bool PropertyParserAnimation::ParseItem(AnimationListItem& item, const String& value, const ParameterMap& parameters) const
{
	if (value.empty())
		return false;

	switch (kind)
	{
	case Kind::Property:
	{
		const String lower = StringUtilities::ToLower(value);
		if (!IsIdentifier(value) || IsCssWideKeyword(lower))
			return false;
		item.name = lower;
		if (lower == "all")
			item.all = true;
		else if (lower != "none")
		{
			if (const ShorthandDefinition* shorthand = StyleSheetSpecification::GetShorthand(lower))
				item.properties = StyleSheetSpecification::GetShorthandUnderlyingProperties(shorthand->id);
			else if (const PropertyDefinition* definition = StyleSheetSpecification::GetProperty(lower))
				item.properties.Insert(definition->GetId());
			// Unknown property names are kept: they take their place in the lists, and transition nothing.
		}
		return true;
	}
	case Kind::Time:
		if (!ParseAnimationTime(value, item.number))
			return false;
		return !(non_negative && item.number < 0.f);
	case Kind::Easing: return Easing::Parse(value, item.tween);
	case Kind::IterationCount:
	{
		const String lower = StringUtilities::ToLower(value);
		if (lower == "infinite")
		{
			item.number = std::numeric_limits<float>::infinity();
			return true;
		}
		float number = 0.f;
#ifdef RMLUI_MATH_EXPRESSIONS
		if (BeginsMathFunction(value))
		{
			CalculationTypeMask final_type = 0;
			if (!ParseMathArgument(value, true, final_type, number) || final_type != CalculationTypeMask(CalculationFinalType::Number))
				return false;
		}
		else
#endif
			if (!Easing::ParseNumber(value, number))
			return false;
		if (number < 0.f)
			return false;
		item.number = number;
		return true;
	}
	case Kind::Name:
	{
		if (ParseQuotedString(value, item.name))
			return true;
		const String lower = StringUtilities::ToLower(value);
		if (lower == "none")
		{
			item.name.clear();
			return true;
		}
		if (!IsIdentifier(value) || IsCssWideKeyword(lower))
			return false;
		item.name = value;
		return true;
	}
	case Kind::Keyword:
	{
		const String lower = StringUtilities::ToLower(value);
		auto it = parameters.find(lower);
		if (it == parameters.end())
			return false;
		item.keyword = it->second;
		item.name = lower;
		return true;
	}
	}
	return false;
}

void PropertyParserAnimation::Initialize()
{
	parser_data.Initialize();
	PropertyParserIndividualTransform::Initialize();
}

void PropertyParserAnimation::Shutdown()
{
	PropertyParserIndividualTransform::Shutdown();
	parser_data.Shutdown();
}

void PropertyParserAnimation::RegisterParsers()
{
	StyleSheetSpecification::RegisterParser("transition_property", &parser_data->transition_property);
	StyleSheetSpecification::RegisterParser("animation_duration", &parser_data->duration);
	StyleSheetSpecification::RegisterParser("animation_time", &parser_data->time);
	StyleSheetSpecification::RegisterParser("animation_easing", &parser_data->easing);
	StyleSheetSpecification::RegisterParser("animation_iteration_count", &parser_data->iteration_count);
	StyleSheetSpecification::RegisterParser("animation_name", &parser_data->animation_name);
	StyleSheetSpecification::RegisterParser("animation_keyword", &parser_data->keyword);
	PropertyParserIndividualTransform::RegisterParsers();
}

TransitionList BuildTransitionList(const ElementStyle& style)
{
	TransitionList result;
	const Vector<AnimationListItem>* properties = GetItems(style, PropertyId::TransitionProperty);
	if (!properties)
		return result;
	const Vector<AnimationListItem>* durations = GetItems(style, PropertyId::TransitionDuration);
	const Vector<AnimationListItem>* easings = GetItems(style, PropertyId::TransitionTimingFunction);
	const Vector<AnimationListItem>* delays = GetItems(style, PropertyId::TransitionDelay);
	const Vector<AnimationListItem>* behaviors = GetItems(style, PropertyId::TransitionBehavior);

	for (size_t i = 0; i < properties->size(); i++)
	{
		const AnimationListItem& property = (*properties)[i];
		if (!property.all && property.properties.Empty())
			continue;

		Transition transition;
		if (const AnimationListItem* item = ItemAt(durations, i))
			transition.duration = item->number;
		if (const AnimationListItem* item = ItemAt(easings, i))
			transition.tween = item->tween;
		if (const AnimationListItem* item = ItemAt(delays, i))
			transition.delay = item->number;
		if (const AnimationListItem* item = ItemAt(behaviors, i))
			transition.allow_discrete = (item->keyword == 1);

		// A transition whose combined duration is zero does nothing.
		if (Math::Max(transition.duration, 0.f) + transition.delay <= 0.f)
			continue;

		if (property.all)
		{
			transition.id = PropertyId::Invalid;
			result.all = true;
			result.transitions.push_back(transition);
		}
		else
		{
			for (const PropertyId id : property.properties)
			{
				transition.id = id;
				result.transitions.push_back(transition);
			}
		}
	}
	result.none = result.transitions.empty();
	return result;
}

AnimationList BuildAnimationList(const ElementStyle& style)
{
	AnimationList result;
	const Vector<AnimationListItem>* names = GetItems(style, PropertyId::AnimationName);
	if (!names)
		return result;
	const Vector<AnimationListItem>* durations = GetItems(style, PropertyId::AnimationDuration);
	const Vector<AnimationListItem>* easings = GetItems(style, PropertyId::AnimationTimingFunction);
	const Vector<AnimationListItem>* delays = GetItems(style, PropertyId::AnimationDelay);
	const Vector<AnimationListItem>* counts = GetItems(style, PropertyId::AnimationIterationCount);
	const Vector<AnimationListItem>* directions = GetItems(style, PropertyId::AnimationDirection);
	const Vector<AnimationListItem>* fill_modes = GetItems(style, PropertyId::AnimationFillMode);
	const Vector<AnimationListItem>* play_states = GetItems(style, PropertyId::AnimationPlayState);

	for (size_t i = 0; i < names->size(); i++)
	{
		const AnimationListItem& name = (*names)[i];
		if (name.name.empty())
			continue;

		Animation animation;
		animation.name = name.name;
		if (const AnimationListItem* item = ItemAt(durations, i))
			animation.duration = item->number;
		if (const AnimationListItem* item = ItemAt(easings, i))
			animation.tween = item->tween;
		if (const AnimationListItem* item = ItemAt(delays, i))
			animation.delay = item->number;
		if (const AnimationListItem* item = ItemAt(counts, i))
			animation.iteration_count = item->number;
		if (const AnimationListItem* item = ItemAt(directions, i))
			animation.direction = AnimationDirection(item->keyword);
		if (const AnimationListItem* item = ItemAt(fill_modes, i))
			animation.fill_mode = AnimationFillMode(item->keyword);
		if (const AnimationListItem* item = ItemAt(play_states, i))
			animation.paused = (item->keyword == 1);
		result.push_back(std::move(animation));
	}
	return result;
}

void ProcessKeyframeTimingFunctions(Vector<KeyframeBlock>& blocks)
{
	for (KeyframeBlock& block : blocks)
	{
		if (const Property* property = block.properties.GetProperty(PropertyId::AnimationTimingFunction))
		{
			if (property->unit == Unit::STYLE_VALUE)
			{
				if (const StyleValuePtr value = property->value.Get<StyleValuePtr>())
				{
					const auto& items = static_cast<const AnimationListValue&>(*value).GetItems();
					if (!items.empty())
					{
						block.has_tween = true;
						block.tween = items[0].tween;
					}
				}
			}
		}
		for (const PropertyId id : parser_data->transition_ids)
			block.properties.RemoveProperty(id);
		for (const PropertyId id : parser_data->animation_ids)
			block.properties.RemoveProperty(id);
	}
}

const PropertyIdSet& GetTransitionLonghandIds()
{
	return parser_data->transition_ids;
}

const PropertyIdSet& GetAnimationLonghandIds()
{
	return parser_data->animation_ids;
}

const char* GetAnimationKeywordList(PropertyId id)
{
	return KeywordsOf(id);
}

} // namespace Rml
