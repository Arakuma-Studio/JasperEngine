#pragma once

#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class StyleSheetNode;

/**
    Constants used to determine the specificity of a selector.
 */
namespace SelectorSpecificity {
	enum {
		Tag = 10'000,
		Class = 100'000,
		Attribute = Class,
		PseudoClass = Class,
		ID = 1'000'000,
	};
}

/**
    Combinator determines how two connected compound selectors are matched against the element hierarchy.
 */
enum class SelectorCombinator {
	Descendant,        // The 'E F' (whitespace) combinator: Matches if F is a descendant of E.
	Child,             // The 'E > F' combinator: Matches if F is a child of E.
	NextSibling,       // The 'E + F' combinator: Matches if F is immediately preceded by E.
	SubsequentSibling, // The 'E ~ F' combinator: Matches if F is preceded by E.
};

/**
    Attribute selector.

    Such as [unit], [unit=meter], [href^=http]
 */
enum class AttributeSelectorType {
	Always,
	Equal = '=',
	InList = '~',
	BeginsWithThenHyphen = '|',
	BeginsWith = '^',
	EndsWith = '$',
	Contains = '*',
};
struct AttributeSelector {
	AttributeSelectorType type = AttributeSelectorType::Always;
	bool case_insensitive = false; // The 'i' flag: the value is compared ASCII case-insensitively ('s', the default, sensitively).
	String name;
	String value;
};
bool operator==(const AttributeSelector& a, const AttributeSelector& b);
bool operator<(const AttributeSelector& a, const AttributeSelector& b);
using AttributeSelectorList = Vector<AttributeSelector>;

/**
    A tree of unstyled style sheet nodes.
 */
struct SelectorTree {
	UniquePtr<StyleSheetNode> root;
	Vector<StyleSheetNode*> leafs; // Owned by root.
	// For :has(): whether each leaf's relative selector starts with a sibling combinator ('+', '~'): its candidates are
	// the following siblings of the subject and their descendants, else the subject's descendants.
	Vector<bool> sibling_relative;
};

/**
    Tree-structural selector.

    Such as :nth-child(2n+1), :empty(), :not(div)
 */
enum class StructuralSelectorType {
	Invalid,
	Nth_Child,
	Nth_Last_Child,
	Nth_Of_Type,
	Nth_Last_Of_Type,
	First_Child,
	Last_Child,
	First_Of_Type,
	Last_Of_Type,
	Only_Child,
	Only_Of_Type,
	Empty,
	Not,
	Scope,

	// Phase 3, cascade (docs/html-ui/phase-3-fork-patches.md 3.5)
	Root,          // :root, the document element
	Is,            // :is(), :matches(), :-webkit-any(): the most specific argument's specificity
	Where,         // :where(): zero specificity
	Has,           // :has(), relative selectors
	Enabled,       // :enabled
	Disabled,      // :disabled, also from a disabled <fieldset> ancestor
	AnyLink,       // :link, :any-link: a or area with href
	Never,         // :visited, never matches
	ReadOnly,      // :read-only
	ReadWrite,     // :read-write
	Required,      // :required
	Optional,      // :optional
	Indeterminate, // :indeterminate
	Default,       // :default
	Target,        // :target, the element whose id is the document's target (ElementDocument::SetTarget)
};
struct StructuralSelector {
	StructuralSelector(StructuralSelectorType type, int a, int b) : type(type), a(a), b(b) {}
	StructuralSelector(StructuralSelectorType type, SharedPtr<const SelectorTree> tree, int specificity) :
		type(type), specificity(specificity), selector_tree(std::move(tree))
	{}

	StructuralSelectorType type = StructuralSelectorType::Invalid;

	// For counting selectors, the following are the 'a' and 'b' variables of an + b.
	int a = 0;
	int b = 0;

	// Specificity is usually determined like a pseudo class, but some types override this value.
	int specificity = SelectorSpecificity::PseudoClass;

	// For selectors that contain internal selectors such as :not().
	SharedPtr<const SelectorTree> selector_tree;
};
bool operator==(const StructuralSelector& a, const StructuralSelector& b);
bool operator<(const StructuralSelector& a, const StructuralSelector& b);
using StructuralSelectorList = Vector<StructuralSelector>;

/**
    Compound selector contains all the basic selectors for a single node.

    Such as div#foo.bar:nth-child(2)
 */
struct CompoundSelector {
	String tag;
	String id;
	StringList class_names;
	StringList pseudo_class_names;
	AttributeSelectorList attributes;
	StructuralSelectorList structural_selectors;
	SelectorCombinator combinator = SelectorCombinator::Descendant; // Determines how to match with our parent node.
	// The pseudo-element the selector ends in (::before, ::after, ::marker): the compound matches the originating
	// element, and the node's properties style its pseudo-element. Only the last compound of a selector has one.
	PseudoElementType pseudo_element = PseudoElementType::None;
};
bool operator==(const CompoundSelector& a, const CompoundSelector& b);

/// True when the element is disabled for CSS's :disabled (and for focus and clicks, phase 3): a form control with the
/// 'disabled' pseudo-class or attribute, or inside a disabled <fieldset> (except its first <legend>).
bool IsElementDisabled(const Element* element);

/// Returns true if the node the given selector is discriminating for is applicable to a given element.
/// @param element[in] The element to determine node applicability for.
/// @param selector[in] The selector to test against the element.
/// @param scope[in] The element considered as the reference point/scope (for :scope).
bool IsSelectorApplicable(const Element* element, const StructuralSelector& selector, const Element* scope);

} // namespace Rml
