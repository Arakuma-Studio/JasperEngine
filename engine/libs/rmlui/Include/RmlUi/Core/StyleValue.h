#pragma once

#include "Header.h"
#include "NumericValue.h"
#include "Types.h"
#include "Unit.h"

namespace Rml {

class Element;

/**
    The value of a property whose type has no Variant of its own (phase 3: background layers, shadows, grid track lists,
    content and counter lists, easing lists, ...). A property holding one has the unit Unit::STYLE_VALUE and its value is
    a StyleValuePtr, immutable once parsed. Each type subclasses StyleValue in its own source file and is read back with
    a typed accessor (Element::GetProperty or ComputedValues::GetLocalProperty, then a static_cast).
 */
class RMLUICORE_API StyleValue {
public:
	virtual ~StyleValue();

	/// The value serialized as CSS.
	virtual String ToString() const = 0;
	/// True if the other value is the same. Only called with values of the same property, which holds values of one
	/// StyleValue type.
	virtual bool Equals(const StyleValue& other) const = 0;
	/// Interpolates from this value to 'to' (of the same dynamic type) at 'alpha' in [0, 1], for transitions and
	/// animations. Returns nullptr when the pair cannot be interpolated: the animation then switches discretely at 0.5.
	virtual SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha, Element& element) const;
	/// The context-relative units the value depends on (em, rem, vw, ...): a change of their base dirties the property.
	virtual Units GetUnitDependencies() const;
};

/// A length or percentage stored by a StyleValue: a number with its unit, or a calc()/min()/max()/clamp() expression.
struct RMLUICORE_API LengthPercentageValue {
	NumericValue value;
#ifdef RMLUI_MATH_EXPRESSIONS
	CalculationPtr calculation; // Set: the value is this expression, 'value' is unused.
#endif

	LengthPercentageValue() = default;
	LengthPercentageValue(float number, Unit unit) : value(number, unit) {}
	explicit LengthPercentageValue(NumericValue value) : value(value) {}

	bool operator==(const LengthPercentageValue& other) const;
	bool operator!=(const LengthPercentageValue& other) const { return !(*this == other); }
	String ToString() const;
	/// The context-relative units it depends on.
	Units GetUnitDependencies() const;
};

/// Parses one length or percentage token ('10px', '50%', '2em', 'calc(50% - 4px)', or a unitless 0).
/// @return True on success.
RMLUICORE_API bool ParseLengthPercentage(const String& token, LengthPercentageValue& result);

/// Resolves a length or percentage to pixels for the element: percentages and the percentages of an expression are
/// taken of 'percent_basis', relative lengths (em, rem, vw, dp, ...) from the element's computed values.
RMLUICORE_API float ResolveLengthPercentage(const LengthPercentageValue& value, Element& element, float percent_basis);

/// Parses a CSS <position> (CSS Values 4, background-position's form): one to four tokens, keywords in either order
/// ('top left'), edge offsets ('right 10px bottom 20%'). The horizontal and vertical positions are given as a length or
/// percentage of the positioning area; 'center' is 50%, an offset from the right or bottom edge is 100% minus it (a
/// calc() expression for a length).
/// @return True if the tokens form a valid position.
RMLUICORE_API bool ParsePosition(const StringList& tokens, LengthPercentageValue& x, LengthPercentageValue& y);

} // namespace Rml
