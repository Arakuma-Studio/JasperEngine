#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"

namespace Rml {

/**
    A property parser for CSS 'cursor' (CSS UI 4, phase 3 values): '[<url> [<x> <y>]?,]* <keyword>'. The value is the
    keyword, lowercase (the system interface's SetMouseCursor receives it), or the name of one of the application's
    own cursors as written (an identifier, as 6.3's string allowed); the image entries are dropped, as cursor images
    are not supported (the compatibility layer reports them). More keywords than CSS's one are taken as fallbacks
    ('help, pointer'): the value lists them, and the system interface takes the first it knows.
 */

class PropertyParserCursor : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;

	/// Whether 'keyword' (lowercase) is one of the CSS UI 4 cursor keywords.
	static bool IsCursorKeyword(const String& keyword);
};

} // namespace Rml
