#pragma once

#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementText.h"
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
    in the font, colour and text shadows of the block container whose line it ends (the line's style, not the clamp
    container's, nor the text run's), without text decorations, on the line's baseline, after the last content that fits
    beside it.
    The text run it follows draws it with its own glyphs (ElementText), after its text. After an atomic inline, or on a line
    of nothing but a <br>, the block container draws it, painted by its stacking context in the inline content after that
    element and its descendants (before it when it is positioned or a stacking context: painted later, over it):
    over earlier inline content, inline boxes' backgrounds and the backgrounds of block-level boxes, under positioned
    ones, as Chromium paints the line's items (Element::BuildLocalStackingContext, RenderStackingContextChildren).
*/
struct LineClampEllipsis {
	// The ellipsis's baseline origin: relative to the text element's line positions (a text run's), or to the block
	// container's border box (the block's).
	Vector2f position;
	String text;
	float width = 0.f;
	FontFaceHandle font_face_handle = 0;
	Colourb colour;
	// The block container: its colour and text shadows when drawn (the colour above when it is gone).
	Element* block = nullptr;
	// The block container's text shaping context.
	String language;
	Style::Direction direction = Style::Direction::Auto;
	Style::FontKerning font_kerning = Style::FontKerning::Auto;
	float letter_spacing = 0.f;
	float word_spacing = 0.f;
};

// The ellipsis a block container draws, with its geometry (generated when first rendered), and the element it is painted
// after (an atomic inline or a <br>).
struct LineClampBlockEllipsis {
	LineClampEllipsis ellipsis;
	Element* after = nullptr;
	// Where its stacking context paints it, found while building the stacking context: after this element of it, before
	// any when null (Element::BuildLocalStackingContext).
	Element* paint_after = nullptr;
	bool placed = false;
	Vector<ElementText::TexturedGeometry> geometry;
	Vector<ElementText::Shadow> shadows;
	int font_handle_version = -1;
	float opacity = -1.f;
	bool dirty = true;
};

// The element state of line clamping, set by the layout (Element befriends it).
class LineClampAccess {
public:
	/// The ellipsis a block container draws itself, painted after the element 'after' and its descendants; null removes it.
	static void SetBlockEllipsis(Element* block, const LineClampEllipsis* ellipsis, Element* after = nullptr);
	/// The colour or text shadows of the block container changed: its ellipsis is generated again.
	static void DirtyBlockEllipsis(Element* block);
	static bool HasBlockEllipsis(const Element* block) { return block->draws_line_clamp_ellipsis; }
	/// The ellipsis the block container draws itself, null when none.
	static LineClampBlockEllipsis* GetBlockEllipsis(Element* block);
	static bool IsStackingContext(const Element* element) { return element->local_stacking_context; }
	/// LineClamp::RenderBlockEllipsis.
	static void RenderBlockEllipsis(Element* block);
	/// Hides an atomic inline the ellipsis truncates, with its subtree (Element::SetBox shows it again).
	static void SetHidden(Element* element, bool hidden);
};

namespace LineClamp {

	/// The ellipsis of a line of the block container 'block' (its font, colour and text shaping context), without a
	/// position. False when the block has no font face (no text to end).
	bool MakeEllipsis(Element* block, LineClampEllipsis& out_ellipsis);

	/// The geometry of an ellipsis with its baseline origin at 'origin': its glyphs in the block container's colour, and the
	/// block container's text shadows.
	void GenerateEllipsis(RenderManager& render_manager, const LineClampEllipsis& ellipsis, Vector2f origin, float opacity,
		Vector<ElementText::TexturedGeometry>& out_geometry, Vector<ElementText::Shadow>& out_shadows);
	/// Paints the ellipsis a block container draws (after an atomic inline or a <br>), with the block container's
	/// transform and clip: its stacking context calls it where the ellipsis is painted (Element::BuildLocalStackingContext).
	void RenderBlockEllipsis(Element* block);

} // namespace LineClamp

} // namespace Rml
