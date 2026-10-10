#pragma once

#include "../../Include/RmlUi/Core/Element.h"

namespace Rml {

/**
    The element internals the phase-3 cascade code works with (selector invalidation, pseudo-elements, counters,
    focus): a friend of Element, so that this code lives in its own files and Element.cpp only calls it.
 */
class ElementCascade {
public:
	/// Dirties the style definition of an element (and what depends on it through combinators).
	static void DirtyDefinition(Element* element, bool siblings)
	{
		element->DirtyDefinition(siblings ? Element::DirtyNodes::SelfAndSiblings : Element::DirtyNodes::Self);
	}

	/// What changed on an element, for the :has() selectors of its style sheet.
	enum class Change {
		Structure,   // a DOM child was added to or removed from the element
		Class,       // the class 'name' was set or removed
		PseudoClass, // the pseudo-class 'name' was set or removed
		Attribute,   // the attribute 'name' changed (id and class included)
	};
	/// The elements whose :has() selectors may match differently after the change are re-matched: the ancestors of
	/// the element (and the element itself for a change of its children), and their preceding siblings when a :has()
	/// argument starts with a sibling combinator. Nothing is done when the sheet has no :has() or none depends on the
	/// name: hover changes do not re-match documents.
	static void OnHasDependencyChange(Element* element, Change change, const String& name);

	/// True when the element cannot take the focus: 'focus: none', or a disabled form control (phase 3: disabled controls
	/// no longer get an inline 'focus: none').
	static bool IsUnfocusable(const Element* element);

	/// Dirties the layout of the element's document.
	static void DirtyLayout(Element& element) { element.DirtyLayout(); }

	/// Computes the element's dirty properties now (outside of the update loop: a list marker whose text changed while
	/// the counters were resolved, right before the layout that reads its box).
	static void UpdateProperties(Element& element);

	/// The slot of the element's pseudo-element of a type (ElementMeta).
	static Element*& PseudoElementSlot(Element& element, PseudoElementType type);
	/// Marks the element to generate its pseudo-elements again (PseudoElements::Update).
	static void DirtyPseudoElements(Element& element);
	/// Clears the mark and returns whether it was set.
	static bool TakePseudoDirty(Element& element);

	/// Marks the document to resolve its counters before its next layout (CounterResolver), and remembers that it uses
	/// counters (its structure changes then dirty them too).
	static void DirtyCounters(ElementDocument& document);
	/// Dirties the counters of the element's document when the document uses counters (a structural change).
	static void DirtyCountersIfUsed(Element& element);
	/// Resolves the document's counters when dirty (ElementDocument::UpdateLayout, before formatting).
	static void ResolveCounters(ElementDocument& document);

private:
	static void MarkHasSubject(Element* element, bool in_non_subject);
};

} // namespace Rml
