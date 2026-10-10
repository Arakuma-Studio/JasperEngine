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

	// 'auto || <ratio>', <ratio> = <number [0,inf]> [ / <number [0,inf]> ]?
	class PropertyParserAspectRatio final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			String spaced;
			spaced.reserve(value.size() + 4);
			for (char c : value)
			{
				if (c == '/')
					spaced += " / ";
				else
					spaced += c;
			}

			StringList tokens;
			StringUtilities::ExpandString(tokens, spaced, ' ');
			tokens.erase(std::remove(tokens.begin(), tokens.end(), String()), tokens.end());

			bool is_auto = false;
			if (!tokens.empty() && StringUtilities::ToLower(tokens.front()) == "auto")
			{
				is_auto = true;
				tokens.erase(tokens.begin());
			}
			else if (!tokens.empty() && StringUtilities::ToLower(tokens.back()) == "auto")
			{
				is_auto = true;
				tokens.pop_back();
			}

			float numerator = 0.f, denominator = 1.f;
			if (tokens.empty())
			{
				if (!is_auto)
					return false;
			}
			else if (tokens.size() == 1 || (tokens.size() == 3 && tokens[1] == "/"))
			{
				if (!ParseNonNegativeNumber(tokens[0], numerator))
					return false;
				if (tokens.size() == 3 && !ParseNonNegativeNumber(tokens[2], denominator))
					return false;
			}
			else
				return false;

			property.value = StyleValuePtr(MakeShared<AspectRatioValue>(is_auto, numerator, tokens.empty() ? 0.f : denominator));
			property.unit = Unit::STYLE_VALUE;
			return true;
		}

	private:
		static bool ParseNonNegativeNumber(const String& token, float& result)
		{
			if (token.empty())
				return false;
			for (char c : token)
			{
				if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
					return false;
			}
			if (!TypeConverter<String, float>::Convert(token, result))
				return false;
			return result >= 0.f && std::isfinite(result);
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

String AspectRatioValue::ToString() const
{
	String result;
	if (is_auto)
		result = "auto";
	if (denominator > 0.f || numerator > 0.f)
	{
		if (!result.empty())
			result += ' ';
		result += CreateString("%g / %g", numerator, denominator);
	}
	return result;
}

bool AspectRatioValue::Equals(const StyleValue& other) const
{
	const AspectRatioValue& value = static_cast<const AspectRatioValue&>(other);
	return is_auto == value.is_auto && numerator == value.numerator && denominator == value.denominator;
}

SharedPtr<const StyleValue> AspectRatioValue::Interpolate(const StyleValue& to, float alpha, Element& /*element*/) const
{
	const AspectRatioValue& target = static_cast<const AspectRatioValue&>(to);
	const float from_ratio = GetRatio(), to_ratio = target.GetRatio();
	if (is_auto || target.is_auto || from_ratio <= 0.f || to_ratio <= 0.f)
		return nullptr;
	const float ratio = std::exp(std::log(from_ratio) + (std::log(to_ratio) - std::log(from_ratio)) * alpha);
	return MakeShared<AspectRatioValue>(false, ratio, 1.f);
}

float AspectRatioValue::GetRatio() const
{
	if (numerator <= 0.f || denominator <= 0.f || !std::isfinite(numerator) || !std::isfinite(denominator))
		return 0.f;
	const float ratio = numerator / denominator;
	return std::isfinite(ratio) ? ratio : 0.f;
}

void RegisterLayoutParsers()
{
	static PropertyParserFitContent fit_content;
	static PropertyParserAspectRatio aspect_ratio;
	StyleSheetSpecification::RegisterParser("fit_content", &fit_content);
	StyleSheetSpecification::RegisterParser("aspect_ratio", &aspect_ratio);
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
		if (property->unit == Unit::STYLE_VALUE)
		{
			if (const StyleValuePtr value = property->Get<StyleValuePtr>())
			{
				const AspectRatioValue& ratio = static_cast<const AspectRatioValue&>(*value);
				values.aspect_ratio(ratio.GetRatio(), ratio.is_auto);
			}
		}
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
