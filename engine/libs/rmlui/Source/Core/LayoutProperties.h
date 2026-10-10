#pragma once

#include "../../Include/RmlUi/Core/ID.h"
#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/StyleTypes.h"
#include "../../Include/RmlUi/Core/StyleValue.h"

namespace Rml {

class Property;

/*
    The layout properties of phase 3 (p3-layout): the sizing keywords of width, height, min-*, max-* and flex-basis,
    'aspect-ratio', 'overflow: clip' and its value fix-up, 'overflow-clip-margin', 'scrollbar-gutter' and
    'flex-line-count'. The parsers are registered by RegisterLayoutParsers, the values computed by the functions below.
 */

/// The value of 'fit-content(<length-percentage>)' (a width or height).
class FitContentValue final : public StyleValue {
public:
	explicit FitContentValue(LengthPercentageValue argument) : argument(std::move(argument)) {}

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha, Element& element) const override;
	Units GetUnitDependencies() const override;

	const LengthPercentageValue argument;
};

/// The value of 'aspect-ratio': 'auto', '<ratio>' or 'auto && <ratio>'. A degenerate ratio (a zero or infinite term)
/// is stored as no ratio.
class AspectRatioValue final : public StyleValue {
public:
	AspectRatioValue(bool is_auto, float numerator, float denominator) : is_auto(is_auto), numerator(numerator), denominator(denominator) {}

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	/// Ratios interpolate in log space (CSS Values 4 §5.5.1); 'auto' does not interpolate.
	SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha, Element& element) const override;

	/// The ratio as width / height, 0 if none or degenerate.
	float GetRatio() const;

	const bool is_auto;
	const float numerator, denominator;
};

/// Registers the parsers of the layout properties ('fit_content', 'aspect_ratio').
void RegisterLayoutParsers();

/// Computes a width, height, min-width, min-height or flex-basis: a sizing keyword computes to 'auto', and the keyword
/// (and the argument of fit-content()) is stored apart in the computed values.
Style::LengthPercentageAuto ComputeSizingProperty(PropertyId id, const Property* property, Style::ComputedValues& values, float font_size,
	float document_font_size, float dp_ratio, Vector2f vp_dimensions);
/// Computes a max-width or max-height: a sizing keyword computes to 'none', the keyword is stored apart.
Style::LengthPercentage ComputeMaxSizingProperty(PropertyId id, const Property* property, Style::ComputedValues& values, float font_size,
	float document_font_size, float dp_ratio, Vector2f vp_dimensions);

/// Computes the layout properties of p3-layout that need no other value: aspect-ratio, overflow-clip-margin,
/// scrollbar-gutter. (flex-line-count is read from the local property.)
void ComputeLayoutProperty(PropertyId id, const Property* property, Style::ComputedValues& values, float font_size, float document_font_size,
	float dp_ratio, Vector2f vp_dimensions);

/// The CSS overflow value fix-up (CSS Overflow 3 §3): when one axis is 'visible' or 'clip' and the other is a scrolling
/// value ('hidden', 'auto', 'scroll'), 'visible' computes to 'auto' and 'clip' to 'hidden'. Applied after every value
/// of an element is computed.
void ApplyOverflowFixUp(Style::ComputedValues& values);

} // namespace Rml
