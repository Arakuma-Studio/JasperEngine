#pragma once

#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class ElementDefinition;
class PropertyDefinition;

/**
    Transitions on any change of a computed value (phase 3, docs/html-ui/phase-3-fork-patches.md 3.13.4). An element with
    a transition list keeps the after-change value of each property its list covers; when ElementStyle::ComputeValues finds
    a different value (a class, pseudo-class, inline style, var(), inherited value or script change), it starts a transition
    from the before-change value, or from the running transition's current value with the CSS reversing rule. The functions
    themselves are members of ElementStyle (ElementTransitions.cpp).
 */
struct TransitionSnapshot {
	// The after-change values (in computed form: var() substituted, relative lengths in px) of the last style change.
	PropertyDictionary values;
	// Before-change values taken when the element's definition changed, for properties the values do not hold (a
	// transition list that changes with the definition, as in 'a:hover { transition: ...; }'); used at the next change.
	PropertyDictionary captured;
	// The values were taken: the element has a before-change style.
	bool initialized = false;
	// The values hold every property that may change: every declared and every inherited property ('transition: all').
	bool complete = false;
	// The value the pre-passes of ComputeValues apply (a transition's start value).
	Property applied;
	// The element's definition with its @starting-style rules, looked up once per style computation that needs it.
	SharedPtr<const ElementDefinition> starting_definition;
	bool starting_looked_up = false;
	// The element is shown again (it or an ancestor was 'display: none') without a change of its own style: its next
	// style computation transitions from its starting style.
	bool restart = false;
};

/// The pseudo-class the style sheet parser gives the rules of @starting-style blocks, as a selector suffix.
const char* GetStartingStyleSelectorSuffix();
/// The pseudo-class name of @starting-style rules.
const String& GetStartingStylePseudoClass();
/// Called by the style sheet parser when it reads @starting-style rules; until then no element looks for them.
void NoteStartingStyleRules();
bool HasStartingStyleRules();

/// True if a transition between the two values interpolates: numbers, lengths, colours, transforms, filters, shadows,
/// ... of compatible types, and 'visibility' to or from 'visible'. Other pairs are discrete: they transition only with
/// 'transition-behavior: allow-discrete'.
bool IsInterpolable(const Property& from, const Property& to, Element& element, const PropertyDefinition* definition);

} // namespace Rml
