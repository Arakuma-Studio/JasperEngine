#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "ComputeProperty.h"
#include "ElementStyle.h"
#include "PropertiesIterator.h"
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
