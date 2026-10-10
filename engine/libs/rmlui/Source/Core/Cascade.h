#pragma once

#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class PropertyDictionary;
class PropertySpecification;
struct ShorthandDefinition;

/**
    The CSS cascade of declarations (phase 3, CSS Cascade 4): importance and the inline style.

    Selector specificities (SelectorSpecificity, a few million at most, plus the rule index) stay far below these offsets:
    an important declaration is imported into a style sheet node with ImportantOffset added to its specificity, an inline
    declaration weighs InlineSpecificity (plus ImportantOffset when important). So, from weakest to strongest: normal
    stylesheet declarations, normal inline declarations, important stylesheet declarations, important inline
    declarations. The animation layers of ElementStyle sit around them: transitions above everything, animations above
    normal declarations and below important ones.
 */
namespace Cascade {

	static constexpr int ImportantOffset = 1 << 29;
	static constexpr int InlineSpecificity = 1 << 28;

	/// The weight of an inline declaration, compared with the specificity of a definition's declaration.
	inline int InlineWeight(const Property& property)
	{
		return InlineSpecificity + (property.important ? ImportantOffset : 0);
	}

	/// True when the inline declaration wins over the declaration of the element's definition.
	inline bool InlineWins(const Property& inline_property, const Property& definition_property)
	{
		return InlineWeight(inline_property) >= definition_property.specificity;
	}

	/// The declaration that applies of an inline declaration and a definition's declaration (either may be null).
	inline const Property* Choose(const Property* inline_property, const Property* definition_property)
	{
		if (!inline_property)
			return definition_property;
		if (!definition_property)
			return inline_property;
		return InlineWins(*inline_property, *definition_property) ? inline_property : definition_property;
	}

	/// Parses a declaration whose value ends in '!important' (ASCII case-insensitive, white space allowed after '!'):
	/// the value before it is parsed by the specification, and every declaration it produces (the longhands of a
	/// shorthand, a custom property, a shorthand pending var() substitution) is marked important.
	/// @return False when the value has no '!important' suffix (nothing is parsed then); otherwise true with 'result' set
	/// to the outcome of the parse.
	bool ParseImportantDeclaration(const PropertySpecification& specification, PropertyDictionary& dictionary, const String& name,
		const String& value, bool& result);

	/// Parses a CSS-wide keyword declaration ('inherit', 'initial', 'unset', 'revert', 'revert-layer'; ASCII
	/// case-insensitive): the property, every longhand of the shorthand ('all': every registered property but the
	/// internal '-rmlui-*' ones), or the custom property is set to the keyword (unit Unit::CSS_WIDE_KEYWORD).
	/// @return False when the value is not a CSS-wide keyword or the name is unknown (nothing is set then).
	bool ParseKeywordDeclaration(const PropertySpecification& specification, PropertyDictionary& dictionary, const String& name,
		const String& value);

	/// The CSS-wide keyword a value is, if any.
	bool ParseKeyword(const String& value, CssWideKeyword& keyword);

	/// Sets every property the 'all' shorthand covers to the keyword: every registered property except the internal
	/// '-rmlui-*' ones (CSS excludes 'direction' and 'unicode-bidi', which RmlUi has as '-rmlui-direction').
	void SetAllProperties(const PropertySpecification& specification, PropertyDictionary& dictionary, CssWideKeyword keyword);

	/// The parser of the 'all' shorthand (ShorthandType::Custom): only CSS-wide keywords are valid.
	bool ParseAllShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
		const PropertySpecification& specification);

	/// Removes the important declarations of a dictionary: CSS ignores them in keyframes.
	void RemoveImportantDeclarations(PropertyDictionary& dictionary);

	/// True when any declaration of the dictionary is important.
	bool HasImportantDeclarations(const PropertyDictionary& dictionary);

} // namespace Cascade
} // namespace Rml
