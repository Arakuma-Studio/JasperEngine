#pragma once

#include "Header.h"
#include "ID.h"
#include "PropertyIdSet.h"
#include "Types.h"

namespace Rml {

class StyleSheetSpecification;
class PropertySpecification;
class PropertyDefinition;
class PropertyDictionary;
class PropertyIdNameMap;
class ShorthandIdNameMap;
struct ShorthandDefinition;

enum class ShorthandType {
	// Normal; properties that fail to parse fall-through to the next until they parse correctly, and any
	// undeclared are not set.
	FallThrough,
	// A single failed parse will abort, and any undeclared are replicated from the last declared property.
	Replicate,
	// For 'padding', 'margin', etc; up to four properties are expected.
	Box,
	// Repeatedly resolves the full value string on each property, whether it is a normal property or another shorthand.
	RecursiveRepeat,
	// Comma-separated list of properties or shorthands, the number of declared values must match the specified.
	RecursiveCommaSeparated,
	// The 'flex' shorthand has some special behavior but otherwise acts like 'FallThrough'.
	Flex,
	// CSS-style shorthand with its items in any order ('border', 'outline', 'list-style', ...): the value is split at
	// whitespace, each value is assigned to the first item (in declaration order) not assigned yet that parses it, and a
	// value no item takes fails the declaration. Items not given a value are set to their initial value. Items may be
	// properties or shorthands; each takes one value.
	AnyOrder,
	// A shorthand whose grammar needs code of its own: the parser registered for it with
	// StyleSheetSpecification::RegisterShorthandParser receives the raw value and sets every longhand (omitted ones to
	// their initial value). Items may be properties or shorthands.
	Custom
};

/// Parses the value of a 'Custom' shorthand into the dictionary: every longhand of the shorthand is set, those the value
/// omits to their initial value. Called with the declared value as written, after var() detection (a value with var()
/// is substituted at computed-value time and parsed then). Returns false when the value is invalid; the dictionary is
/// then left unchanged by the caller.
using ShorthandParserFunction = bool (*)(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

/// Splits a comma-separated list value at its top-level commas: commas inside parentheses and quotes do not split.
/// Every item is stripped of surrounding whitespace; empty items are kept, so that 'a,,b' can be rejected by the caller.
RMLUICORE_API void SplitCommaList(const String& value, StringList& items);
/// Splits a value at its top-level whitespace: whitespace inside parentheses and quotes does not split. Empty items are
/// skipped.
RMLUICORE_API void SplitWhitespaceList(const String& value, StringList& items);

/**
    A property specification stores a group of property definitions.
 */

class RMLUICORE_API PropertySpecification {
public:
	PropertySpecification(size_t reserve_num_properties, size_t reserve_num_shorthands);
	~PropertySpecification();

	/// Registers a property with a new definition.
	/// @param[in] property_name The name to register the new property under.
	/// @param[in] default_value The default value to be used for an element if it has no other definition provided.
	/// @param[in] inherited True if this property is inherited from parent to child, false otherwise.
	/// @param[in] forces_layout True if this property requires its parent to be reformatted if changed.
	/// @param[in] id If 'Invalid' then automatically assigns a new id, otherwise assigns the given id.
	/// @return The new property definition, ready to have parsers attached.
	PropertyDefinition& RegisterProperty(const String& property_name, const String& default_value, bool inherited, bool forces_layout,
		PropertyId id = PropertyId::Invalid);
	/// Returns a property definition.
	/// @param[in] id The id of the desired property.
	/// @return The appropriate property definition if it could be found, nullptr otherwise.
	const PropertyDefinition* GetProperty(PropertyId id) const;
	const PropertyDefinition* GetProperty(const String& property_name) const;

	/// Returns the id set of all registered property definitions.
	const PropertyIdSet& GetRegisteredProperties() const;
	/// Returns the id set of all registered inherited property definitions.
	const PropertyIdSet& GetRegisteredInheritedProperties() const;
	/// Returns the id set of all registered property definitions that may dirty the layout.
	const PropertyIdSet& GetRegisteredPropertiesForcingLayout() const;

	/// Registers a shorthand property definition.
	/// @param[in] shorthand_name The name to register the new shorthand property under.
	/// @param[in] property_names A comma-separated list of the properties this definition is shorthand for. The order
	/// in which they are specified here is the order in which the values will be processed.
	/// @param[in] type The type of shorthand to declare.
	/// @param[in] id If 'Invalid' then automatically assigns a new id, otherwise assigns the given id.
	/// @return An ID for the new shorthand, or 'Invalid' if the shorthand declaration is invalid.
	ShorthandId RegisterShorthand(const String& shorthand_name, const String& property_names, ShorthandType type,
		ShorthandId id = ShorthandId::Invalid);
	/// Sets the parser of a registered shorthand of type 'Custom'.
	/// @return True on success, false if the shorthand does not exist or is not of type 'Custom'.
	bool SetShorthandParser(ShorthandId id, ShorthandParserFunction parser);
	/// Returns a shorthand definition.
	/// @param[in] id The id of the desired shorthand.
	/// @return The appropriate shorthand definition if it could be found, nullptr otherwise.
	const ShorthandDefinition* GetShorthand(ShorthandId id) const;
	const ShorthandDefinition* GetShorthand(const String& shorthand_name) const;

	/// Parse declaration by name, whether it's a property or shorthand.
	bool ParsePropertyDeclaration(PropertyDictionary& dictionary, const String& property_name, const String& property_value) const;
	/// Parse property declaration by ID.
	bool ParsePropertyDeclaration(PropertyDictionary& dictionary, PropertyId property_id, const String& property_value) const;
	/// Parses a shorthand declaration, setting any parsed and validated properties on the given dictionary.
	/// @return True if all properties were parsed successfully, false otherwise.
	bool ParseShorthandDeclaration(PropertyDictionary& dictionary, ShorthandId shorthand_id, const String& property_value) const;

	/// Sets all undefined properties in the dictionary to their defaults.
	/// @param dictionary[in-out] The dictionary to set the default values on.
	void SetPropertyDefaults(PropertyDictionary& dictionary) const;
	/// Sets every longhand of a shorthand, recursively, to its initial (default) value: what CSS shorthands do with the
	/// items their value omits.
	void SetShorthandDefaults(PropertyDictionary& dictionary, const ShorthandDefinition& definition) const;

	/// Returns the properties of dictionary converted to a string.
	String PropertiesToString(const PropertyDictionary& dictionary, bool include_name, char delimiter) const;

private:
	using Properties = Vector<UniquePtr<PropertyDefinition>>;
	using Shorthands = Vector<UniquePtr<ShorthandDefinition>>;

	Properties properties;
	Shorthands shorthands;

	UniquePtr<PropertyIdNameMap> property_map;
	UniquePtr<ShorthandIdNameMap> shorthand_map;

	PropertyIdSet property_ids;
	PropertyIdSet property_ids_inherited;
	PropertyIdSet property_ids_forcing_layout;
	// Properties whose parser reads the value as written, quotes included (SplitOption::Raw; phase 3: font-family).
	PropertyIdSet property_ids_raw_value;

	enum class ParsePropertyResult { Success, ContainsVariable, Error };
	enum class SplitOption { None, Whitespace, Comma, Raw };
	ParsePropertyResult ParsePropertyValues(StringList& values_list, const String& values, SplitOption split_option) const;
	bool ParseAnyOrderShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const StringList& values) const;

	friend class Rml::StyleSheetSpecification;
	friend class TestPropertySpecification;
};

} // namespace Rml
