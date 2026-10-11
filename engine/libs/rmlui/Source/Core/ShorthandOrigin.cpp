#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#include "PropertyShorthandDefinition.h"
#include "ValuesShorthands.h"

namespace Rml {

// Sets the x and y longhands of an origin from a <position>, each written as the longhand parses it.
static bool ParseOrigin(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const StringList& position, const String* z,
	const PropertySpecification& specification)
{
	if (definition.items.size() < 2)
		return false;

	LengthPercentageValue x, y;
	if (!ParsePosition(position, x, y))
		return false;

	PropertyDictionary parsed;
	specification.SetShorthandDefaults(parsed, definition);
	if (!specification.ParsePropertyDeclaration(parsed, definition.items[0].property_id, x.ToString()) ||
		!specification.ParsePropertyDeclaration(parsed, definition.items[1].property_id, y.ToString()))
		return false;
	if (z && (definition.items.size() < 3 || !specification.ParsePropertyDeclaration(parsed, definition.items[2].property_id, *z)))
		return false;

	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

bool ParseTransformOriginShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	StringList tokens;
	SplitWhitespaceList(value, tokens);
	if (tokens.empty() || tokens.size() > 3)
		return false;

	// A third value is the z length; the position takes one or two values (no edge offsets).
	String z;
	const bool has_z = (tokens.size() == 3);
	if (has_z)
	{
		z = std::move(tokens[2]);
		tokens.pop_back();
	}
	return ParseOrigin(dictionary, definition, tokens, has_z ? &z : nullptr, specification);
}

bool ParsePerspectiveOriginShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	StringList tokens;
	SplitWhitespaceList(value, tokens);
	// The three-value form is background-position's alone (CSS Values 4, <position>).
	if (tokens.size() == 3)
		return false;
	return ParseOrigin(dictionary, definition, tokens, nullptr, specification);
}

} // namespace Rml
