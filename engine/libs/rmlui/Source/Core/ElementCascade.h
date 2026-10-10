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

private:
	static void MarkHasSubject(Element* element, bool in_non_subject);
};

} // namespace Rml
