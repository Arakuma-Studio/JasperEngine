#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/Decorator.h"
#include "../../Include/RmlUi/Core/StyleSheetTypes.h"
#include "ComputeProperty.h"
#include "ElementStyle.h"
#include "PropertiesIterator.h"
#include "PropertyParserColour.h"
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "Calculation.h"
#endif

namespace Rml {

// The hooks of ElementStyle::ComputeValues for CSS values (phase 3, docs/html-ui/phase-3-fork-patches.md 3.3).

static bool UsesFontFaceUnits(const Property& property)
{
	const Units font_face_units = Unit::CH | Unit::EX;
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property.unit == Unit::CALCULATION)
	{
		const CalculationPtr calculation = property.value.Get<CalculationPtr>();
		return calculation && Any(calculation->GetDependencyMask() & font_face_units);
	}
#endif
	return Any(property.unit & font_face_units);
}

// Whether a value with colours inside (a shadow list, decorators with gradients) has a 'currentcolor', which the element's
// color replaces when it is drawn.
static bool HoldsCurrentColor(const Property& property)
{
	switch (property.unit)
	{
	case Unit::BOXSHADOWLIST:
		if (property.value.GetType() == Variant::BOXSHADOWLIST)
		{
			for (const BoxShadow& shadow : property.value.GetReference<BoxShadowList>())
				if (shadow.current_color)
					return true;
		}
		return false;
	case Unit::COLORSTOPLIST:
		if (property.value.GetType() == Variant::COLORSTOPLIST)
		{
			for (const ColorStop& stop : property.value.GetReference<ColorStopList>())
				if (stop.current_color)
					return true;
		}
		return false;
	case Unit::DECORATOR:
		if (property.value.GetType() == Variant::DECORATORSPTR)
		{
			if (const DecoratorsPtr& decorators = property.value.GetReference<DecoratorsPtr>())
			{
				for (const DecoratorDeclaration& declaration : decorators->list)
				{
					for (const auto& id_property : declaration.properties.GetProperties())
						if (PropertyParserColour::IsCurrentColor(id_property.second) || HoldsCurrentColor(id_property.second))
							return true;
				}
			}
		}
		return false;
	default: break;
	}
	return false;
}

bool ElementStyle::ComputeColorFirst(Style::ComputedValues& values, const Style::ComputedValues* parent_values, Property& property_storage)
{
	if (const Property* property = GetLocalProperty(PropertyId::Color))
	{
		SmallUnorderedSet<String> variable_dependencies;
		property = ResolveVariables(PropertyId::Color, property, variable_dependencies, property_storage);
		if (property)
		{
			if (PropertyParserColour::IsCurrentColor(*property))
				values.color(parent_values ? parent_values->color() : DefaultComputedValues().color());
			else if (property->unit == Unit::COLOUR)
				values.color(property->Get<Colourb>());
		}
	}
	return GetDirtyPropertiesRef().properties.Contains(PropertyId::Color);
}

const Property* ElementStyle::ResolveValueKeywords(PropertyId id, const Property* property, const Style::ComputedValues& values,
	const Style::ComputedValues* parent_values, bool color_dirty, Property& current_color_storage)
{
	if (property->unit == Unit::KEYWORD)
	{
		if (id == PropertyId::FontWeight)
		{
			const int keyword = property->Get<int>();
			if (keyword != FontWeightBolder && keyword != FontWeightLighter)
				return property;
			const Style::FontWeight parent_weight = (parent_values ? parent_values->font_weight() : DefaultComputedValues().font_weight());
			current_color_storage = Property(int(ComputeRelativeFontWeight(keyword, parent_weight)), Unit::KEYWORD);
			current_color_storage.definition = property->definition;
			current_color_storage.source = property->source;
			return &current_color_storage;
		}
		if (!PropertyParserColour::IsCurrentColor(*property))
			return property;
		// In 'color' it is the parent's color, as 'inherit'.
		const bool own_color = (id == PropertyId::Color);
		const Colourb color = own_color ? (parent_values ? parent_values->color() : DefaultComputedValues().color()) : values.color();
		if (color_dirty && !own_color)
			DirtyProperty(id);
		current_color_storage = Property(color, Unit::COLOUR);
		current_color_storage.definition = property->definition;
		current_color_storage.source = property->source;
		return &current_color_storage;
	}
	if (color_dirty && (property->unit == Unit::BOXSHADOWLIST || property->unit == Unit::DECORATOR) && HoldsCurrentColor(*property))
		DirtyProperty(id);
	return property;
}

void ElementStyle::ComputeFontRelativeValues(Style::ComputedValues& values, FontUnitScope& font_unit_scope, FontFaceHandle previous_font_face,
	FontFaceHandle loop_font_face, float dp_ratio, Vector2f vp_dimensions, float font_size, float document_font_size, Property& property_storage)
{
	const FontFaceHandle font_face = values.font_face_handle();
	font_unit_scope.SetFontFace(font_face);

	// 'ch' and 'ex' lengths were computed against the inherited face: compute them again against the element's own,
	// and report them changed when the face differs from the one of the last computation.
	if (font_face != loop_font_face || font_face != previous_font_face)
	{
		// The units a specified value that may hold 'ch' or 'ex' has.
		const Units candidate_units = Unit::CH | Unit::EX | Unit::VAR_EXPRESSION | Unit::SHORTHAND_PLACEHOLDER
#ifdef RMLUI_MATH_EXPRESSIONS
			| Unit::CALCULATION
#endif
			;
		SmallUnorderedSet<String> variable_dependencies;
		bool dirty_font_face_handle = false;
		for (auto it = Iterate(); !it.AtEnd(); ++it)
		{
			auto id_property_pair = *it;
			const PropertyId id = id_property_pair.first;
			if (id == PropertyId::FontSize || !Any(id_property_pair.second.unit & candidate_units))
				continue;
			variable_dependencies.clear();
			const Property* property = ResolveVariables(id, &id_property_pair.second, variable_dependencies, property_storage);
			if (!property || !UsesFontFaceUnits(*property))
				continue;
			if (id == PropertyId::LineHeight)
				values.line_height(ComputeLineHeight(property, font_size, document_font_size, dp_ratio, vp_dimensions));
			else if (font_face != loop_font_face)
				ComputeValue(values, dp_ratio, vp_dimensions, font_size, document_font_size, dirty_font_face_handle, id, property);
			DirtyProperty(id);
		}
	}
}

} // namespace Rml
