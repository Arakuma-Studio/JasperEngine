#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"

namespace Rml {

/**
    A property parser for CSS 'font-family' lists (phase 3, values): a comma-separated list of quoted strings and
    unquoted names, written as the canonical list the font engine resolves from left to right: quotes removed, white
    space collapsed, lowercase, joined by commas without spaces ("inter,segoe ui,sans-serif").
 */

class PropertyParserFontFamily : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;

	/// The canonical list of a 'font-family' value, false when it is not a valid list.
	static bool CanonicalFamilyList(const String& value, String& canonical);
};

} // namespace Rml
