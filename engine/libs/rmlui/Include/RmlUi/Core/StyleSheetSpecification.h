#pragma once

#include "Header.h"
#include "PropertySpecification.h"
#include "Types.h"

namespace Rml {

class PropertyParser;
struct DefaultStyleSheetParsers;
struct RegisteredCustomProperties;

/// The syntax of a registered custom property (CSS Properties and Values API 1, '@property'; fork, phase 5). Each one is
/// read by its own parser: 'Universal' ('*') keeps the value as written; 'Time' is a number of seconds.
enum class RegisteredPropertySyntax : uint8_t { Universal, Number, Integer, Length, Percentage, LengthPercentage, Color, Angle, Time };

/// The registration of a registered custom property.
struct RegisteredCustomProperty {
	RegisteredPropertySyntax syntax = RegisteredPropertySyntax::Universal;
	bool inherits = true;
	// False only for the universal syntax without 'initial-value': its initial value is the guaranteed-invalid value.
	bool has_initial_value = true;
	String initial_value;
};

class RMLUICORE_API StyleSheetSpecification {
public:
	/// Starts up the specification structure and registers default properties and type parsers.
	static void Initialise();
	/// Destroys the specification structure and releases the parsers.
	static void Shutdown();

	/// Registers a parser for use in property definitions.
	/// @param[in] parser_name The name to register the new parser under.
	/// @param[in] parser A non-owning pointer to the parser to register.
	/// @return True if the parser was registered successfully, false otherwise.
	/// @lifetime The parser must be kept alive until after the call to Rml::Shutdown.
	static bool RegisterParser(const String& parser_name, PropertyParser* parser);
	/// Returns the parser registered with a specific name.
	/// @param[in] parser_name The name of the desired parser.
	/// @return The parser registered under the given name, or nullptr if no such parser exists.
	static PropertyParser* GetParser(const String& parser_name);

	/// Registers a custom property with a new definition.
	/// @param[in] property_name The name to register the new property under.
	/// @param[in] default_value The default value to be used for an element if it has no other definition provided.
	/// @param[in] inherited True if this property is inherited from parent to child, false otherwise.
	/// @param[in] forces_layout True if a change in this property on an element will cause the element's layout to possibly change.
	/// @return The new property definition, ready to have parsers attached.
	static PropertyDefinition& RegisterProperty(const String& property_name, const String& default_value, bool inherited, bool forces_layout = false);
	/// Returns a property definition.
	/// @param[in] property_name The name of the desired property.
	/// @return The appropriate property definition if it could be found, nullptr otherwise.
	static const PropertyDefinition* GetProperty(const String& property_name);
	static const PropertyDefinition* GetProperty(PropertyId id);

	/// Returns the id set of all registered property definitions.
	static const PropertyIdSet& GetRegisteredProperties();
	/// Returns the id set of all registered inherited property definitions.
	static const PropertyIdSet& GetRegisteredInheritedProperties();
	/// Returns the id set of all registered property definitions that may dirty the layout.
	static const PropertyIdSet& GetRegisteredPropertiesForcingLayout();

	/// Registers a custom shorthand property definition.
	/// @param[in] shorthand_name The name to register the new shorthand property under.
	/// @param[in] property_names A comma-separated list of the properties this definition is shorthand for. The order in which they are specified
	/// here is the order in which the values will be processed.
	/// @param[in] type The type of shorthand to declare.
	/// @return The new shorthand ID if successfully parsed, Invalid otherwise.
	static ShorthandId RegisterShorthand(const String& shorthand_name, const String& property_names, ShorthandType type);
	/// Registers the parser of a shorthand of type 'Custom' (see ShorthandParserFunction).
	/// @param[in] id The shorthand, registered with ShorthandType::Custom.
	/// @param[in] parser The function that parses its value and sets every longhand.
	/// @return True on success, false if the shorthand is not a registered custom shorthand.
	static bool RegisterShorthandParser(ShorthandId id, ShorthandParserFunction parser);
	/// Returns a shorthand definition.
	/// @param[in] shorthand_name The name of the desired shorthand.
	/// @return The appropriate shorthand definition if it could be found, nullptr otherwise.
	static const ShorthandDefinition* GetShorthand(const String& shorthand_name);
	static const ShorthandDefinition* GetShorthand(ShorthandId id);

	/// Parses a property declaration, setting any parsed and validated properties on the given dictionary.
	/// @param[in] dictionary The property dictionary which will hold all declared properties.
	/// @param[in] property_name The name of the declared property.
	/// @param[in] property_value The values the property is being set to.
	/// @return True if all properties were parsed successfully, false otherwise.
	static bool ParsePropertyDeclaration(PropertyDictionary& dictionary, const String& property_name, const String& property_value);

	static PropertyId GetPropertyId(const String& property_name);
	static ShorthandId GetShorthandId(const String& shorthand_name);
	static const String& GetPropertyName(PropertyId id);
	static const String& GetShorthandName(ShorthandId id);

	// Get the underlying property ids associated by a shorthand.
	static PropertyIdSet GetShorthandUnderlyingProperties(ShorthandId id);

	static const PropertySpecification& GetPropertySpecification();

	/// Fork (phase 5, P5-1): the id of '-jfx-text-fill', a filter list the element applies before its 'filter' list (the
	/// engine's gradient text: effect layers clipped to the text, in their filter form). Invalid before Initialise.
	static PropertyId GetTextFillPropertyId();
	/// Registered custom properties (fork, phase 5; CSS Properties and Values API 1). A registered '--name' is a property like
	/// the others, typed by the parser of its syntax, inherited or not, with an initial value: the cascade, transitions and
	/// animations treat it as one, and var(--name) substitutes its computed value on the element (ElementStyle). A value
	/// that does not match the syntax is kept as written and is invalid at computed-value time, as in CSS.
	/// Registers '--name' with the parser of its syntax and marks it. An empty initial value is none (the universal syntax
	/// only). Returns the property's id: a name already registered is left as it is (its id is returned whatever its
	/// registration; compare it with GetRegisteredCustomProperty). Returns PropertyId::Invalid when the name is not a custom
	/// property name or the name of another property, when the initial value does not match the syntax, or when no id is
	/// free (GetNumFreePropertyIds).
	static PropertyId RegisterCustomProperty(const String& name, RegisteredPropertySyntax syntax, bool inherits, const String& initial_value);
	/// Marks a property registered with RegisterProperty as a registered custom property (RegisterCustomProperty does).
	static void MarkRegisteredCustomProperty(PropertyId id, const RegisteredCustomProperty& registration = RegisteredCustomProperty());
	static bool IsRegisteredCustomProperty(PropertyId id);
	/// The registration of a registered custom property, nullptr for other ids.
	static const RegisteredCustomProperty* GetRegisteredCustomProperty(PropertyId id);
	/// The id set of all registered custom properties.
	static const PropertyIdSet& GetRegisteredCustomProperties();
	/// True once a custom property is registered (until Rml::Shutdown): the style code skips its work for them before.
	static bool AnyRegisteredCustomProperties() { return any_registered_custom_properties; }
	/// The number of property ids still free for RegisterProperty and RegisterCustomProperty (ids are limited to
	/// PropertyId::MaxNumIds).
	static int GetNumFreePropertyIds();

private:
	StyleSheetSpecification();
	~StyleSheetSpecification();

	PropertyDefinition& RegisterProperty(PropertyId id, const String& property_name, const String& default_value, bool inherited,
		bool forces_layout = false);
	ShorthandId RegisterShorthand(ShorthandId id, const String& shorthand_name, const String& property_names, ShorthandType type);

	// Registers RmlUi's default parsers.
	void RegisterDefaultParsers();
	// Registers RmlUi's default style properties.
	void RegisterDefaultProperties();

	// Parsers used by all property definitions.
	typedef UnorderedMap<String, PropertyParser*> ParserMap;
	ParserMap parsers;

	// The properties defined in the style sheet specification.
	PropertySpecification properties;

	UniquePtr<DefaultStyleSheetParsers> default_parsers;

	PropertyId text_fill_id = PropertyId::Invalid;
	// Registered custom properties (fork, phase 5).
	UniquePtr<RegisteredCustomProperties> registered_custom_properties;
	static bool any_registered_custom_properties;
};

} // namespace Rml
