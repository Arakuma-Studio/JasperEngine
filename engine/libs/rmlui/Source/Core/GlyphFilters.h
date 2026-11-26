#pragma once

#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {
namespace GlyphFilters {

	// Fork (S93 R11): the font effects' glyph filters in time linear in the bitmap's area, whatever the effect's width. RmlUi
	// ran each as a dense ConvolutionFilter: O(area x (2w+1)^2) for an outline, seconds for big outlined text.
	// Every function reads and writes A8 buffers of 'dimensions' (stride = width) and is safe to call from any thread.

	// Copies the alpha of a glyph bitmap (A8 or RGBA8) into an A8 buffer of 'dimensions' at 'offset', zero elsewhere.
	void Place(byte* destination, Vector2i dimensions, const byte* source, Vector2i source_dimensions, Vector2i offset, ColorFormat source_format);

	// The dilation of FontEffectOutline's kernel: each pixel is the largest source(q) * weight(|p - q|), the weight 1 within
	// 'radius' and falling to 0 over the next pixel. Exact Euclidean distance transforms (Felzenszwalb and Huttenlocher) of
	// the fully covered pixels and of the covered ones give the result; a pixel whose value comes from partly covered ones
	// only (the ring's antialiased edge, thin strokes) takes the largest of the covered pixels around its nearest one.
	// 'radius' 0 copies. destination and source must not overlap.
	void Dilate(byte* destination, const byte* source, Vector2i dimensions, int radius);

	// The blur of FontEffectBlur and FontEffectGlow: per axis, the normalised kernel exp(-|x| / (sqrt(2) * std_dev)) over
	// [-radius, radius], each pass truncated to bytes as ConvolutionFilter's were. The kernel is a two-sided exponential,
	// so each pass is a pair of recursive sums: O(area) for any radius. In place.
	void Blur(byte* buffer, Vector2i dimensions, int radius, float std_dev);

	// Writes an A8 buffer into the RGBA8 region of a glyph texture as premultiplied white (FontEffect::FillColorValuesFromAlpha).
	void WriteWhite(byte* destination, Vector2i dimensions, int destination_stride, const byte* alpha);

} // namespace GlyphFilters
} // namespace Rml
