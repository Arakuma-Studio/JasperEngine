#pragma once

#include "../../../Include/RmlUi/Core/Types.h"
#include "FormattingContext.h"

namespace Rml {

class LayoutBox;
class ContainerBox;
class FlexContainer;

/*
    Formats a flex container element and its flex items according to flexible box (flexbox) layout rules.

    The algorithm is a port of Taffy's flexbox algorithm (https://github.com/DioxusLabs/taffy, MIT, commit 8409f3f8,
    src/compute/flexbox.rs; LICENSE-taffy.txt), itself CSS Flexible Box Layout 1 §9 with the corrections that make it
    match browsers, measuring items with RmlUi's formatting contexts (LayoutDetails::GetContentWidth, GetContentHeight).
    It adds the automatic minimum size of flex items, the CSS Box Alignment 3 keywords with 'safe', baseline alignment,
    'flex-basis: content' and the sizing keywords, aspect ratios, and 'flex-wrap: balance' with 'flex-line-count'
    (CSS Flexbox 2).
*/
class FlexFormattingContext final : public FormattingContext {
public:
	/// Formats a flex container element and its flex items according to flexbox layout rules.
	static UniquePtr<LayoutBox> Format(ContainerBox* parent_container, Element* element, const Box* override_initial_box);

	/// Computes the max-content size of a flex container's content box.
	/// @param[in] definite_content_height The height of the container's content box when its 'height' is definite, else negative.
	static Vector2f GetMaxContentSize(Element* element, float definite_content_height = -1.f);
	/// Computes the min-content width of a flex container's content box (the height is not computed, 0).
	static Vector2f GetMinContentSize(Element* element, float definite_content_height = -1.f);

private:
	FlexFormattingContext() = default;
};

} // namespace Rml
