#pragma once

#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "StyleSheetSelector.h"

namespace Rml {

struct StyleSheetIndex;
class StyleSheetNode;
using StyleSheetNodeList = Vector<UniquePtr<StyleSheetNode>>;

/**
    A style sheet is composed of a tree of nodes.
 */

class StyleSheetNode {
public:
	StyleSheetNode();
	StyleSheetNode(StyleSheetNode* parent, const CompoundSelector& selector);
	StyleSheetNode(StyleSheetNode* parent, CompoundSelector&& selector);

	/// Retrieves or creates a child node with requirements equivalent to the 'other' node.
	StyleSheetNode* GetOrCreateChildNode(const CompoundSelector& other);
	/// Retrieves a child node with the given requirements if they match an existing node, or else creates a new one.
	StyleSheetNode* GetOrCreateChildNode(CompoundSelector&& other);

	/// Merges an entire tree hierarchy into our hierarchy.
	void MergeHierarchy(StyleSheetNode* node, int specificity_offset = 0);
	/// Copy this node including all descendent nodes.
	UniquePtr<StyleSheetNode> DeepCopy(StyleSheetNode* parent = nullptr) const;
	/// Builds up a style sheet's index recursively.
	void BuildIndex(StyleSheetIndex& styled_node_index) const;

	/// The compound selector of the node.
	const CompoundSelector& GetSelector() const { return selector; }
	/// The parent node, nullptr for the root node.
	const StyleSheetNode* GetParent() const { return parent; }

	/// Imports properties from a single rule definition into the node's properties and sets the appropriate specificity on them. Any existing
	/// attributes sharing a key with a new attribute will be overwritten if they are of a lower specificity.
	/// @param[in] properties The properties to import.
	/// @param[in] rule_specificity The specificity of the importing rule.
	void ImportProperties(const PropertyDictionary& properties, int rule_specificity);
	/// Returns the node's default properties.
	const PropertyDictionary& GetProperties() const;
	/// Sets the cascade origin of the properties of this node and its descendants.
	void SetOrigin(StyleOrigin origin);
	/// The declarations of the user-agent origin merged into this node, kept even where an author declaration of the same
	/// selector overrides them (they are what 'revert' restores), or nullptr when there are none.
	const PropertyDictionary* GetUserAgentProperties() const { return user_agent_properties.get(); }

	/// Returns true if this node is applicable to the given element, given its IDs, classes and heritage.
	/// @note For performance reasons this call does not check whether 'element' is a text element. The caller must manually check this condition and
	/// consider any text element not applicable.
	bool IsApplicable(const Element* element, const Element* scope) const;

	/// Returns the specificity of this node.
	int GetSpecificity() const;

private:
	void CalculateAndSetSpecificity();

	// Match an element to the local node requirements.
	inline bool Match(const Element* element, const Element* scope) const;
	inline bool MatchStructuralSelector(const Element* element, const Element* scope) const;
	inline bool MatchAttributes(const Element* element) const;

	// Recursively traverse the nodes up towards the root to match the element and its hierarchy.
	bool TraverseMatch(const Element* element, const Element* scope) const;

	// The parent of this node; is nullptr for the root node.
	StyleSheetNode* parent = nullptr;

	// Node requirements
	CompoundSelector selector;

	// A measure of specificity of this node; the attribute in a node with a higher value will override those of a node with a lower value.
	int specificity = 0;

	PropertyDictionary properties;
	UniquePtr<PropertyDictionary> user_agent_properties;

	StyleSheetNodeList children;
};

} // namespace Rml
