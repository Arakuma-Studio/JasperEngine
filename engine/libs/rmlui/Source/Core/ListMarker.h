#pragma once

#include "../../Include/RmlUi/Core/Geometry.h"
#include "PseudoElements.h"

namespace Rml {

class ElementDefinition;

/**
    The ::marker of a 'display: list-item' element (docs/html-ui/phase-3-fork-patches.md 3.6.4). Its content is the
    'content' of a ::marker rule, else made from the inherited list-style-* values: an image ('list-style-image'),
    a symbol (disc, circle, square, disclosure-open, disclosure-closed: drawn as shapes sized from the font, as
    Chromium draws them), or the item's 'list-item' counter in the counter style followed by ". " (a string style is
    used as written).

    'list-style-position: inside': an inline box, the first of the item's first line. 'outside': an inline-block on
    the first line whose margins cancel its advance and put it outside the item's start edge, where Chromium places
    it (the end of an ordinal marker at the item's content start; a symbol 'ascent * 2 / 3 + 8' px before it); it is
    aligned on the line's baseline and does not change the item's size or the position of its content.
 */
class ElementListMarker final : public ElementPseudo {
public:
	RMLUI_RTTI_DeclareWithParent(ElementListMarker, ElementPseudo)

	explicit ElementListMarker(const String& tag);
	~ElementListMarker();

	/// Whether a list item generates a marker: 'display: list-item' with a list-style-type or a list-style-image, or a
	/// ::marker rule that sets 'content'.
	static bool IsGenerated(Element& element, const ElementDefinition* marker_definition);

	bool UsesCounters() const override;
	void OnCountersResolved(const CssCounterSet& counters, int& quote_depth) override;

	/// The text of the marker (empty for a symbol or an image), for tests.
	const String& GetMarkerText() const { return marker_text; }
	/// The counter style or string the marker shows ("" for an image or a ::marker content).
	const String& GetMarkerStyle() const { return style; }

protected:
	void OnPropertyChange(const PropertyIdSet& changed_properties) override;
	void OnRender() override;
	void OnResize() override;
	void OnLayout() override;
	void BuildContent() override;

private:
	// The generated content: the children for the style, the text for the ordinal.
	void BuildGenerated();
	void UpdateGeneratedText();
	// The box and its margins on the line (display, width, margins as inline properties).
	void UpdateBox();
	void GenerateSymbolGeometry();

	bool custom_content = false; // A ::marker rule sets 'content'.
	String style;                // The list-style-type (or the string, quoted with '"' first).
	String image_source;         // The list-style-image URL, if any.
	int ordinal = 1;
	String marker_text;
	ElementText* text_element = nullptr;
	Element* image_element = nullptr;
	Geometry symbol_geometry;
	bool geometry_dirty = true;
	bool box_dirty = true;
};

namespace ListMarker {
	using Rml::ElementListMarker;
	inline bool IsGenerated(Element& element, const ElementDefinition* marker_definition)
	{
		return ElementListMarker::IsGenerated(element, marker_definition);
	}
} // namespace ListMarker

} // namespace Rml
