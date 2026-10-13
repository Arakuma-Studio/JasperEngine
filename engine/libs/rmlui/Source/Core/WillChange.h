#pragma once

#include "../../Include/RmlUi/Core/StyleTypes.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

/**
    'will-change' and the boxes that contain positioned descendants (CSS Will Change 1 §3.1, CSS Transforms 1/2, Filter
    Effects 1/2, CSS Containment 2, CSS Positioned Layout 3).
 */
namespace WillChange {

	enum Hint : uint8_t {
		StackingContext = 1 << 0,         // a named property creates a stacking context when not initial
		FixedContainingBlock = 1 << 1,    // ... a containing block for fixed (and absolutely positioned) boxes
		AbsoluteContainingBlock = 1 << 2, // ... a containing block for absolutely positioned boxes
	};

	/// The hints of a 'will-change' value: a comma-separated list of property names (ASCII case-insensitive), or 'auto'.
	/// The names are the ones Chromium 153 reads for each hint, including properties the fork does not have (contain,
	/// offset-path, clip-path, mix-blend-mode, ...): a name stands for "a non-initial value of it is coming".
	uint8_t ParseHints(const String& value);

	/// True when the element is the containing block of its 'position: fixed' descendants (instead of the viewport): a
	/// transform (the property or translate/rotate/scale), perspective, a filter, a backdrop filter, or 'will-change' naming
	/// one of them. A mask does not (as in Chromium).
	bool IsFixedPositioningContainingBlock(const Style::ComputedValues& computed);
	/// True when the element is the containing block of its absolutely positioned descendants: positioned, or a containing
	/// block for fixed boxes, or 'will-change: position'.
	bool IsAbsolutePositioningContainingBlock(const Style::ComputedValues& computed);

} // namespace WillChange
} // namespace Rml
