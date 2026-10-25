#pragma once

#include "CompiledFilterShader.h"
#include "Element.h"
#include "Geometry.h"
#include "Header.h"
#include "TextShapingContext.h"

namespace Rml {

struct LineClampEllipsis;

class RMLUICORE_API ElementText final : public Element {
public:
	RMLUI_RTTI_DeclareWithParent(ElementText, Element)

	ElementText(const String& tag);
	virtual ~ElementText();

	/// Sets the raw string this text element contains. The actual rendered text may be different due to whitespace formatting.
	void SetText(const String& text);
	/// Returns the raw string this text element contains.
	const String& GetText() const;

	/// Generates a line of text rendered from this element.
	/// @param[out] line The characters making up the line, with white-space characters collapsed and endlines processed appropriately.
	/// @param[out] line_length The number of characters from the source string consumed making up this string; this may very well be different from
	/// line.size()!
	/// @param[out] line_width The width (in pixels) of the generated line.
	/// @param[in] line_begin The index of the first character to be rendered in the line.
	/// @param[in] maximum_line_width The width (in pixels) of space allowed for the line, or infinite for unlimited space.
	/// @param[in] right_spacing_width The width (in pixels) of the spacing (consisting of margins, padding, etc.) that must be remaining on the right
	/// of the line if the last of the text is rendered onto this line.
	/// @param[in] trim_whitespace_prefix If we're collapsing whitespace, true to remove all prefixing whitespace, or false to collapse it down to a
	/// single space.
	/// @param[in] decode_escape_characters Decode escaped characters such as &amp; into &.
	/// @param[in] allow_empty Allow no tokens to be consumed from the line.
	/// @return True if the line reached the end of the element's text, false if not.
	bool GenerateLine(String& line, int& line_length, float& line_width, int line_begin, float maximum_line_width, float right_spacing_width,
		bool trim_whitespace_prefix, bool decode_escape_characters, bool allow_empty);

	/// Line breaking across the inline formatting context (fork, P6-L2): whether a line may break before this text's first
	/// character, after the inline content before it (the text of the elements around it, as Chromium computes the
	/// opportunities on the whole context's text: no break inside a word that an element boundary splits); true at the
	/// context's start.
	bool IsLineBreakOpportunityBefore();
	/// Links the texts of the inline formatting context of the block container 'root' in order (fork, P6-L2: the inline
	/// content they go on through, inline boxes, and where it stops, atomic inlines, blocks, <br>), before its layout.
	static void LinkInlineFormattingContext(Element* root);
	/// A few code points of the inline formatting context's text around a text (line breaking, fork P6-L2).
	struct LineBreakContext {
		static constexpr int capacity = 8;
		Character characters[capacity];
		int size = 0;
		bool operator==(const LineBreakContext& other) const
		{
			if (size != other.size)
				return false;
			for (int i = 0; i < size; i++)
			{
				if (characters[i] != other.characters[i])
					return false;
			}
			return true;
		}
		bool operator!=(const LineBreakContext& other) const { return !(*this == other); }
	};
	/// The texts before and after this one in its inline formatting context, as last linked (null: none, or an atomic
	/// inline, a block or a <br> between them).
	ElementText* GetContextPrevious() const { return context_previous; }
	/// Links this text after 'previous' in its inline formatting context (null: it starts it).
	void LinkAfter(ElementText* previous);
	ElementText* GetContextNext() const { return context_next; }

	/// Clears all lines of generated text and prepares the element for generating new lines.
	void ClearLines();
	/// Adds a new line into the text element.
	/// @param[in] line_position The position of this line, as an offset from the first line.
	/// @param[in] line The contents of the line.
	void AddLine(Vector2f line_position, String line);
	/// Adds a new line of justified text (p6-justify): the space added around its characters.
	void AddLine(Vector2f line_position, String line, const Vector<JustificationSpacing>& justification);
	/// Makes the line added last the one that a line-clamp ellipsis ends (fork, phase 6, P6-L1): its text is what fits beside
	/// the ellipsis, drawn after it with the ellipsis's own font and colour (LineClampEllipsis, positioned from the line's
	/// position).
	void SetLineClampEllipsis(UniquePtr<LineClampEllipsis> ellipsis);

	/// Prevents the element from dirtying its document's layout when its text is changed.
	void SuppressAutoLayout();

	// Used to store the position and length of each line we have geometry for.
	struct Line {
		Line(String text, Vector2f position) : text(std::move(text)), position(position), width(0) {}
		String text;
		Vector2f position;
		int width;
		// A justified line (p6-justify): its index among the element's justified lines (GetJustification), -1 otherwise.
		int justification_index = -1;
	};

	using LineList = Vector<Line>;

	// Returns the current list of lines.
	const LineList& GetLines() const { return lines; }
	/// Returns the space a justified line adds around its characters (p6-justify), included in its width; empty when the
	/// line is not justified.
	Span<const JustificationSpacing> GetJustification(const Line& line) const
	{
		return line.justification_index < 0 ? Span<const JustificationSpacing>() : Span<const JustificationSpacing>(justifications[line.justification_index]);
	}

	/// Text geometry with its texture.
	struct TexturedGeometry {
		Geometry geometry;
		Texture texture;
	};
	/// The geometry of one 'text-shadow' of a text.
	struct Shadow {
		Vector<TexturedGeometry> geometry; // the text in the shadow's colour, offset
		Geometry decoration;               // and its decoration lines
		float blur = 0.f;                  // CSS blur radius
		Rectanglef ink;                    // what it covers before blurring, in the element's space
		CompiledFilter filter;             // the blur, compiled for the render manager when first drawn
		bool gpu = false;                  // fork (S16-F3): blurred by the font engine (distance glyphs), no layer
	};
	/// Draws text shadows, the last one first (the first is on top, CSS Text Decoration 3 §4); a blurred shadow through a
	/// layer and the blur filter. Used by the text and by the line-clamp ellipsis a block container draws (fork, P6-L1).
	/// @param[in] transformed The geometry is drawn under a transform (no scissor from the shadows' ink then).
	static void RenderShadowList(RenderManager& render_manager, Vector<Shadow>& shadows, Vector2f translation, bool transformed);

	/// Fork (S16-F3): how the text was last drawn. Bitmap: rasterized at 'scale' device px per document px (0: the font
	/// engine's own scale, no transform-aware text); DistanceField: from distance glyphs; Stretched: a bitmap of another
	/// scale, waiting to settle at the on-screen one.
	enum class RasterMode { Bitmap, DistanceField, Stretched };
	RasterMode GetRasterMode(float& scale) const;

protected:
	void OnRender() override;

	void OnPropertyChange(const PropertyIdSet& properties) override;

	void GetRML(String& content) override;

private:
	// Prepares the font effects this element uses for its font.
	bool UpdateFontEffects();

	// Clears and regenerates all of the text's geometry: the text, its decoration lines and its shadows.
	void GenerateGeometry(RenderManager& render_manager, FontFaceHandle font_face_handle);
	// Fork (S16-F3): transform-aware text. At render: the text's on-screen scale, and how it is drawn (a bitmap at that scale,
	// distance glyphs while it animates, above 128 device px or under perspective, the bitmap stretched while neither is
	// ready); regenerates the geometry when that changes and asks for another frame while the text settles.
	void UpdateRasterScale(RenderManager& render_manager, FontFaceHandle font_face_handle);
	// The base raster scale times the larger singular value of the text's transform at its centre; non_affine: perspective there.
	float ComputeScreenScale(float base_scale, bool& non_affine);
	// Every line's glyphs ready at the on-screen scale (bitmaps within the font engine's budget) or as distance glyphs.
	bool PrepareLines(FontFaceHandle font_face_handle, bool distance);
	// Generates the decoration lines of 'text-decoration' (phase 3: underline and overline into 'under', drawn below the
	// text, line-through into 'over', drawn above it), in the given colour and offset from the lines.
	void GenerateDecoration(Mesh& under, Mesh& over, FontFaceHandle font_face_handle, ColourbPremultiplied decoration_colour, Vector2f offset);

	String text;

	LineList lines;
	// The spacing of the justified lines (Line::justification_index).
	Vector<Vector<JustificationSpacing>> justifications;

	Vector<TexturedGeometry> geometry;

	// The decoration lines: underline and overline are drawn under the text, line-through over it (CSS Text Decoration 3).
	Geometry decoration_under;
	Geometry decoration_over;

	// The shadows of 'text-shadow' (phase 3), in their declared order.
	Vector<Shadow> shadows;

	// The line-clamp ellipsis after one of the lines (fork, P6-L1), and that line; its glyphs and the block container's
	// text shadows, drawn after the text as Chromium paints the ellipsis after the item it follows.
	UniquePtr<LineClampEllipsis> line_clamp_ellipsis;
	int line_clamp_ellipsis_line = -1;
	Vector<TexturedGeometry> line_clamp_ellipsis_geometry;
	Vector<Shadow> line_clamp_ellipsis_shadows;

	ColourbPremultiplied colour;
	float opacity;

	int font_handle_version;

	// Fork (S16-F3): the scale the geometry was made at (0: the font engine's own), the on-screen scale of the last render,
	// the renders since it last changed, and whether the geometry draws distance glyphs.
	float raster_scale = 0.f;
	float last_screen_scale = 0.f;
	int frames_since_scale_change = 0;
	bool distance_field = false;

	// The line-break opportunities of the text (TextUnicode::GetLineBreakOpportunities, tailored as Chromium), by the byte
	// offset of the character a line may break before; computed when lines are first generated after the text changes.
	Vector<uint8_t> line_breaks; // (empty for a text of printable ASCII and white space: TextUnicode::IsAsciiLineBreakOpportunity)
	Style::WordBreak line_breaks_word_break = Style::WordBreak::Normal;
	bool line_breaks_decode_escape_characters = false;
	bool line_breaks_dirty = true;
	// The inline formatting context's text before and after this one the opportunities were computed with (fork, P6-L2: a
	// few code points of the texts around it, through inline boxes), and whether a line may break at its start and end.
	LineBreakContext line_breaks_prefix;
	LineBreakContext line_breaks_suffix;
	bool line_break_before = true;
	bool line_break_after = true;
	bool line_breaks_ascii = false; // (no table: printable ASCII and white space, ComputeLineBreaks)
	// (the context is gathered once per layout: each linking of the context stamps its texts anew)
	uint32_t context_stamp = 0;
	uint32_t line_breaks_stamp = 0;
	// The texts before and after this one in its inline formatting context (LinkInlineFormattingContext).
	// (a text unlinks itself from its neighbours when it is destroyed; the links are made again before each layout)
	ElementText* context_previous = nullptr;
	ElementText* context_next = nullptr;

	void UpdateLineBreaks(bool decode_escape_characters);
	void EnsureLineBreaks(bool decode_escape_characters);
	float MeasureLeadingWord(bool& out_whole);
	float MeasureGluedContentAfter();

	bool geometry_dirty : 1;

	bool dirty_layout_on_change : 1;

	bool font_effects_dirty;
	FontEffectsHandle font_effects_handle;
};

} // namespace Rml
