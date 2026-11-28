#include "FontEffectGlow.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "GlyphFilters.h"
#include <functional>
#include <vector>

namespace Rml {

FontEffectGlow::FontEffectGlow()
{
	width_blur = 0;
	width_outline = 0;
	combined_width = 0;
	std_dev = 1.f;
	SetLayer(Layer::Back);
}

FontEffectGlow::~FontEffectGlow() {}

bool FontEffectGlow::HasUniqueTexture() const
{
	return true;
}

bool FontEffectGlow::Initialise(int _width_outline, int _width_blur, Vector2i _offset)
{
	if (_width_outline < 0 || _width_blur < 0)
		return false;

	width_outline = _width_outline;
	width_blur = _width_blur;
	combined_width = width_blur + width_outline;
	offset = _offset;

	// the Gaussian of RmlUi's blur kernel
	std_dev = (width_blur == 0 ? 1.f : .4f * float(width_blur));

	return true;
}

bool FontEffectGlow::GetDistanceEffect(DistanceEffect& effect) const
{
	// the outline pass, then the blur's standard deviation (Initialise)
	effect.dilation = (width_outline > 0 ? float(width_outline) + 0.5f : 0.f);
	effect.sigma = (width_blur == 0 ? 0.f : .4f * float(width_blur));
	effect.offset = Vector2f(offset);
	return true;
}

bool FontEffectGlow::GetBitmapKey(BitmapKey& key) const
{
	// the offset moves the quad, not the bitmap: an outline of width_outline is a glow without blur, a blur one without outline
	key.shape = (width_blur == 0) ? std::hash<int>()(width_outline) * 31u + 1u
	            : (width_outline == 0) ? std::hash<int>()(width_blur) * 31u + 2u
	                                   : (std::hash<int>()(width_outline) * 31u + std::hash<int>()(width_blur)) * 31u + 3u;
	key.offset = offset;
	return true;
}

bool FontEffectGlow::GetGlyphMetrics(Vector2i& origin, Vector2i& dimensions, const FontGlyph& /*glyph*/) const
{
	if (dimensions.x * dimensions.y > 0)
	{
		origin.x += offset.x - combined_width;
		origin.y += offset.y - combined_width;

		dimensions.x += 2 * combined_width;
		dimensions.y += 2 * combined_width;

		return true;
	}

	return false;
}

void FontEffectGlow::GenerateGlyphTexture(byte* destination_data, const Vector2i destination_dimensions, int destination_stride,
	const FontGlyph& glyph) const
{
	// Fork (S93 R11): the outline pass as a distance-transform dilation, the blur as recursive passes (no dense kernel; heap
	// buffers, so effect glyphs can be made off the main thread)
	const size_t area = (size_t)destination_dimensions.x * (size_t)destination_dimensions.y;
	std::vector<byte> placed(area), glow(area);
	GlyphFilters::Place(placed.data(), destination_dimensions, glyph.bitmap_data, glyph.bitmap_dimensions, Vector2i(combined_width),
		glyph.color_format);
	GlyphFilters::Dilate(glow.data(), placed.data(), destination_dimensions, width_outline);
	GlyphFilters::Blur(glow.data(), destination_dimensions, width_blur, std_dev);
	GlyphFilters::WriteWhite(destination_data, destination_dimensions, destination_stride, glow.data());
}

FontEffectGlowInstancer::FontEffectGlowInstancer() :
	id_width_outline(PropertyId::Invalid), id_width_blur(PropertyId::Invalid), id_color(PropertyId::Invalid)
{
	id_width_outline = RegisterProperty("width-outline", "1px", true).AddParser("length").GetId();
	id_width_blur = RegisterProperty("width-blur", "-1px", true).AddParser("length").GetId();
	id_offset_x = RegisterProperty("offset-x", "0px", true).AddParser("length").GetId();
	id_offset_y = RegisterProperty("offset-y", "0px", true).AddParser("length").GetId();
	id_color = RegisterProperty("color", "white", false).AddParser("color").GetId();
	RegisterShorthand("font-effect", "width-outline, width-blur, offset-x, offset-y, color", ShorthandType::FallThrough);
}

FontEffectGlowInstancer::~FontEffectGlowInstancer() {}

SharedPtr<FontEffect> FontEffectGlowInstancer::InstanceFontEffect(const String& /*name*/, const PropertyDictionary& properties)
{
	Vector2i offset;
	const Property* p_width_outline = properties.GetProperty(id_width_outline);
	const Property* p_width_blur = properties.GetProperty(id_width_blur);
	const Property* p_offset_x = properties.GetProperty(id_offset_x);
	const Property* p_offset_y = properties.GetProperty(id_offset_y);
#ifdef RMLUI_MATH_EXPRESSIONS
	if (p_width_outline->unit == Unit::CALCULATION || p_width_blur->unit == Unit::CALCULATION || p_offset_x->unit == Unit::CALCULATION ||
		p_offset_y->unit == Unit::CALCULATION)
		return nullptr;
#endif
	int width_outline = p_width_outline->Get<int>();
	int width_blur = p_width_blur->Get<int>();
	offset.x = p_offset_x->Get<int>();
	offset.y = p_offset_y->Get<int>();
	Colourb color = properties.GetProperty(id_color)->Get<Colourb>();

	if (width_blur < 0)
		width_blur = width_outline;

	auto font_effect = MakeShared<FontEffectGlow>();
	if (font_effect->Initialise(width_outline, width_blur, offset))
	{
		font_effect->SetColour(color);
		return font_effect;
	}

	return nullptr;
}

} // namespace Rml
