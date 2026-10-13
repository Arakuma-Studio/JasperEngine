#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
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
	// A complex value (grid track lists, ...) names the units its lengths depend on.
	if (property.unit == Unit::STYLE_VALUE)
	{
		const StyleValuePtr value = property.value.Get<StyleValuePtr>();
		return value && Any(value->GetUnitDependencies() & font_face_units);
	}
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
						if (PropertyParserColour::DependsOnCurrentColor(id_property.second) || HoldsCurrentColor(id_property.second))
							return true;
				}
			}
		}
		return false;
	default: break;
	}
	return false;
}

void ElementStyle::ComputeColor(Style::ComputedValues& values, const Style::ComputedValues* parent_values)
{
	// Without its own 'color' the element has its parent's, copied with the inherited values.
	if (const Property* property = GetLocalProperty(PropertyId::Color))
	{
		SmallUnorderedSet<String> variable_dependencies;
		Property property_storage;
		property = ResolveVariables(PropertyId::Color, property, variable_dependencies, property_storage);
		if (property)
		{
			if (PropertyParserColour::DependsOnCurrentColor(*property))
				values.color(PropertyParserColour::Resolve(*property, parent_values ? parent_values->color() : DefaultComputedValues().color()));
			else if (property->unit == Unit::COLOUR)
				values.color(property->Get<Colourb>());
		}
	}
}

const Property* ElementStyle::ResolveValueKeywords(PropertyId id, const Property* property, Style::ComputedValues& values,
	const Style::ComputedValues* parent_values, bool& color_computed, Property& current_color_storage)
{
	if (property->unit == Unit::KEYWORD || PropertyParserColour::IsCurrentColorMix(*property))
	{
		if (id == PropertyId::FontWeight && property->unit == Unit::KEYWORD)
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
		if (!PropertyParserColour::DependsOnCurrentColor(*property))
			return property;
		// In 'color' it is the parent's color, as 'inherit'.
		const bool own_color = (id == PropertyId::Color);
		if (!own_color && !color_computed)
		{
			ComputeColor(values, parent_values);
			color_computed = true;
		}
		const Colourb color = own_color ? (parent_values ? parent_values->color() : DefaultComputedValues().color()) : values.color();
		if (!own_color && GetDirtyPropertiesRef().properties.Contains(PropertyId::Color))
			DirtyProperty(id);
		current_color_storage = Property(PropertyParserColour::Resolve(*property, color), Unit::COLOUR);
		current_color_storage.definition = property->definition;
		current_color_storage.source = property->source;
		return &current_color_storage;
	}
	if ((property->unit == Unit::BOXSHADOWLIST || property->unit == Unit::DECORATOR) && GetDirtyPropertiesRef().properties.Contains(PropertyId::Color) &&
		HoldsCurrentColor(*property))
		DirtyProperty(id);
	return property;
}

void ElementStyle::ComputeFontRelativeValues(Style::ComputedValues& values, FontUnitScope& font_unit_scope, FontFaceHandle previous_font_face,
	FontFaceHandle loop_font_face, float dp_ratio, Vector2f vp_dimensions, float font_size, float document_font_size, Property& property_storage,
	bool any_font_face_candidates)
{
	const FontFaceHandle font_face = values.font_face_handle();
	font_unit_scope.SetFontFace(font_face);

	// 'line-height: normal' is the line spacing of the face.
	const Style::LineHeight line_height = values.line_height();
	if (line_height.inherit_type == Style::LineHeight::Normal &&
		(font_face != loop_font_face || font_face != previous_font_face || GetDirtyPropertiesRef().properties.Contains(PropertyId::LineHeight)))
	{
		const float value = ComputeNormalLineHeight(font_face, font_size);
		if (value != line_height.value)
		{
			values.line_height(Style::LineHeight(value, Style::LineHeight::Normal, line_height.inherit_value));
			DirtyProperty(PropertyId::LineHeight);
			// Vertical-align lengths of the line height: computed again.
			if (const Property* vertical_align = GetLocalProperty(PropertyId::VerticalAlign))
			{
				SmallUnorderedSet<String> variable_dependencies;
				bool dirty_font_face_handle = false;
				if ((vertical_align = ResolveVariables(PropertyId::VerticalAlign, vertical_align, variable_dependencies, property_storage)))
					ComputeValue(values, dp_ratio, vp_dimensions, font_size, document_font_size, dirty_font_face_handle, PropertyId::VerticalAlign,
						vertical_align);
				DirtyProperty(PropertyId::VerticalAlign);
			}
		}
	}

	// 'ch' and 'ex' lengths were computed against the inherited face: compute them again against the element's own,
	// and report them changed when the face differs from the one of the last computation.
	if ((font_face != loop_font_face || font_face != previous_font_face) && (any_font_face_candidates || animation_layers))
	{
		// The units a specified value that may hold 'ch' or 'ex' has.
		const Units candidate_units = FontFaceCandidateUnits();
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
