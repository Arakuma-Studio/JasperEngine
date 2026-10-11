#pragma once

#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class ElementDocument;

/// A CSS counter (CSS Lists 3): its name, the element that created it, its value.
struct CssCounter {
	String name;
	const Element* origin = nullptr;
	int value = 0;
	bool reversed = false; // Created by reversed(name): list items decrement it.
};
/// The counters in scope at an element, outermost first.
using CssCounterSet = Vector<CssCounter>;

/**
    CSS counters (docs/html-ui/phase-3-fork-patches.md 3.6.3): counter-reset, counter-increment, counter-set, the
    implicit 'list-item' counter, and <ol start>, <ol reversed>, <li value>. A document that uses counters is resolved
    before its layout whenever something they depend on changed (ElementDocument::UpdateLayout): one pre-order walk over
    its elements and pseudo-elements (display: none excluded), inheriting counters as CSS Lists 3 says (from the parent,
    from the preceding sibling by name, values from the previous element in tree order), which then hands every
    pseudo-element the counters in its scope (its counter() text, a list marker's number).
 */
class CounterResolver {
public:
	/// The text of a counter value in a counter style: decimal, decimal-leading-zero, lower-/upper-alpha, lower-/
	/// upper-latin, lower-/upper-roman, lower-greek, disc, circle, square, disclosure-open, disclosure-closed, none, or a
	/// string style ('"' followed by the string). Other names count in decimal (the fallback of every counter style).
	static String FormatCounter(int value, const String& style);
	/// True for the counter styles that are symbols (disc, circle, square, disclosure-open, disclosure-closed).
	static bool IsSymbolStyle(const String& style);

	/// The value of the innermost counter of the name, or 0 when none is in scope.
	static int GetCounterValue(const CssCounterSet& counters, const String& name);
	/// counters(): every counter of the name, outermost first, formatted and joined.
	static String FormatCounters(const CssCounterSet& counters, const String& name, const String& separator, const String& style);

	/// Resolves the counters of a document and updates its pseudo-elements.
	static void Resolve(ElementDocument& document);
};

} // namespace Rml
