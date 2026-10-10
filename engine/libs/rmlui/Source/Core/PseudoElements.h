#pragma once

#include "../../Include/RmlUi/Core/Element.h"
#include "CounterResolver.h"

namespace Rml {

class ContentValue;
class ElementText;

/**
    A pseudo-element (docs/html-ui/phase-3-fork-patches.md 3.6): '::before', '::after' or '::marker' of its originating
    element, which owns it as a non-DOM child (invisible to the DOM API, :nth-child() and queries; laid out and painted
    in CSS order, LayoutDetails::GetLayoutChildren). Its definition comes from the rules ending in its pseudo-element
    (StyleSheet::GetElementDefinition(originating element, type)), its computed values inherit from the originating
    element. Its children are made from 'content': the text of its strings, attr(), counter(), counters() and quotes in
    one text element per run, an image element for each url().
 */
class ElementPseudo : public Element {
public:
	RMLUI_RTTI_DeclareWithParent(ElementPseudo, Element)

	explicit ElementPseudo(const String& tag);
	virtual ~ElementPseudo();

	/// The element that generates this pseudo-element.
	Element* GetOriginatingElement() const { return GetParentNode(); }

	/// True when the text depends on counters or quotes: the document's counter pass then hands them over.
	virtual bool UsesCounters() const;
	/// The counter pass (CounterResolver): the counters in scope and the quote depth at this pseudo-element, in tree
	/// order. Updates the text; the quote depth moves past this pseudo-element's quotes.
	virtual void OnCountersResolved(const CssCounterSet& counters, int& quote_depth);

	/// The originating element's attributes changed: an attr() text is made again.
	void OnOriginatingAttributeChange(const ElementAttributes& changed_attributes);

protected:
	void OnUpdate() override;
	void OnPropertyChange(const PropertyIdSet& changed_properties) override;

	/// Makes the children again when the content changed.
	virtual void BuildContent();

	// The children for a list of segments: a text element per run of text segments, an image per url(); their texts are
	// set by SetText.
	void BuildChildren(const ContentValue* content);
	// The text of each text run, with the counters and quote depth given (or the last ones known).
	void UpdateTexts(const CssCounterSet* counters, int* quote_depth);
	// The quote strings at a depth ('quotes' of this element).
	String GetQuote(int depth, bool open);

	SharedPtr<const ContentValue> content;
	Vector<Element*> text_runs; // One per run of text segments, in order (nullptr for a url() child).
	CssCounterSet last_counters;
	int last_quote_depth = 0;
	bool content_dirty = true;
};

/**
    The generation of pseudo-elements: Element::Update calls Update after the element's properties, when its definition
    was matched again or a property markers depend on changed (ElementMeta::pseudo_dirty).
 */
class PseudoElements {
public:
	/// Creates, keeps or removes the element's ::before, ::after and ::marker.
	static void Update(Element& element);
	/// The element's attributes changed (attr() in its pseudo-elements; <ol start>, <li value> for counters).
	static void OnAttributeChange(Element& element, const ElementAttributes& changed_attributes);
	/// The originating element of an element inside a pseudo-element, or the element itself (hit testing, events).
	static Element* GetOriginatingElement(Element* element);
	/// Marks the element's document to resolve its counters before the next layout.
	static void DirtyCounters(Element& element);
};

} // namespace Rml
