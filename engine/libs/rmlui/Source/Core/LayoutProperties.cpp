#include "LayoutProperties.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "ComputeProperty.h"
#include <algorithm>
#include <cmath>

namespace Rml {

namespace {

	// fit-content(<length-percentage>), a non-negative length or percentage.
	class PropertyParserFitContent final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			static const String name = "fit-content(";
			const String trimmed = StringUtilities::StripWhitespace(value);
			if (trimmed.size() <= name.size() || trimmed.back() != ')' || StringUtilities::ToLower(trimmed.substr(0, name.size())) != name)
				return false;

			LengthPercentageValue argument;
			if (!ParseLengthPercentage(StringUtilities::StripWhitespace(trimmed.substr(name.size(), trimmed.size() - name.size() - 1)), argument))
				return false;
#ifdef RMLUI_MATH_EXPRESSIONS
			// An expression is not stored by the computed value (a length or a percentage).
			if (argument.calculation)
				return false;
#endif
			if (argument.value.number < 0.f)
				return false;

			property.value = StyleValuePtr(MakeShared<FitContentValue>(std::move(argument)));
			property.unit = Unit::STYLE_VALUE;
			return true;
		}
	};


	Style::SizingKeyword ToSizingKeyword(const Property* property)
	{
		if (property->unit == Unit::STYLE_VALUE)
			return Style::SizingKeyword::FitContentLength;
		if (property->unit == Unit::KEYWORD)
			return static_cast<Style::SizingKeyword>(property->Get<int>());
		return Style::SizingKeyword::None;
	}

	Style::LengthPercentage ComputeFitContentArgument(const Property* property, float font_size, float document_font_size, float dp_ratio,
		Vector2f vp_dimensions)
	{
		const StyleValuePtr value = property->Get<StyleValuePtr>();
		if (!value)
			return {};
		const NumericValue argument = static_cast<const FitContentValue&>(*value).argument.value;
		if (argument.unit == Unit::PERCENT)
			return Style::LengthPercentage(Style::LengthPercentage::Percentage, argument.number);
		return Style::LengthPercentage(Style::LengthPercentage::Length, ComputeLength(argument, font_size, document_font_size, dp_ratio, vp_dimensions));
	}

} // namespace

String FitContentValue::ToString() const
{
	return "fit-content(" + argument.ToString() + ")";
}

bool FitContentValue::Equals(const StyleValue& other) const
{
	return argument == static_cast<const FitContentValue&>(other).argument;
}

SharedPtr<const StyleValue> FitContentValue::Interpolate(const StyleValue& to, float alpha, Element& /*element*/) const
{
	const FitContentValue& target = static_cast<const FitContentValue&>(to);
	if (argument.value.unit != target.argument.value.unit)
		return nullptr;
	const float number = argument.value.number + (target.argument.value.number - argument.value.number) * alpha;
	return MakeShared<FitContentValue>(LengthPercentageValue(number, argument.value.unit));
}

Units FitContentValue::GetUnitDependencies() const
{
	return argument.GetUnitDependencies();
}

void RegisterLayoutParsers()
{
	static PropertyParserFitContent fit_content;
	StyleSheetSpecification::RegisterParser("fit_content", &fit_content);
}

Style::LengthPercentageAuto ComputeSizingProperty(PropertyId id, const Property* property, Style::ComputedValues& values, float font_size,
	float document_font_size, float dp_ratio, Vector2f vp_dimensions)
{
	using namespace Style;
	const SizingKeyword keyword = ToSizingKeyword(property);
	switch (id)
	{
	case PropertyId::Width:
		values.width_keyword(keyword);
		if (keyword == SizingKeyword::FitContentLength)
			values.fit_content_width(ComputeFitContentArgument(property, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::Height:
		values.height_keyword(keyword);
		if (keyword == SizingKeyword::FitContentLength)
			values.fit_content_height(ComputeFitContentArgument(property, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::MinWidth: values.min_width_keyword(keyword); break;
	case PropertyId::MinHeight: values.min_height_keyword(keyword); break;
	case PropertyId::FlexBasis: values.flex_basis_keyword(keyword); break;
	default: RMLUI_ERROR; break;
	}

	if (keyword != SizingKeyword::None || property->unit == Unit::KEYWORD)
		return LengthPercentageAuto(LengthPercentageAuto::Auto);
	return ComputeLengthPercentageAuto(property, font_size, document_font_size, dp_ratio, vp_dimensions);
}

Style::LengthPercentage ComputeMaxSizingProperty(PropertyId id, const Property* property, Style::ComputedValues& values, float font_size,
	float document_font_size, float dp_ratio, Vector2f vp_dimensions)
{
	const Style::SizingKeyword keyword = ToSizingKeyword(property);
	if (id == PropertyId::MaxWidth)
		values.max_width_keyword(keyword);
	else
		values.max_height_keyword(keyword);
	return ComputeMaxSize(property, font_size, document_font_size, dp_ratio, vp_dimensions);
}

void ComputeLayoutProperty(PropertyId id, const Property* property, Style::ComputedValues& values, float font_size, float document_font_size,
	float dp_ratio, Vector2f vp_dimensions)
{
	switch (id)
	{
	case PropertyId::AspectRatio:
		// p3-values' 'aspect_ratio' parser: 'auto' a keyword, '<ratio>' Unit::RATIO (width, height), negative terms with 'auto'.
		if (property->unit == Unit::RATIO)
		{
			const Vector2f ratio = property->Get<Vector2f>();
			const bool is_auto = (ratio.x < 0.f || ratio.y < 0.f);
			const float width = Math::Absolute(ratio.x), height = Math::Absolute(ratio.y);
			// A degenerate ratio (a zero or infinite term) is no ratio.
			const float value = (width > 0.f && height > 0.f ? width / height : 0.f);
			values.aspect_ratio(std::isfinite(value) ? value : 0.f, is_auto);
		}
		else
			values.aspect_ratio(0.f, true);
		break;
	case PropertyId::OverflowClipMargin:
	{
		float margin = 0.f;
		if (ComputeDefiniteLength(property, font_size, document_font_size, dp_ratio, vp_dimensions, margin))
			values.overflow_clip_margin(Math::Max(margin, 0.f));
	}
	break;
	case PropertyId::ScrollbarGutter: values.scrollbar_gutter(static_cast<Style::ScrollbarGutter>(property->Get<int>())); break;
	default: break;
	}
}

void ApplyOverflowFixUp(Style::ComputedValues& values)
{
	using Style::Overflow;
	const Overflow x = values.overflow_x(), y = values.overflow_y();
	auto scrolling = [](Overflow overflow) { return overflow == Overflow::Hidden || overflow == Overflow::Auto || overflow == Overflow::Scroll; };
	auto fix = [](Overflow overflow) {
		if (overflow == Overflow::Visible)
			return Overflow::Auto;
		if (overflow == Overflow::Clip)
			return Overflow::Hidden;
		return overflow;
	};
	if (scrolling(x) != scrolling(y))
	{
		values.overflow_x(fix(x));
		values.overflow_y(fix(y));
	}
}

} // namespace Rml
