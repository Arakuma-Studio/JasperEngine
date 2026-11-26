#include "FontEffectBlur.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "GlyphFilters.h"
#include <functional>
#include <vector>

namespace Rml {

FontEffectBlur::FontEffectBlur()
{
	width = 0;
	std_dev = 1.f;
	SetLayer(Layer::Back);
}

FontEffectBlur::~FontEffectBlur() {}

bool FontEffectBlur::HasUniqueTexture() const
{
	return true;
}

bool FontEffectBlur::Initialise(int _width)
{
	if (_width <= 0)
		return false;

	width = _width;

	std_dev = .4f * float(width);

	return true;
}

bool FontEffectBlur::GetDistanceEffect(DistanceEffect& effect) const
{
	// the kernel's standard deviation (Initialise)
	effect.dilation = 0.f;
	effect.sigma = .4f * float(width);
	effect.offset = Vector2f(0.f);
	return true;
}

bool FontEffectBlur::GetBitmapKey(BitmapKey& key) const
{
	// a glow without outline of the same blur makes the same bitmaps
	key.shape = std::hash<int>()(width) * 31u + 2u;
	key.offset = Vector2i(0);
	return true;
}

bool FontEffectBlur::GetGlyphMetrics(Vector2i& origin, Vector2i& dimensions, const FontGlyph& /*glyph*/) const
{
	if (dimensions.x * dimensions.y > 0)
	{
		origin.x -= width;
		origin.y -= width;

		dimensions.y += 2 * width;
		dimensions.x += 2 * width;

		return true;
	}

	return false;
}

void FontEffectBlur::GenerateGlyphTexture(byte* destination_data, const Vector2i destination_dimensions, int destination_stride,
	const FontGlyph& glyph) const
{
	// Fork (S93 R11): recursive passes in place of the dense kernel
	std::vector<byte> blurred((size_t)destination_dimensions.x * (size_t)destination_dimensions.y);
	GlyphFilters::Place(blurred.data(), destination_dimensions, glyph.bitmap_data, glyph.bitmap_dimensions, Vector2i(width), glyph.color_format);
	GlyphFilters::Blur(blurred.data(), destination_dimensions, width, std_dev);
	GlyphFilters::WriteWhite(destination_data, destination_dimensions, destination_stride, blurred.data());
}

FontEffectBlurInstancer::FontEffectBlurInstancer() : id_width(PropertyId::Invalid), id_color(PropertyId::Invalid)
{
	id_width = RegisterProperty("width", "1px", true).AddParser("length").GetId();
	id_color = RegisterProperty("color", "white", false).AddParser("color").GetId();
	RegisterShorthand("font-effect", "width, color", ShorthandType::FallThrough);
}

FontEffectBlurInstancer::~FontEffectBlurInstancer() {}

SharedPtr<FontEffect> FontEffectBlurInstancer::InstanceFontEffect(const String& /*name*/, const PropertyDictionary& properties)
{
	const Property* p_width = properties.GetProperty(id_width);
#ifdef RMLUI_MATH_EXPRESSIONS
	if (p_width->unit == Unit::CALCULATION)
		return nullptr;
#endif
	float width = p_width->Get<float>();
	Colourb color = properties.GetProperty(id_color)->Get<Colourb>();

	auto font_effect = MakeShared<FontEffectBlur>();
	if (font_effect->Initialise(int(width)))
	{
		font_effect->SetColour(color);
		return font_effect;
	}

	return nullptr;
}

} // namespace Rml
