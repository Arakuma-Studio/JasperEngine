#pragma once

#include "CompiledFilterShader.h"
#include "Element.h"
#include "Geometry.h"
#include "Header.h"
#include "TextShapingContext.h"

namespace Rml {

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

	/// Clears all lines of generated text and prepares the element for generating new lines.
	void ClearLines();
	/// Adds a new line into the text element.
	/// @param[in] line_position The position of this line, as an offset from the first line.
	/// @param[in] line The contents of the line.
	void AddLine(Vector2f line_position, String line);
	/// Adds a new line of justified text (p6-justify): the space added around its characters.
	void AddLine(Vector2f line_position, String line, const Vector<JustificationSpacing>& justification);

	/// Prevents the element from dirtying its document's layout when its text is changed.
	void SuppressAutoLayout();

	// Used to store the position and length of each line we have geometry for.
	struct Line {
		Line(String text, Vector2f position) : text(std::move(text)), position(position), width(0) {}
		String text;
		Vector2f position;
		int width;
		// The space a justified line adds around the characters (p6-justify; empty when the line is not justified),
		// included in 'width'.
		Vector<JustificationSpacing> justification;
	};

	using LineList = Vector<Line>;

	// Returns the current list of lines.
	const LineList& GetLines() const { return lines; }

protected:
	void OnRender() override;

	void OnPropertyChange(const PropertyIdSet& properties) override;

	void GetRML(String& content) override;

private:
	// Prepares the font effects this element uses for its font.
	bool UpdateFontEffects();

	// Clears and regenerates all of the text's geometry: the text, its decoration lines and its shadows.
	void GenerateGeometry(RenderManager& render_manager, FontFaceHandle font_face_handle);
	// Generates the decoration lines of 'text-decoration' (phase 3: underline and overline into 'under', drawn below the
	// text, line-through into 'over', drawn above it), in the given colour and offset from the lines.
	void GenerateDecoration(Mesh& under, Mesh& over, FontFaceHandle font_face_handle, ColourbPremultiplied decoration_colour, Vector2f offset);
	// Draws the text shadows behind the text, the last one first; a blurred shadow through a layer and the blur filter.
	void RenderShadows(RenderManager& render_manager, Vector2f translation);

	String text;

	LineList lines;

	struct TexturedGeometry {
		Geometry geometry;
		Texture texture;
	};
	Vector<TexturedGeometry> geometry;

	// The decoration lines: underline and overline are drawn under the text, line-through over it (CSS Text Decoration 3).
	Geometry decoration_under;
	Geometry decoration_over;

	// The shadows of 'text-shadow' (phase 3), in their declared order.
	struct Shadow {
		Vector<TexturedGeometry> geometry; // the text in the shadow's colour, offset
		Geometry decoration;               // and its decoration lines
		float blur = 0.f;                  // CSS blur radius
		Rectanglef ink;                    // what it covers before blurring, in the element's space
		CompiledFilter filter;             // the blur, compiled for the render manager when first drawn
	};
	Vector<Shadow> shadows;

	ColourbPremultiplied colour;
	float opacity;

	int font_handle_version;

	bool geometry_dirty : 1;

	bool dirty_layout_on_change : 1;

	bool font_effects_dirty;
	FontEffectsHandle font_effects_handle;
};

} // namespace Rml
