#pragma once

#include "PropertyDictionary.h"
#include "Spritesheet.h"
#include "StyleSheetTypes.h"
#include "Traits.h"

namespace Rml {

class Element;
class ElementDefinition;
class StyleSheetNode;
class Decorator;
class RenderManager;
class SpritesheetList;
class StyleSheetContainer;
class StyleSheetParser;
struct PropertySource;
struct Sprite;
enum class PseudoElementType : uint8_t;

using DecoratorPtrList = Vector<SharedPtr<const Decorator>>;

/**
    StyleSheet maintains a single stylesheet definition. A stylesheet can be combined with another stylesheet to create
    a new, merged stylesheet.
 */

class RMLUICORE_API StyleSheet final : public NonCopyMoveable {
public:
	~StyleSheet();

	/// Combines this style sheet with another one, producing a new sheet.
	UniquePtr<StyleSheet> CombineStyleSheet(const StyleSheet& sheet) const;
	/// Merges another style sheet into this.
	void MergeStyleSheet(const StyleSheet& sheet);

	/// Builds the node index for a combined style sheet.
	void BuildNodeIndex();

	/// Sets the cascade origin of every declaration of the sheet (phase 3): a user-agent sheet's declarations are what the
	/// 'revert' keyword restores. Set before the sheet is combined with others.
	void SetOrigin(StyleOrigin origin);

	/// Returns the named @decorator, or null if it does not exist.
	const NamedDecorator* GetNamedDecorator(const String& name) const;

	/// Returns the Keyframes of the given name, or null if it does not exist.
	/// @lifetime The returned pointer becomes invalidated whenever the style sheet is re-generated. Do not store this pointer or references to
	/// subobjects around.
	const Keyframes* GetKeyframes(const String& name) const;

	/// Get sprite located in any spritesheet within this stylesheet.
	/// @lifetime The returned pointer becomes invalidated whenever the style sheet is re-generated. Do not store this pointer or references to
	/// subobjects around.
	const Sprite* GetSprite(const String& name) const;

	/// Returns the compiled element definition for a given element and its hierarchy.
	SharedPtr<const ElementDefinition> GetElementDefinition(const Element* element) const;
	/// Returns the compiled element definition of a pseudo-element (phase 3): the rules ending in that pseudo-element
	/// whose selector matches the originating element; null when none does.
	SharedPtr<const ElementDefinition> GetElementDefinition(const Element* originating_element, PseudoElementType type) const;
	/// True when some rule of the sheet ends in the pseudo-element.
	bool HasPseudoElementRules(PseudoElementType type) const;
	/// True when some rule of the sheet ends in a pseudo-element.
	bool HasAnyPseudoElementRules() const
	{
		return !styled_node_index.pseudo_element_nodes[0].empty() || !styled_node_index.pseudo_element_nodes[1].empty() ||
			!styled_node_index.pseudo_element_nodes[2].empty();
	}
	/// What the :has() selectors of the sheet depend on (invalidation).
	const StyleSheetIndex::HasDependencies& GetHasDependencies() const { return styled_node_index.has; }

	/// Returns a list of instanced decorators from the declarations. The instances are cached for faster future retrieval.
	const DecoratorPtrList& InstanceDecorators(RenderManager& render_manager, const DecoratorDeclarationList& declaration_list,
		const PropertySource* decorator_source) const;

private:
	StyleSheet();

	// Root level node, attributes from special nodes like "body" get added to this node
	UniquePtr<StyleSheetNode> root;

	// The maximum specificity offset used in this style sheet to distinguish between properties in
	// similarly-specific rules, but declared on different lines. When style sheets are merged, the
	// more-specific style sheet (ie, coming further 'down' the include path) adds the offset of
	// the less-specific style sheet onto its offset, thereby ensuring its properties take
	// precedence in the event of a conflict.
	int specificity_offset;

	// Name of every @keyframes mapped to their keys
	KeyframesMap keyframes;

	// Name of every @decorator mapped to their specification
	NamedDecoratorMap named_decorator_map;

	// Name of every @spritesheet and underlying sprites mapped to their values
	SpritesheetList spritesheet_list;

	// Map of all styled nodes, that is, they have one or more properties.
	StyleSheetIndex styled_node_index;

	// Index of node sets to element definitions.
	using ElementDefinitionCache = UnorderedMap<StyleSheetIndex::NodeList, SharedPtr<const ElementDefinition>>;
	mutable ElementDefinitionCache node_cache;

	// Cached decorator instances.
	using DecoratorCache = UnorderedMap<String, Vector<SharedPtr<const Decorator>>>;
	mutable DecoratorCache decorator_cache;

	friend Rml::StyleSheetParser;
	friend Rml::StyleSheetContainer;
};

} // namespace Rml
