#include "IndividualTransforms.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "ControlledLifetimeResource.h"
#include "Easing.h"
#include <cstring>

namespace Rml {

/*
    The individual transform properties of CSS Transforms 2 (phase 3, docs/html-ui/phase-3-fork-patches.md 3.13.7):
    'translate', 'rotate' and 'scale'. Each value is held as a transform of one primitive, written as the 'transform'
    function that does the same and parsed by the 'transform' parser (so calc() works as in 'transform'); they are not
    inherited, 'none' initially, interpolate as transforms, and are composed by Element::UpdateTransformState as
    translate * rotate * scale * transform around the transform origin.
*/

namespace {

	bool IsNumberToken(const String& token)
	{
		float number = 0.f;
		return Easing::ParseNumber(token, number);
	}

	bool IsMathFunction(const String& token)
	{
		const String lower = StringUtilities::ToLower(token);
		for (const char* name : {"calc(", "min(", "max(", "clamp("})
			if (lower.rfind(name, 0) == 0)
				return true;
		return false;
	}

	// A <number> or <percentage> as a number ('150%' is 1.5); math functions are kept as written.
	bool ScaleComponent(const String& token, String& result)
	{
		float number = 0.f;
		if (Easing::ParseNumber(token, number))
		{
			result = token;
			return true;
		}
		if (token.size() > 1 && token.back() == '%' && Easing::ParseNumber(token.substr(0, token.size() - 1), number))
		{
			result = CreateString("%g", number * 0.01f);
			return true;
		}
		if (IsMathFunction(token))
		{
			result = token;
			return true;
		}
		return false;
	}

	bool IsAngleToken(const String& token)
	{
		if (IsMathFunction(token))
			return true;
		const String lower = StringUtilities::ToLower(token);
		for (const char* unit : {"deg", "rad", "grad", "turn"})
		{
			const size_t length = strlen(unit);
			if (lower.size() > length && lower.compare(lower.size() - length, length, unit) == 0 &&
				IsNumberToken(lower.substr(0, lower.size() - length)))
				return true;
		}
		// A unitless zero angle.
		return token == "0";
	}

} // namespace

PropertyParserIndividualTransform::PropertyParserIndividualTransform(Kind kind) : kind(kind) {}

bool PropertyParserIndividualTransform::ToTransformFunction(Kind kind, const String& value, String& function)
{
	StringList tokens;
	SplitWhitespaceList(value, tokens);
	if (tokens.empty())
		return false;
	if (tokens.size() == 1 && StringUtilities::ToLower(tokens[0]) == "none")
	{
		function = "none";
		return true;
	}

	switch (kind)
	{
	case Kind::Translate:
	{
		// <length-percentage> [<length-percentage> <length>?]?; the 'transform' parser checks the units.
		if (tokens.size() > 3)
			return false;
		const String y = (tokens.size() > 1 ? tokens[1] : String("0px"));
		const String z = (tokens.size() > 2 ? tokens[2] : String("0px"));
		if (z.back() == '%')
			return false;
		function = "translate3d(" + tokens[0] + ", " + y + ", " + z + ")";
		return true;
	}
	case Kind::Rotate:
	{
		// <angle> | [x | y | z | <number>{3}] && <angle>
		if (tokens.size() == 1)
		{
			if (!IsAngleToken(tokens[0]))
				return false;
			function = "rotate(" + tokens[0] + ")";
			return true;
		}
		String angle;
		StringList axis;
		for (const String& token : tokens)
		{
			if (angle.empty() && token != "0" && IsAngleToken(token))
				angle = token;
			else
				axis.push_back(token);
		}
		if (angle.empty())
			return false;
		if (axis.size() == 1)
		{
			const String name = StringUtilities::ToLower(axis[0]);
			if (name == "x")
				function = "rotateX(" + angle + ")";
			else if (name == "y")
				function = "rotateY(" + angle + ")";
			else if (name == "z")
				function = "rotateZ(" + angle + ")";
			else
				return false;
			return true;
		}
		if (axis.size() == 3 && IsNumberToken(axis[0]) && IsNumberToken(axis[1]) && IsNumberToken(axis[2]))
		{
			function = "rotate3d(" + axis[0] + ", " + axis[1] + ", " + axis[2] + ", " + angle + ")";
			return true;
		}
		return false;
	}
	case Kind::Scale:
	{
		// [<number> | <percentage>]{1,3}
		if (tokens.size() > 3)
			return false;
		String components[3];
		for (size_t i = 0; i < tokens.size(); i++)
			if (!ScaleComponent(tokens[i], components[i]))
				return false;
		const String& x = components[0];
		const String& y = (tokens.size() > 1 ? components[1] : components[0]);
		const String z = (tokens.size() > 2 ? components[2] : String("1"));
		function = "scale3d(" + x + ", " + y + ", " + z + ")";
		return true;
	}
	}
	return false;
}

bool PropertyParserIndividualTransform::ParseValue(Property& property, const String& value, const ParameterMap& parameters) const
{
	String function;
	if (!ToTransformFunction(kind, value, function))
		return false;
	PropertyParser* transform_parser = StyleSheetSpecification::GetParser("transform");
	return transform_parser && transform_parser->ParseValue(property, function, parameters);
}

struct IndividualTransformParsers {
	PropertyParserIndividualTransform translate{PropertyParserIndividualTransform::Kind::Translate};
	PropertyParserIndividualTransform rotate{PropertyParserIndividualTransform::Kind::Rotate};
	PropertyParserIndividualTransform scale{PropertyParserIndividualTransform::Kind::Scale};
};

static ControlledLifetimeResource<IndividualTransformParsers> individual_transform_parsers;

void PropertyParserIndividualTransform::Initialize()
{
	individual_transform_parsers.Initialize();
}

void PropertyParserIndividualTransform::Shutdown()
{
	individual_transform_parsers.Shutdown();
}

void PropertyParserIndividualTransform::RegisterParsers()
{
	StyleSheetSpecification::RegisterParser("translate", &individual_transform_parsers->translate);
	StyleSheetSpecification::RegisterParser("rotate", &individual_transform_parsers->rotate);
	StyleSheetSpecification::RegisterParser("scale", &individual_transform_parsers->scale);
}

} // namespace Rml
