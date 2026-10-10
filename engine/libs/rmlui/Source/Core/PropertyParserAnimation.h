#pragma once

#include "../../Include/RmlUi/Core/Animation.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#include "../../Include/RmlUi/Core/Tween.h"

namespace Rml {

class ElementStyle;
class PropertyDictionary;
class PropertySpecification;
class StyleSheetSpecification;
struct ShorthandDefinition;

/**
    The transition and animation longhands (phase 3, docs/html-ui/phase-3-fork-patches.md 3.13.2-3.13.3): each is a
    comma-separated list, stored as an AnimationListValue. The 'transition' and 'animation' shorthands
    (ShorthandAnimation.cpp) set them all; the element's computed TransitionList and AnimationList are built from them
    with the CSS list-length rules (BuildTransitionList, BuildAnimationList).
 */

/// One item of a transition-* or animation-* longhand.
struct AnimationListItem {
	float number = 0.f;       // A time in seconds, or an iteration count (infinite: INFINITY).
	int keyword = 0;          // The keyword's value (direction, fill mode, play state, behavior, composition).
	Tween tween;              // An easing function.
	String name;              // An animation name ("" for none), a keyword or a transition property as written.
	PropertyIdSet properties; // transition-property: the properties the name covers (empty for none, all and unknown names).
	bool all = false;         // transition-property: 'all'.

	bool operator==(const AnimationListItem& other) const;
	bool operator!=(const AnimationListItem& other) const { return !(*this == other); }
};

/// The value of a transition-* or animation-* longhand: its list of items.
class AnimationListValue final : public StyleValue {
public:
	enum class Kind : uint8_t { Property, Time, Easing, IterationCount, Name, Keyword };

	AnimationListValue(Kind kind, Vector<AnimationListItem> items);

	Kind GetKind() const { return kind; }
	const Vector<AnimationListItem>& GetItems() const { return items; }

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;

private:
	Kind kind;
	Vector<AnimationListItem> items;
};

/// Parses a transition or animation longhand's list; the parameters of a keyword list are its keywords.
class PropertyParserAnimation : public PropertyParser {
public:
	using Kind = AnimationListValue::Kind;

	/// @param[in] kind The kind of items of the list.
	/// @param[in] non_negative Times: negative values are invalid (durations).
	PropertyParserAnimation(Kind kind, bool non_negative = false);

	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;

	/// Parses one item of a list. Keywords are matched without regard to case.
	bool ParseItem(AnimationListItem& item, const String& value, const ParameterMap& parameters) const;

	static void Initialize();
	static void Shutdown();

	/// Registers the parsers of the longhands with the style sheet specification.
	static void RegisterParsers();

private:
	Kind kind;
	bool non_negative;
};

/// Parses a CSS <time> ('200ms', '0.2s', a calc() of times); 'seconds' receives the value. A unitless 0 is not a time.
bool ParseAnimationTime(const String& value, float& seconds);

/// The parameter maps of the keyword longhands (transition-behavior, animation-direction, ...), matching the keywords
/// their parsers are registered with.
const ParameterMap& GetAnimationKeywords(PropertyId id);
/// The keywords of a keyword longhand as a parser parameter string ("normal, allow-discrete").
const char* GetAnimationKeywordList(PropertyId id);

/// Builds the element's computed transition list from its transition longhands (CSS list-length rules: one entry per
/// transition-property item, the other lists repeated or truncated). Entries without effect (unknown properties, a
/// combined duration of zero) are left out.
TransitionList BuildTransitionList(const ElementStyle& style);

/// Builds the element's computed animation list from its animation longhands (one entry per animation-name item; 'none'
/// items are left out).
AnimationList BuildAnimationList(const ElementStyle& style);

/// The parsers of the 'transition' and 'animation' shorthands (ShorthandAnimation.cpp), registered as 'Custom'.
bool ParseTransitionShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
bool ParseAnimationShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

/// The computed transition and animation lists of an element, kept by its ElementStyle.
struct ElementAnimationStyle {
	TransitionList transitions;
	AnimationList animations;
};

/// The ids of the transition and of the animation longhands.
const PropertyIdSet& GetTransitionLonghandIds();
const PropertyIdSet& GetAnimationLonghandIds();

} // namespace Rml
