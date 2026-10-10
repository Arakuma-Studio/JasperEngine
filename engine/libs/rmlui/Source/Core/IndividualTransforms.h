#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"

namespace Rml {

/**
    Parses the individual transform properties 'translate', 'rotate' and 'scale' (CSS Transforms 2) into a transform of
    one primitive (IndividualTransforms.cpp).
 */
class PropertyParserIndividualTransform : public PropertyParser {
public:
	enum class Kind { Translate, Rotate, Scale };

	PropertyParserIndividualTransform(Kind kind);

	bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override;

	/// The 'transform' function that does what the value of the property does ('none' for none).
	static bool ToTransformFunction(Kind kind, const String& value, String& function);

	static void Initialize();
	static void Shutdown();
	static void RegisterParsers();

private:
	Kind kind;
};

} // namespace Rml
