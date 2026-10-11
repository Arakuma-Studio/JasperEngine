#include "TextProperties.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "ComputeProperty.h"

namespace Rml {

// The keyword values of 'text-align' as registered (StyleSheetSpecification, text section): the first six are the
// Style::TextAlign values.
enum class TextAlignKeyword { Left, Right, Center, Justify, Start, End, MatchParent, WebkitCenter };

Style::TextAlign ComputeTextAlign(const Property* property, Element* element)
{
	const TextAlignKeyword keyword = (TextAlignKeyword)property->Get<int>();
	switch (keyword)
	{
	case TextAlignKeyword::MatchParent:
	{
		// The parent's value; 'start' and 'end' then follow this element's direction, as Chromium resolves them (CSS
		// resolves them against the parent's).
		Element* parent = element ? element->GetParentNode() : nullptr;
		return parent ? parent->GetComputedValues().specified_text_align() : Style::TextAlign::Start;
	}
	case TextAlignKeyword::WebkitCenter: return Style::TextAlign::Center;
	case TextAlignKeyword::Left:
	case TextAlignKeyword::Right:
	case TextAlignKeyword::Center:
	case TextAlignKeyword::Justify:
	case TextAlignKeyword::Start:
	case TextAlignKeyword::End: break;
	}
	return (Style::TextAlign)keyword;
}

float ComputeTextSpacing(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions)
{
	if (property->unit == Unit::KEYWORD)
		return 0.f; // normal
	// Percentages are of the font size (CSS Text 4 §8).
	const Style::LengthPercentage value = ComputeLengthPercentage(property, font_size, document_font_size, dp_ratio, vp_dimensions);
	return ResolveValueOr(value, font_size, 0.f);
}

} // namespace Rml
