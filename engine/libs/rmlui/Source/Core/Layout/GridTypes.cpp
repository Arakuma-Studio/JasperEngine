/*
    CSS Grid layout, ported from Taffy (https://github.com/DioxusLabs/taffy, commit 8409f3f8, src/compute/grid).
    Taffy is MIT licensed: Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors; see LICENSE-taffy.txt.

    This file ports the helpers of types/grid_track.rs and types/grid_item.rs that resolve values.
*/

#include "GridTypes.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "../Calculation.h"
	#include "../CalculationResolver.h"
#endif

namespace Rml {
namespace GridLayout {

	OptionalFloat TrackLength::Resolve(OptionalFloat basis) const
	{
		switch (type)
		{
		case Type::Px: return value;
		case Type::Percent:
			if (!basis)
				return std::nullopt;
			return value * *basis;
		case Type::Calc:
		{
#ifdef RMLUI_MATH_EXPRESSIONS
			if (calculation_uses_percentage && !basis)
				return std::nullopt;
			float result = 0.f;
			const float percentage_basis = basis ? *basis : 0.f;
			const bool resolved = element ? ResolveElementCalculation(*calculation, *element, percentage_basis, result)
										  : ResolveUsedCalculation(*calculation, percentage_basis, result);
			if (!resolved)
				return std::nullopt;
			return result;
#else
			return std::nullopt;
#endif
		}
		}
		return std::nullopt;
	}

	bool GridItem::HasCyclicBlockSizeDependency() const
	{
		bool height_uses_percentage = (size_height.type == Style::LengthPercentageAuto::Percentage);
#ifdef RMLUI_MATH_EXPRESSIONS
		if (size_height.type == Style::LengthPercentageAuto::Calculation && size_height.calculation)
			height_uses_percentage = Any(size_height.calculation->GetDependencyMask() & Unit::PERCENT);
#endif
		return height_uses_percentage && (crosses_intrinsic_row || crosses_flexible_row);
	}

} // namespace GridLayout
} // namespace Rml
