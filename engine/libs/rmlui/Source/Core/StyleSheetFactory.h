#pragma once

#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class StyleSheetContainer;
enum class StructuralSelectorType;
struct StructuralSelector;

/**
    Creates stylesheets on the fly as needed. The factory keeps a cache of built sheets for optimisation.
 */

class StyleSheetFactory {
public:
	~StyleSheetFactory();

	/// Initialise the style factory
	static bool Initialise();
	/// Shutdown style manager
	static void Shutdown();

	/// Gets the named sheet, retrieving it from the cache if its already been loaded.
	/// @param sheet name of sheet to load
	/// @lifetime Returned pointer is valid until the next call to ClearStyleSheetCache or Shutdown, it should not be stored around.
	static const StyleSheetContainer* GetStyleSheetContainer(const String& sheet);

	/// Clear the style sheet cache.
	static void ClearStyleSheetCache();

	/// Returns one of the available node selectors.
	/// @param name[in] The name of the desired selector, with its parenthesized argument if any (the name in lower case).
	/// @return The selector registered with the given name, or one of type Invalid if none exists or its argument is
	/// invalid.
	static StructuralSelector GetSelector(const String& name);

	/// True for the names of the pseudo-classes elements carry as state (:hover, :active, :focus, :checked, ...), and
	/// those registered with RegisterPseudoClass. A selector with any other pseudo-class is invalid (phase 3: dropped
	/// with a warning instead of a rule that never matches).
	static bool IsPseudoClass(const String& name);
	/// Registers the name of a pseudo-class that an application sets on elements (Element::SetPseudoClass).
	static void RegisterPseudoClass(const String& name);

	/// Parses an An+B expression (CSS Syntax 3), or 'odd'/'even'.
	static bool ParseAnB(const String& text, int& a, int& b);

private:
	StyleSheetFactory();

	// Loads an individual style sheet
	UniquePtr<const StyleSheetContainer> LoadStyleSheetContainer(const String& sheet);

	// Individual loaded stylesheets
	using StyleSheets = UnorderedMap<String, UniquePtr<const StyleSheetContainer>>;
	StyleSheets stylesheets;

	// Custom complex selectors available for style sheets.
	using SelectorMap = UnorderedMap<String, StructuralSelectorType>;
	SelectorMap selectors;

	// The pseudo-classes elements carry as state.
	UnorderedSet<String> pseudo_classes;
};

} // namespace Rml
