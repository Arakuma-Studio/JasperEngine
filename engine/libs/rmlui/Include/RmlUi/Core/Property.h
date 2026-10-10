#pragma once

#include "Header.h"
#include "NumericValue.h"
#include "Unit.h"
#include "Variant.h"
#include <type_traits>

namespace Rml {

class PropertyDefinition;

/// The cascade origin of a declaration (phase 3). User-agent declarations come from a style sheet linked with
/// origin="user-agent"; they are what the 'revert' keyword restores.
enum class StyleOrigin : uint8_t { Author, UserAgent };

struct RMLUICORE_API PropertySource {
	PropertySource(String path, int line_number, String rule_name) : path(std::move(path)), line_number(line_number), rule_name(std::move(rule_name))
	{}
	String path;
	int line_number;
	String rule_name;
};

class RMLUICORE_API Property {
public:
	Property();
	template <typename PropertyType>
	Property(PropertyType value, Unit unit, int specificity = -1) : value(value), unit(unit), specificity(specificity)
	{
		definition = nullptr;
		parser_index = -1;
	}
	template <typename EnumType, typename = typename std::enable_if_t<std::is_enum_v<EnumType>, EnumType>>
	Property(EnumType value) : value(static_cast<int>(value)), unit(Unit::KEYWORD), specificity(-1)
	{}

	/// Get the value of the property as a string, with ' !important' appended for an important declaration.
	String ToString() const;

	/// Get the value of the property as a numeric value, if applicable.
	NumericValue GetNumericValue() const;

	/// Templatised accessor.
	template <typename T>
	T Get() const
	{
		return value.Get<T>();
	}

	bool operator==(const Property& other) const { return unit == other.unit && value == other.value; }
	bool operator!=(const Property& other) const { return !(*this == other); }

	Variant value;
	Unit unit;
	int specificity;
	/// Declared with '!important' (CSS Cascade 4): stylesheet importance is added to the specificity when the declaration
	/// is imported (see PropertyDictionary::Import), inline importance is compared at lookup (ElementStyle).
	bool important = false;
	/// The origin of the style sheet the declaration comes from.
	StyleOrigin origin = StyleOrigin::Author;

	const PropertyDefinition* definition = nullptr;
	int parser_index = -1;

	SharedPtr<const PropertySource> source;
};

} // namespace Rml
