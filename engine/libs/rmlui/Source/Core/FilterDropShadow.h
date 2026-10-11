#pragma once

#include "../../Include/RmlUi/Core/Filter.h"
#include "../../Include/RmlUi/Core/ID.h"
#include "../../Include/RmlUi/Core/NumericValue.h"

namespace Rml {

class FilterDropShadow : public Filter {
public:
	bool Initialise(Colourb color, NumericValue offset_x, NumericValue offset_y, NumericValue sigma);
#ifdef RMLUI_MATH_EXPRESSIONS
	bool Initialise(Colourb color, NumericValue offset_x, NumericValue offset_y, NumericValue sigma, CalculationPtr offset_x_calculation,
		CalculationPtr offset_y_calculation, CalculationPtr sigma_calculation);
#endif

	CompiledFilter CompileFilter(Element* element) const override;

	void ExtendInkOverflow(Element* element, Rectanglef& scissor_region) const override;

	// Without a colour the shadow takes the element's colour (CSS: 'currentcolor', phase 3 p3-paint).
	void SetCurrentColor() { current_color = true; }

private:
	Colourb color;
	bool current_color = false;
	NumericValue value_offset_x, value_offset_y, value_sigma;
#ifdef RMLUI_MATH_EXPRESSIONS
	CalculationPtr calculation_offset_x, calculation_offset_y, calculation_sigma;
#endif
};

class FilterDropShadowInstancer : public FilterInstancer {
public:
	FilterDropShadowInstancer();

	SharedPtr<Filter> InstanceFilter(const String& name, const PropertyDictionary& properties) override;

private:
	struct PropertyIds {
		PropertyId color, offset_x, offset_y, sigma;
	};
	PropertyIds ids;
};

} // namespace Rml
