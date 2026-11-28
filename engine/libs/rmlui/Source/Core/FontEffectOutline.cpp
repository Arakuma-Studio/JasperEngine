#include "FontEffectOutline.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "GlyphFilters.h"
#include <functional>
#include <vector>

namespace Rml {

FontEffectOutline::FontEffectOutline()
{
	width = 0;
	SetLayer(Layer::Back);
}

FontEffectOutline::~FontEffectOutline() {}

bool FontEffectOutline::HasUniqueTexture() const
{
	return true;
}

bool FontEffectOutline::Initialise(int _width)
{
	if (_width <= 0)
		return false;

	width = _width;

	return true;
}

bool FontEffectOutline::GetDistanceEffect(DistanceEffect& effect) const
{
	// the dilation filter's weights fall off over the last pixel: the ring's edge at width + 0.5
	effect.dilation = float(width) + 0.5f;
	effect.sigma = 0.f;
	effect.offset = Vector2f(0.f);
	return true;
}

bool FontEffectOutline::GetBitmapKey(BitmapKey& key) const
{
	// a glow of the same outline and no blur makes the same bitmaps
	key.shape = std::hash<int>()(width) * 31u + 1u;
	key.offset = Vector2i(0);
	return true;
}

bool FontEffectOutline::GetGlyphMetrics(Vector2i& origin, Vector2i& dimensions, const FontGlyph& /*glyph*/) const
{
	if (dimensions.x * dimensions.y > 0)
	{
		origin.x -= width;
		origin.y -= width;

		dimensions.x += 2 * width;
		dimensions.y += 2 * width;

		return true;
	}

	return false;
}

void FontEffectOutline::GenerateGlyphTexture(byte* destination_data, const Vector2i destination_dimensions, int destination_stride,
	const FontGlyph& glyph) const
{
	// Fork (S93 R11): a distance-transform dilation in place of the dense kernel
	const size_t area = (size_t)destination_dimensions.x * (size_t)destination_dimensions.y;
	std::vector<byte> placed(area), dilated(area);
	GlyphFilters::Place(placed.data(), destination_dimensions, glyph.bitmap_data, glyph.bitmap_dimensions, Vector2i(width), glyph.color_format);
	GlyphFilters::Dilate(dilated.data(), placed.data(), destination_dimensions, width);
	GlyphFilters::WriteWhite(destination_data, destination_dimensions, destination_stride, dilated.data());
}

FontEffectOutlineInstancer::FontEffectOutlineInstancer() : id_width(PropertyId::Invalid), id_color(PropertyId::Invalid)
{
	id_width = RegisterProperty("width", "1px", true).AddParser("length").GetId();
	id_color = RegisterProperty("color", "white", false).AddParser("color").GetId();
	RegisterShorthand("font-effect", "width, color", ShorthandType::FallThrough);
}

FontEffectOutlineInstancer::~FontEffectOutlineInstancer() {}

SharedPtr<FontEffect> FontEffectOutlineInstancer::InstanceFontEffect(const String& /*name*/, const PropertyDictionary& properties)
{
	const Property* p_width = properties.GetProperty(id_width);
#ifdef RMLUI_MATH_EXPRESSIONS
	if (p_width->unit == Unit::CALCULATION)
		return nullptr;
#endif
	float width = p_width->Get<float>();
	Colourb color = properties.GetProperty(id_color)->Get<Colourb>();

	auto font_effect = MakeShared<FontEffectOutline>();
	if (font_effect->Initialise(int(width)))
	{
		font_effect->SetColour(color);
		return font_effect;
	}

	return nullptr;
}

} // namespace Rml
