#include "ComputeProperty.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/FontMetrics.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/TextShapingContext.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "ControlledLifetimeResource.h"
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "CalculationResolver.h"
#endif

namespace Rml {

struct ComputedPropertyData {
	const Style::ComputedValues computed{nullptr};
};
static ControlledLifetimeResource<ComputedPropertyData> computed_property_data;

const Style::ComputedValues& DefaultComputedValues()
{
	return computed_property_data->computed;
}

void InitializeComputeProperty()
{
	computed_property_data.Initialize();
}

void ShutdownComputeProperty()
{
	computed_property_data.Shutdown();
}

static constexpr float PixelsPerInch = 96.0f;

static float ComputePPILength(NumericValue value, float dp_ratio)
{
	RMLUI_ASSERT(Any(value.unit & Unit::PPI_UNIT));

	// Values based on pixels-per-inch. Scaled by the dp-ratio as a placeholder solution until we make the pixel unit itself scalable.
	const float inch = value.number * PixelsPerInch * dp_ratio;

	switch (value.unit)
	{
	case Unit::INCH: return inch;
	case Unit::CM: return inch * (1.0f / 2.54f);
	case Unit::MM: return inch * (1.0f / 25.4f);
	case Unit::PT: return inch * (1.0f / 72.0f);
	case Unit::PC: return inch * (1.0f / 6.0f);
	default: break;
	}

	RMLUI_ERROR;
	return 0.f;
}

static FontFaceHandle current_font_unit_face = 0;

FontUnitScope::FontUnitScope(FontFaceHandle font_face) : previous(current_font_unit_face)
{
	current_font_unit_face = font_face;
}

FontUnitScope::~FontUnitScope()
{
	current_font_unit_face = previous;
}

void FontUnitScope::SetFontFace(FontFaceHandle font_face)
{
	current_font_unit_face = font_face;
}

FontFaceHandle FontUnitScope::GetFontFace()
{
	return current_font_unit_face;
}

float ComputeChSize(FontFaceHandle font_face, float font_size)
{
	if (font_face)
	{
		if (FontEngineInterface* font_engine = GetFontEngineInterface())
		{
			static const String no_language;
			const int advance = font_engine->GetStringWidth(font_face, "0", TextShapingContext{no_language});
			if (advance > 0)
				return float(advance);
		}
	}
	return 0.5f * font_size;
}

float ComputeExSize(FontFaceHandle font_face, float font_size)
{
	if (font_face)
	{
		if (FontEngineInterface* font_engine = GetFontEngineInterface())
		{
			const float x_height = font_engine->GetFontMetrics(font_face).x_height;
			if (x_height > 0.f)
				return x_height;
		}
	}
	return 0.5f * font_size;
}

float ComputeLength(NumericValue value, const LengthContext& context)
{
	if (Any(value.unit & Unit::PPI_UNIT))
		return ComputePPILength(value, context.dp_ratio);

	switch (value.unit)
	{
	case Unit::PX: return value.number;
	case Unit::EM: return value.number * context.font_size;
	case Unit::REM: return value.number * context.root_font_size;
	case Unit::DP: return value.number * context.dp_ratio;
	case Unit::VW: return value.number * context.vp_dimensions.x * 0.01f;
	case Unit::VH: return value.number * context.vp_dimensions.y * 0.01f;
	case Unit::VMIN: return value.number * Math::Min(context.vp_dimensions.x, context.vp_dimensions.y) * 0.01f;
	case Unit::VMAX: return value.number * Math::Max(context.vp_dimensions.x, context.vp_dimensions.y) * 0.01f;
	case Unit::CH: return value.number * ComputeChSize(context.font_face, context.font_size);
	case Unit::EX: return value.number * ComputeExSize(context.font_face, context.font_size);
	default: break;
	}

	RMLUI_ERROR;
	return 0.0f;
}

float ComputeLength(NumericValue value, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions)
{
	return ComputeLength(value, LengthContext{font_size, document_font_size, dp_ratio, vp_dimensions, current_font_unit_face});
}

float ComputeAngle(NumericValue value)
{
	switch (value.unit)
	{
	case Unit::NUMBER:
	case Unit::RAD: return value.number;

	case Unit::DEG: return Math::DegreesToRadians(value.number);
	case Unit::TURN: return value.number * 2.f * Math::RMLUI_PI;
	case Unit::GRAD: return value.number * (Math::RMLUI_PI / 200.f);
	default: break;
	}

	RMLUI_ERROR;
	return 0.0f;
}

float ComputeFontsize(const Property* property, const Style::ComputedValues& values, const Style::ComputedValues* parent_values,
	const Style::ComputedValues* document_values, float dp_ratio, Vector2f vp_dimensions)
{
	// An element without a parent resolves its relative font sizes against the initial font size, as CSS does for the
	// root element ('rem' on the document element already did).
	const float parent_font_size = parent_values ? parent_values->font_size() : DefaultComputedValues().font_size();

#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		const float root_font_size =
			(!document_values || &values == document_values) ? DefaultComputedValues().font_size() : document_values->font_size();
		CalculationResolverContext context{values.font_size(), parent_font_size, root_font_size, values.line_height().value, vp_dimensions, dp_ratio,
			property->definition ? property->definition->GetRelativeTarget() : RelativeTarget::ParentFontSize, current_font_unit_face};
		ResolvedCalculation resolved;
		const CalculationPtr calculation = property->value.Get<CalculationPtr>();
		if (calculation && ResolveCalculation(*calculation, context, resolved) && resolved.is_constant && resolved.unit == Unit::PX)
			return resolved.value;
		return values.font_size();
	}
#endif

	// The keywords (phase 3, values), in the order they are registered: the absolute sizes of CSS Fonts 4 relative to
	// the initial 16px (Chromium's table), 'smaller'/'larger' by a factor of 1.2 of the parent's size, 'math' the parent's.
	if (property->unit == Unit::KEYWORD)
	{
		static constexpr float absolute_sizes[] = {9.f, 10.f, 13.f, 16.f, 18.f, 24.f, 32.f, 48.f};
		const int keyword = property->Get<int>();
		if (keyword >= 0 && keyword < 8)
			return absolute_sizes[keyword] * (DefaultComputedValues().font_size() / 16.f);
		if (keyword == 8)
			return parent_font_size / 1.2f;
		if (keyword == 9)
			return parent_font_size * 1.2f;
		return parent_font_size;
	}

	NumericValue value = property->GetNumericValue();
	if (Any(value.unit & (Unit::PERCENT | Unit::EM | Unit::REM)))
	{
		// Relative values are based on the parent's or document's font size instead of our own.
		float multiplier = 1.0f;

		switch (value.unit)
		{
		case Unit::PERCENT:
			multiplier = 0.01f;
			//-fallthrough
		case Unit::EM: return value.number * multiplier * parent_font_size;

		case Unit::REM:
			// If the current element is a document, the rem unit is relative to the default size.
			if (!document_values || &values == document_values)
				return value.number * DefaultComputedValues().font_size();

			// Otherwise it is relative to the document font size.
			return value.number * document_values->font_size();
		default: break;
		}
	}

	// 'ch' and 'ex' are the parent's: the inherited font face is current while the font size is computed.
	if (value.unit == Unit::CH)
		return value.number * ComputeChSize(current_font_unit_face, parent_font_size);
	if (value.unit == Unit::EX)
		return value.number * ComputeExSize(current_font_unit_face, parent_font_size);

	// Font-relative lengths handled above, other lengths should be handled as normal.
	return ComputeLength(value, 0.f, 0.f, dp_ratio, vp_dimensions);
}

bool ComputeDefiniteLength(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions, float& result)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		CalculationResolverContext context{font_size, font_size, document_font_size, 0.f, vp_dimensions, dp_ratio,
			property->definition ? property->definition->GetRelativeTarget() : RelativeTarget::None, current_font_unit_face};
		ResolvedCalculation resolved;
		const CalculationPtr calculation = property->value.Get<CalculationPtr>();
		if (!calculation || !ResolveCalculation(*calculation, context, resolved) || !resolved.is_constant || resolved.unit != Unit::PX)
			return false;
		result = resolved.value;
		return true;
	}
#endif
	result = ComputeLength(property->GetNumericValue(), font_size, document_font_size, dp_ratio, vp_dimensions);
	return true;
}

bool ComputeDefiniteNumber(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions, float& result)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		CalculationResolverContext context{font_size, font_size, document_font_size, 0.f, vp_dimensions, dp_ratio,
			property->definition ? property->definition->GetRelativeTarget() : RelativeTarget::None, current_font_unit_face};
		ResolvedCalculation resolved;
		const CalculationPtr calculation = property->value.Get<CalculationPtr>();
		if (!calculation || !ResolveCalculation(*calculation, context, resolved) || !resolved.is_constant || resolved.unit != Unit::NUMBER)
			return false;
		result = resolved.value;
		return true;
	}
#else
	(void)font_size;
	(void)document_font_size;
	(void)dp_ratio;
	(void)vp_dimensions;
#endif
	result = property->Get<float>();
	return true;
}

Style::FontWeight ComputeRelativeFontWeight(int keyword, Style::FontWeight parent_weight)
{
	const int parent = (parent_weight == Style::FontWeight::Auto ? 400 : int(parent_weight));
	int weight = parent;
	if (keyword == FontWeightBolder)
		weight = (parent < 350 ? 400 : parent < 550 ? 700 : parent < 900 ? 900 : parent);
	else if (keyword == FontWeightLighter)
		weight = (parent < 100 ? parent : parent < 550 ? 100 : parent < 750 ? 400 : 700);
	return Style::FontWeight(weight);
}

String ComputeFontFamily(String font_family)
{
	return StringUtilities::ToLower(std::move(font_family));
}

Style::Clip ComputeClip(const Property* property)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		float value = 0.f;
		if (ComputeDefiniteNumber(property, 0.f, 0.f, 1.f, Vector2f(1.f), value))
			return Style::Clip(Style::Clip::Type::Number, static_cast<int8_t>(value));
		return Style::Clip();
	}
#endif
	const int value = property->Get<int>();
	if (property->unit == Unit::KEYWORD)
		return Style::Clip(static_cast<Style::Clip::Type>(value));
	else if (property->unit == Unit::NUMBER)
		return Style::Clip(Style::Clip::Type::Number, static_cast<int8_t>(value));
	RMLUI_ERRORMSG("Invalid clip type");
	return Style::Clip();
}

Style::LineHeight ComputeLineHeight(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		CalculationResolverContext context{font_size, font_size, document_font_size, 0.f, vp_dimensions, dp_ratio,
			property->definition ? property->definition->GetRelativeTarget() : RelativeTarget::FontSize, current_font_unit_face};
		ResolvedCalculation resolved;
		const CalculationPtr calculation = property->value.Get<CalculationPtr>();
		if (calculation && ResolveCalculation(*calculation, context, resolved) && resolved.is_constant)
		{
			if (resolved.unit == Unit::NUMBER)
				return Style::LineHeight(font_size * resolved.value, Style::LineHeight::Number, resolved.value);
			if (resolved.unit == Unit::PX)
				return Style::LineHeight(resolved.value, Style::LineHeight::Length, resolved.value);
		}
		return Style::LineHeight();
	}
#endif
	if (Any(property->unit & Unit::LENGTH))
	{
		float value = ComputeLength(property->GetNumericValue(), font_size, document_font_size, dp_ratio, vp_dimensions);
		return Style::LineHeight(value, Style::LineHeight::Length, value);
	}

	float scale_factor = 1.0f;

	switch (property->unit)
	{
	case Unit::NUMBER: scale_factor = property->value.Get<float>(); break;
	case Unit::PERCENT: scale_factor = property->value.Get<float>() * 0.01f; break;
	default: RMLUI_ERRORMSG("Invalid unit for line-height");
	}

	float value = font_size * scale_factor;
	return Style::LineHeight(value, Style::LineHeight::Number, scale_factor);
}

Style::VerticalAlign ComputeVerticalAlign(const Property* property, float line_height, float font_size, float document_font_size, float dp_ratio,
	Vector2f vp_dimensions)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		CalculationResolverContext context{font_size, font_size, document_font_size, line_height, vp_dimensions, dp_ratio,
			property->definition ? property->definition->GetRelativeTarget() : RelativeTarget::LineHeight, current_font_unit_face};
		ResolvedCalculation resolved;
		const CalculationPtr calculation = property->value.Get<CalculationPtr>();
		if (calculation && ResolveCalculation(*calculation, context, resolved) && resolved.is_constant && resolved.unit == Unit::PX)
			return Style::VerticalAlign(resolved.value);
		return Style::VerticalAlign();
	}
#endif
	if (Any(property->unit & Unit::LENGTH))
	{
		float value = ComputeLength(property->GetNumericValue(), font_size, document_font_size, dp_ratio, vp_dimensions);
		return Style::VerticalAlign(value);
	}
	else if (property->unit == Unit::PERCENT)
	{
		return Style::VerticalAlign(property->Get<float>() * line_height * 0.01f);
	}

	RMLUI_ASSERT(property->unit == Unit::KEYWORD);
	return Style::VerticalAlign((Style::VerticalAlign::Type)property->Get<int>());
}

Style::LengthPercentage ComputeLengthPercentage(const Property* property, float font_size, float document_font_size, float dp_ratio,
	Vector2f vp_dimensions)
{
	using namespace Style;
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		CalculationResolverContext context{font_size, font_size, document_font_size, 0.f, vp_dimensions, dp_ratio,
			property->definition ? property->definition->GetRelativeTarget() : RelativeTarget::None, current_font_unit_face};
		ResolvedCalculation resolved;
		const CalculationPtr calculation = property->value.Get<CalculationPtr>();
		if (calculation && ResolveCalculation(*calculation, context, resolved))
		{
			if (resolved.is_constant && resolved.unit == Unit::PX)
				return LengthPercentage(LengthPercentage::Length, resolved.value);
			if (resolved.is_constant && resolved.unit == Unit::PERCENT)
				return LengthPercentage(LengthPercentage::Percentage, resolved.value);
			if (resolved.residual)
				return LengthPercentage(std::move(resolved.residual));
		}
		return LengthPercentage();
	}
#endif
	if (property->unit == Unit::PERCENT)
		return LengthPercentage(LengthPercentage::Percentage, property->Get<float>());

	return LengthPercentage(LengthPercentage::Length,
		ComputeLength(property->GetNumericValue(), font_size, document_font_size, dp_ratio, vp_dimensions));
}

Style::LengthPercentageAuto ComputeLengthPercentageAuto(const Property* property, float font_size, float document_font_size, float dp_ratio,
	Vector2f vp_dimensions)
{
	using namespace Style;
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		const LengthPercentage value = ComputeLengthPercentage(property, font_size, document_font_size, dp_ratio, vp_dimensions);
		if (value.type == LengthPercentage::Calculation)
			return LengthPercentageAuto(value.calculation);
		if (value.type == LengthPercentage::Percentage)
			return LengthPercentageAuto(LengthPercentageAuto::Percentage, value.value);
		return LengthPercentageAuto(LengthPercentageAuto::Length, value.value);
	}
#endif
	if (property->unit == Unit::PERCENT)
		return LengthPercentageAuto(LengthPercentageAuto::Percentage, property->Get<float>());
	else if (property->unit == Unit::KEYWORD)
		return LengthPercentageAuto(LengthPercentageAuto::Auto);

	return LengthPercentageAuto(LengthPercentageAuto::Length,
		ComputeLength(property->GetNumericValue(), font_size, document_font_size, dp_ratio, vp_dimensions));
}

Style::LengthPercentage ComputeOrigin(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions)
{
	using namespace Style;
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
		return ComputeLengthPercentage(property, font_size, document_font_size, dp_ratio, vp_dimensions);
#endif
	static_assert(
		(int)OriginX::Left == (int)OriginY::Top && (int)OriginX::Center == (int)OriginY::Center && (int)OriginX::Right == (int)OriginY::Bottom, "");

	if (property->unit == Unit::KEYWORD)
	{
		float percent = 0.0f;
		OriginX origin = (OriginX)property->Get<int>();
		switch (origin)
		{
		case OriginX::Left: percent = 0.0f; break;
		case OriginX::Center: percent = 50.0f; break;
		case OriginX::Right: percent = 100.f; break;
		}
		return LengthPercentage(LengthPercentage::Percentage, percent);
	}
	else if (property->unit == Unit::PERCENT)
		return LengthPercentage(LengthPercentage::Percentage, property->Get<float>());

	return LengthPercentage(LengthPercentage::Length,
		ComputeLength(property->GetNumericValue(), font_size, document_font_size, dp_ratio, vp_dimensions));
}

Style::LengthPercentage ComputeMaxSize(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions)
{
	using namespace Style;
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property->unit == Unit::CALCULATION)
	{
		LengthPercentage value = ComputeLengthPercentage(property, font_size, document_font_size, dp_ratio, vp_dimensions);
		if (value.type == LengthPercentage::Length && value.value < 0.f)
			value.value = FLT_MAX;
		return value;
	}
#endif
	if (Any(property->unit & Unit::KEYWORD))
		return LengthPercentage(LengthPercentage::Length, FLT_MAX);
	else if (Any(property->unit & Unit::PERCENT))
		return LengthPercentage(LengthPercentage::Percentage, property->Get<float>());

	const float length = ComputeLength(property->GetNumericValue(), font_size, document_font_size, dp_ratio, vp_dimensions);
	return LengthPercentage(LengthPercentage::Length, length < 0.f ? FLT_MAX : length);
}

uint16_t ComputeBorderWidth(float computed_length)
{
	if (computed_length <= 0.f)
		return 0;

	if (computed_length <= 1.f)
		return 1;

	return uint16_t(computed_length + 0.5f);
}

String GetFontFaceDescription(const String& font_family, Style::FontStyle style, Style::FontWeight weight)
{
	String font_attributes;

	if (style == Style::FontStyle::Italic)
		font_attributes += "italic, ";
	if (weight == Style::FontWeight::Bold)
		font_attributes += "bold, ";
	else if (weight != Style::FontWeight::Auto && weight != Style::FontWeight::Normal)
		font_attributes += "weight=" + ToString((int)weight) + ", ";

	if (font_attributes.empty())
		font_attributes = "regular";
	else
		font_attributes.resize(font_attributes.size() - 2);

	return CreateString("'%s' [%s]", font_family.c_str(), font_attributes.c_str());
}

} // namespace Rml
