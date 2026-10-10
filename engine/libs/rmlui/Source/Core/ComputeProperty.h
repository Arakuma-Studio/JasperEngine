#pragma once

#include "../../Include/RmlUi/Core/NumericValue.h"
#include "../../Include/RmlUi/Core/StyleTypes.h"
#include "../../Include/RmlUi/Core/Traits.h"

namespace Rml {

class Property;

// What a length needs to be resolved to pixels.
struct LengthContext {
	float font_size = 0.f;          // 'em'; and the fallback of 'ch' and 'ex' without a font face (0.5em)
	float root_font_size = 0.f;     // 'rem'
	float dp_ratio = 1.f;           // 'dp' and the physical units
	Vector2f vp_dimensions = {};    // 'vw', 'vh', 'vmin', 'vmax'
	FontFaceHandle font_face = 0;   // 'ch' (advance of '0') and 'ex' (x-height)
};

// Note that numbers and percentages are not lengths, they have to be resolved elsewhere.
float ComputeLength(NumericValue value, const LengthContext& context);
// As above, with the font face of the current FontUnitScope for 'ch' and 'ex'.
float ComputeLength(NumericValue value, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions);

// The size of 'ch' (the advance of '0') and 'ex' (the x-height) in a font face, or half the font size without one.
float ComputeChSize(FontFaceHandle font_face, float font_size);
float ComputeExSize(FontFaceHandle font_face, float font_size);

// The font face the five-scalar compute functions resolve 'ch' and 'ex' against while the scope lives: the element's
// own face while its values are computed (ElementStyle::ComputeValues). Scopes nest; none = half the font size.
class FontUnitScope : NonCopyMoveable {
public:
	explicit FontUnitScope(FontFaceHandle font_face);
	~FontUnitScope();
	void SetFontFace(FontFaceHandle font_face);
	static FontFaceHandle GetFontFace();

private:
	FontFaceHandle previous;
};

float ComputeAngle(NumericValue value);

float ComputeFontsize(const Property* property, const Style::ComputedValues& values, const Style::ComputedValues* parent_values,
	const Style::ComputedValues* document_values, float dp_ratio, Vector2f vp_dimensions);

bool ComputeDefiniteLength(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions,
	float& result);
bool ComputeDefiniteNumber(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions,
	float& result);

String ComputeFontFamily(String font_family);

Style::Clip ComputeClip(const Property* property);

Style::LineHeight ComputeLineHeight(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions);

Style::VerticalAlign ComputeVerticalAlign(const Property* property, float line_height, float font_size, float document_font_size, float dp_ratio,
	Vector2f vp_dimensions);

Style::LengthPercentage ComputeLengthPercentage(const Property* property, float font_size, float document_font_size, float dp_ratio,
	Vector2f vp_dimensions);

Style::LengthPercentageAuto ComputeLengthPercentageAuto(const Property* property, float font_size, float document_font_size, float dp_ratio,
	Vector2f vp_dimensions);

Style::LengthPercentage ComputeOrigin(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions);

Style::LengthPercentage ComputeMaxSize(const Property* property, float font_size, float document_font_size, float dp_ratio, Vector2f vp_dimensions);

uint16_t ComputeBorderWidth(float computed_length);

String GetFontFaceDescription(const String& font_family, Style::FontStyle style, Style::FontWeight weight);

const Style::ComputedValues& DefaultComputedValues();

void InitializeComputeProperty();
void ShutdownComputeProperty();

} // namespace Rml
