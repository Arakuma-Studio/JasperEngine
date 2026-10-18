#pragma once

#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/StyleTypes.h"
#include "../../Include/RmlUi/Core/Texture.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class RenderManager;

/*
    The ellipsis of a line-clamp container's last shown line (fork, phase 6, P6-L1: Chromium 153's legacy
    '-webkit-line-clamp'). As Chromium's LineTruncator draws it: "…" (U+2026, or "..." when the font has no glyph for it)
    in the font and colour of the block container whose line it ends (the line's style, not the clamp container's), on the
    line's baseline, after the last content that fits beside it.

    The text run it follows draws it with its own glyphs (ElementText): it is painted over the line's inline boxes as
    Chromium paints it. After an atomic inline, or on a line of nothing but a <br>, the block container draws it
    (Element::Render, before its children).
*/
struct LineClampEllipsis {
	// The ellipsis's baseline origin: relative to the text element's line positions (a text run's), or to the block
	// container's border box (the block's).
	Vector2f position;
	String text;
	float width = 0.f;
	FontFaceHandle font_face_handle = 0;
	Colourb colour;

	// The block container's text shaping context.
	String language;
	Style::Direction direction = Style::Direction::Auto;
	Style::FontKerning font_kerning = Style::FontKerning::Auto;
	float letter_spacing = 0.f;
	float word_spacing = 0.f;
};

// The ellipsis a block container draws, with its geometry (generated when first rendered).
struct LineClampBlockEllipsis {
	LineClampEllipsis ellipsis;
	Vector<Geometry> geometry;
	Vector<Texture> textures;
	int font_handle_version = -1;
	float opacity = -1.f;
};

// The element state of line clamping, set by the layout (Element befriends it).
class LineClampAccess {
public:
	/// The ellipsis a block container draws itself; null removes it.
	static void SetBlockEllipsis(Element* block, const LineClampEllipsis* ellipsis);
	static bool HasBlockEllipsis(const Element* block);
	/// Hides an atomic inline the ellipsis truncates, with its subtree (Element::SetBox shows it again).
	static void SetHidden(Element* element, bool hidden);
};

namespace LineClamp {

	/// The ellipsis of a line of the block container 'block' (its font, colour and text shaping context), without a
	/// position. False when the block has no font face (no text to end).
	bool MakeEllipsis(Element* block, LineClampEllipsis& out_ellipsis);

	/// Draws the ellipsis a block container owns (Element::Render), clipped as its content is.
	void RenderBlockEllipsis(Element* block, LineClampBlockEllipsis& block_ellipsis);

} // namespace LineClamp

} // namespace Rml
