#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"

namespace Rml {

/**
    A property parser for CSS 'aspect-ratio' (CSS Box Sizing 4, phase 3 values; the layout package computes it):
    'auto || <ratio>', <ratio> = <number [0,inf]> [/ <number [0,inf]>]?.
      'auto'                   Unit::KEYWORD 0
      '<ratio>'                Unit::RATIO, Vector2f(width, height) (height 1 when omitted)
      'auto <ratio>' (either order)  Unit::RATIO, Vector2f(-width, -height): the natural ratio of a replaced element
                               if it has one, else the ratio
 */

class PropertyParserAspectRatio : public PropertyParser {
public:
	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;
};

} // namespace Rml
