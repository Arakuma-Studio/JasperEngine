#include "PropertyParserColour.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "ColorSpaces.h"
#include "ControlledLifetimeResource.h"
#include <cmath>
#include <stdlib.h>
#include <string.h>

namespace Rml {

using ColorSpaces::Color;
using ColorSpaces::Space;

struct PropertyParserColourData {
	// CSS Color 4 named colors (148) and 'transparent'.
	const UnorderedMap<String, Colourb> html_colours = {
		{"aliceblue", Colourb(240, 248, 255)},
		{"antiquewhite", Colourb(250, 235, 215)},
		{"aqua", Colourb(0, 255, 255)},
		{"aquamarine", Colourb(127, 255, 212)},
		{"azure", Colourb(240, 255, 255)},
		{"beige", Colourb(245, 245, 220)},
		{"bisque", Colourb(255, 228, 196)},
		{"black", Colourb(0, 0, 0)},
		{"blanchedalmond", Colourb(255, 235, 205)},
		{"blue", Colourb(0, 0, 255)},
		{"blueviolet", Colourb(138, 43, 226)},
		{"brown", Colourb(165, 42, 42)},
		{"burlywood", Colourb(222, 184, 135)},
		{"cadetblue", Colourb(95, 158, 160)},
		{"chartreuse", Colourb(127, 255, 0)},
		{"chocolate", Colourb(210, 105, 30)},
		{"coral", Colourb(255, 127, 80)},
		{"cornflowerblue", Colourb(100, 149, 237)},
		{"cornsilk", Colourb(255, 248, 220)},
		{"crimson", Colourb(220, 20, 60)},
		{"cyan", Colourb(0, 255, 255)},
		{"darkblue", Colourb(0, 0, 139)},
		{"darkcyan", Colourb(0, 139, 139)},
		{"darkgoldenrod", Colourb(184, 134, 11)},
		{"darkgray", Colourb(169, 169, 169)},
		{"darkgreen", Colourb(0, 100, 0)},
		{"darkgrey", Colourb(169, 169, 169)},
		{"darkkhaki", Colourb(189, 183, 107)},
		{"darkmagenta", Colourb(139, 0, 139)},
		{"darkolivegreen", Colourb(85, 107, 47)},
		{"darkorange", Colourb(255, 140, 0)},
		{"darkorchid", Colourb(153, 50, 204)},
		{"darkred", Colourb(139, 0, 0)},
		{"darksalmon", Colourb(233, 150, 122)},
		{"darkseagreen", Colourb(143, 188, 143)},
		{"darkslateblue", Colourb(72, 61, 139)},
		{"darkslategray", Colourb(47, 79, 79)},
		{"darkslategrey", Colourb(47, 79, 79)},
		{"darkturquoise", Colourb(0, 206, 209)},
		{"darkviolet", Colourb(148, 0, 211)},
		{"deeppink", Colourb(255, 20, 147)},
		{"deepskyblue", Colourb(0, 191, 255)},
		{"dimgray", Colourb(105, 105, 105)},
		{"dimgrey", Colourb(105, 105, 105)},
		{"dodgerblue", Colourb(30, 144, 255)},
		{"firebrick", Colourb(178, 34, 34)},
		{"floralwhite", Colourb(255, 250, 240)},
		{"forestgreen", Colourb(34, 139, 34)},
		{"fuchsia", Colourb(255, 0, 255)},
		{"gainsboro", Colourb(220, 220, 220)},
		{"ghostwhite", Colourb(248, 248, 255)},
		{"gold", Colourb(255, 215, 0)},
		{"goldenrod", Colourb(218, 165, 32)},
		{"gray", Colourb(128, 128, 128)},
		{"green", Colourb(0, 128, 0)},
		{"greenyellow", Colourb(173, 255, 47)},
		{"grey", Colourb(128, 128, 128)},
		{"honeydew", Colourb(240, 255, 240)},
		{"hotpink", Colourb(255, 105, 180)},
		{"indianred", Colourb(205, 92, 92)},
		{"indigo", Colourb(75, 0, 130)},
		{"ivory", Colourb(255, 255, 240)},
		{"khaki", Colourb(240, 230, 140)},
		{"lavender", Colourb(230, 230, 250)},
		{"lavenderblush", Colourb(255, 240, 245)},
		{"lawngreen", Colourb(124, 252, 0)},
		{"lemonchiffon", Colourb(255, 250, 205)},
		{"lightblue", Colourb(173, 216, 230)},
		{"lightcoral", Colourb(240, 128, 128)},
		{"lightcyan", Colourb(224, 255, 255)},
		{"lightgoldenrodyellow", Colourb(250, 250, 210)},
		{"lightgray", Colourb(211, 211, 211)},
		{"lightgreen", Colourb(144, 238, 144)},
		{"lightgrey", Colourb(211, 211, 211)},
		{"lightpink", Colourb(255, 182, 193)},
		{"lightsalmon", Colourb(255, 160, 122)},
		{"lightseagreen", Colourb(32, 178, 170)},
		{"lightskyblue", Colourb(135, 206, 250)},
		{"lightslategray", Colourb(119, 136, 153)},
		{"lightslategrey", Colourb(119, 136, 153)},
		{"lightsteelblue", Colourb(176, 196, 222)},
		{"lightyellow", Colourb(255, 255, 224)},
		{"lime", Colourb(0, 255, 0)},
		{"limegreen", Colourb(50, 205, 50)},
		{"linen", Colourb(250, 240, 230)},
		{"magenta", Colourb(255, 0, 255)},
		{"maroon", Colourb(128, 0, 0)},
		{"mediumaquamarine", Colourb(102, 205, 170)},
		{"mediumblue", Colourb(0, 0, 205)},
		{"mediumorchid", Colourb(186, 85, 211)},
		{"mediumpurple", Colourb(147, 112, 219)},
		{"mediumseagreen", Colourb(60, 179, 113)},
		{"mediumslateblue", Colourb(123, 104, 238)},
		{"mediumspringgreen", Colourb(0, 250, 154)},
		{"mediumturquoise", Colourb(72, 209, 204)},
		{"mediumvioletred", Colourb(199, 21, 133)},
		{"midnightblue", Colourb(25, 25, 112)},
		{"mintcream", Colourb(245, 255, 250)},
		{"mistyrose", Colourb(255, 228, 225)},
		{"moccasin", Colourb(255, 228, 181)},
		{"navajowhite", Colourb(255, 222, 173)},
		{"navy", Colourb(0, 0, 128)},
		{"oldlace", Colourb(253, 245, 230)},
		{"olive", Colourb(128, 128, 0)},
		{"olivedrab", Colourb(107, 142, 35)},
		{"orange", Colourb(255, 165, 0)},
		{"orangered", Colourb(255, 69, 0)},
		{"orchid", Colourb(218, 112, 214)},
		{"palegoldenrod", Colourb(238, 232, 170)},
		{"palegreen", Colourb(152, 251, 152)},
		{"paleturquoise", Colourb(175, 238, 238)},
		{"palevioletred", Colourb(219, 112, 147)},
		{"papayawhip", Colourb(255, 239, 213)},
		{"peachpuff", Colourb(255, 218, 185)},
		{"peru", Colourb(205, 133, 63)},
		{"pink", Colourb(255, 192, 203)},
		{"plum", Colourb(221, 160, 221)},
		{"powderblue", Colourb(176, 224, 230)},
		{"purple", Colourb(128, 0, 128)},
		{"rebeccapurple", Colourb(102, 51, 153)},
		{"red", Colourb(255, 0, 0)},
		{"rosybrown", Colourb(188, 143, 143)},
		{"royalblue", Colourb(65, 105, 225)},
		{"saddlebrown", Colourb(139, 69, 19)},
		{"salmon", Colourb(250, 128, 114)},
		{"sandybrown", Colourb(244, 164, 96)},
		{"seagreen", Colourb(46, 139, 87)},
		{"seashell", Colourb(255, 245, 238)},
		{"sienna", Colourb(160, 82, 45)},
		{"silver", Colourb(192, 192, 192)},
		{"skyblue", Colourb(135, 206, 235)},
		{"slateblue", Colourb(106, 90, 205)},
		{"slategray", Colourb(112, 128, 144)},
		{"slategrey", Colourb(112, 128, 144)},
		{"snow", Colourb(255, 250, 250)},
		{"springgreen", Colourb(0, 255, 127)},
		{"steelblue", Colourb(70, 130, 180)},
		{"tan", Colourb(210, 180, 140)},
		{"teal", Colourb(0, 128, 128)},
		{"thistle", Colourb(216, 191, 216)},
		{"tomato", Colourb(255, 99, 71)},
		{"transparent", Colourb(0, 0, 0, 0)},
		{"turquoise", Colourb(64, 224, 208)},
		{"violet", Colourb(238, 130, 238)},
		{"wheat", Colourb(245, 222, 179)},
		{"white", Colourb(255, 255, 255)},
		{"whitesmoke", Colourb(245, 245, 245)},
		{"yellow", Colourb(255, 255, 0)},
		{"yellowgreen", Colourb(154, 205, 50)},
	};
};

ControlledLifetimeResource<PropertyParserColourData> PropertyParserColour::parser_data;

namespace {

	// Nested color-mix() levels.
	constexpr int MaxColorDepth = 8;

	bool IsSpace(char c)
	{
		return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
	}

	String Trim(const String& value)
	{
		size_t begin = 0, end = value.size();
		while (begin < end && IsSpace(value[begin]))
			++begin;
		while (end > begin && IsSpace(value[end - 1]))
			--end;
		return value.substr(begin, end - begin);
	}

	bool EqualsNoCase(const String& value, const char* literal)
	{
		const size_t length = strlen(literal);
		if (value.size() != length)
			return false;
		for (size_t i = 0; i < length; i++)
		{
			char c = value[i];
			if (c >= 'A' && c <= 'Z')
				c = char(c - 'A' + 'a');
			if (c != literal[i])
				return false;
		}
		return true;
	}

	// Splits the arguments of a color function at the top level: at commas when there is one ('comma' set), else at white
	// space, with '/' a token of its own.
	bool SplitArguments(const String& arguments, StringList& out, bool& comma)
	{
		out.clear();
		comma = false;
		int depth = 0;
		for (char c : arguments)
		{
			if (c == '(')
				depth++;
			else if (c == ')')
			{
				if (--depth < 0)
					return false;
			}
			else if (c == ',' && depth == 0)
				comma = true;
		}
		if (depth != 0)
			return false;

		String current;
		depth = 0;
		auto flush = [&]() {
			if (!current.empty())
				out.push_back(std::move(current));
			current.clear();
		};
		for (char c : arguments)
		{
			if (c == '(')
				depth++;
			else if (c == ')')
				depth--;

			if (depth == 0 && comma && c == ',')
			{
				current = Trim(current);
				if (current.empty())
					return false;
				out.push_back(std::move(current));
				current.clear();
			}
			else if (depth == 0 && !comma && IsSpace(c))
				flush();
			else if (depth == 0 && !comma && c == '/')
			{
				flush();
				out.push_back("/");
			}
			else
				current += c;
		}
		if (comma)
		{
			current = Trim(current);
			if (current.empty())
				return false;
			out.push_back(std::move(current));
		}
		else
			flush();
		return true;
	}

	bool ParseNumber(const String& token, float& value)
	{
		if (token.empty())
			return false;
		char* end = nullptr;
		value = strtof(token.c_str(), &end);
		return end == token.c_str() + token.size() && std::isfinite(value);
	}

	bool ParsePercentage(const String& token, float& value)
	{
		if (token.size() < 2 || token.back() != '%')
			return false;
		return ParseNumber(token.substr(0, token.size() - 1), value);
	}

	// A hue: a number (degrees) or an angle.
	bool ParseHue(const String& token, float& degrees)
	{
		if (ParseNumber(token, degrees))
			return true;
		size_t unit = token.size();
		while (unit > 0 && ((token[unit - 1] >= 'a' && token[unit - 1] <= 'z') || (token[unit - 1] >= 'A' && token[unit - 1] <= 'Z')))
			--unit;
		float number = 0.f;
		if (unit == token.size() || !ParseNumber(token.substr(0, unit), number))
			return false;
		const String suffix = token.substr(unit);
		if (EqualsNoCase(suffix, "deg"))
			degrees = number;
		else if (EqualsNoCase(suffix, "rad"))
			degrees = number * (180.f / Math::RMLUI_PI);
		else if (EqualsNoCase(suffix, "grad"))
			degrees = number * 0.9f;
		else if (EqualsNoCase(suffix, "turn"))
			degrees = number * 360.f;
		else
			return false;
		return true;
	}

	enum class Kind { Number, Percentage, None, Invalid };

	// A number or percentage component; 'none' when allowed.
	Kind ParseComponent(const String& token, float& value, bool allow_none)
	{
		if (allow_none && EqualsNoCase(token, "none"))
		{
			value = 0.f;
			return Kind::None;
		}
		if (ParsePercentage(token, value))
			return Kind::Percentage;
		if (ParseNumber(token, value))
			return Kind::Number;
		return Kind::Invalid;
	}

	// The alpha of a color, a number from 0 to 1 or a percentage; 'none' in the space syntax.
	bool ParseAlpha(const String& token, Color& color, bool allow_none)
	{
		float value = 0.f;
		switch (ParseComponent(token, value, allow_none))
		{
		case Kind::Number: color.alpha = Math::Clamp(value, 0.f, 1.f); return true;
		case Kind::Percentage: color.alpha = Math::Clamp(value * 0.01f, 0.f, 1.f); return true;
		case Kind::None:
			color.alpha = 0.f;
			color.alpha_missing = true;
			return true;
		case Kind::Invalid: break;
		}
		return false;
	}

	// Takes the three components and the alpha of the space syntax ('c1 c2 c3 [/ alpha]').
	bool SplitSpaceSyntax(const StringList& arguments, const String* components[3], const String*& alpha)
	{
		alpha = nullptr;
		if (arguments.size() == 3)
		{
		}
		else if (arguments.size() == 5 && arguments[3] == "/")
			alpha = &arguments[4];
		else
			return false;
		for (int i = 0; i < 3; i++)
		{
			if (arguments[i] == "/")
				return false;
			components[i] = &arguments[i];
		}
		return true;
	}

	bool ParseRGB(Color& color, const StringList& arguments, bool comma)
	{
		color.space = Space::SRGB;
		const String* components[3] = {};
		const String* alpha = nullptr;
		if (comma)
		{
			if (arguments.size() != 3 && arguments.size() != 4)
				return false;
			for (int i = 0; i < 3; i++)
				components[i] = &arguments[i];
			if (arguments.size() == 4)
				alpha = &arguments[3];
		}
		else if (!SplitSpaceSyntax(arguments, components, alpha))
			return false;

		Kind first_kind = Kind::Invalid;
		for (int i = 0; i < 3; i++)
		{
			float value = 0.f;
			const Kind kind = ParseComponent(*components[i], value, !comma);
			if (kind == Kind::Invalid)
				return false;
			// The comma syntax takes all numbers or all percentages.
			if (comma && i > 0 && kind != first_kind)
				return false;
			if (i == 0)
				first_kind = kind;
			color.missing[i] = (kind == Kind::None);
			color.components[i] = Math::Clamp(kind == Kind::Percentage ? value * 0.01f : value * (1.f / 255.f), 0.f, 1.f);
		}
		return !alpha || ParseAlpha(*alpha, color, !comma);
	}

	bool ParseHSL(Color& color, const StringList& arguments, bool comma, bool hwb)
	{
		color.space = (hwb ? Space::HWB : Space::HSL);
		const String* components[3] = {};
		const String* alpha = nullptr;
		if (comma)
		{
			if (hwb || (arguments.size() != 3 && arguments.size() != 4))
				return false;
			for (int i = 0; i < 3; i++)
				components[i] = &arguments[i];
			if (arguments.size() == 4)
				alpha = &arguments[3];
		}
		else if (!SplitSpaceSyntax(arguments, components, alpha))
			return false;

		if (!comma && EqualsNoCase(*components[0], "none"))
		{
			color.components[0] = 0.f;
			color.missing[0] = true;
		}
		else if (!ParseHue(*components[0], color.components[0]))
			return false;

		for (int i = 1; i < 3; i++)
		{
			float value = 0.f;
			const Kind kind = ParseComponent(*components[i], value, !comma);
			// The comma syntax takes percentages only, the space syntax numbers too (the same scale).
			if (kind == Kind::Invalid || (comma && kind != Kind::Percentage))
				return false;
			color.missing[i] = (kind == Kind::None);
			color.components[i] = Math::Clamp(value * 0.01f, 0.f, 1.f);
		}
		return !alpha || ParseAlpha(*alpha, color, !comma);
	}

	// lab(), lch(), oklab(), oklch(): 'lightness_scale' is the lightness of 100%, 'axis_scale' the a/b or chroma of 100%.
	bool ParseLabLike(Color& color, const StringList& arguments, Space space, float lightness_scale, float axis_scale)
	{
		color.space = space;
		const bool polar = (space == Space::LCH || space == Space::Oklch);
		const String* components[3] = {};
		const String* alpha = nullptr;
		if (!SplitSpaceSyntax(arguments, components, alpha))
			return false;

		for (int i = 0; i < 3; i++)
		{
			float value = 0.f;
			if (polar && i == 2)
			{
				if (EqualsNoCase(*components[i], "none"))
					color.missing[i] = true;
				else if (!ParseHue(*components[i], value))
					return false;
				color.components[i] = value;
				continue;
			}
			const Kind kind = ParseComponent(*components[i], value, true);
			if (kind == Kind::Invalid)
				return false;
			color.missing[i] = (kind == Kind::None);
			if (i == 0)
				color.components[i] = Math::Clamp(kind == Kind::Percentage ? value * 0.01f * lightness_scale : value, 0.f, lightness_scale);
			else
			{
				value = (kind == Kind::Percentage ? value * 0.01f * axis_scale : value);
				// Chroma is never negative.
				color.components[i] = (polar ? Math::Max(value, 0.f) : value);
			}
		}
		return !alpha || ParseAlpha(*alpha, color, true);
	}

	// color(<space> c1 c2 c3 [/ alpha]) in the spaces this parser converts (srgb, srgb-linear, xyz, xyz-d50, xyz-d65).
	bool ParsePredefined(Color& color, const StringList& arguments)
	{
		if (arguments.size() != 4 && arguments.size() != 6)
			return false;
		Space space = Space::SRGB;
		if (!ColorSpaces::ParseSpace(arguments[0], space) || ColorSpaces::IsPolar(space) || space == Space::Lab || space == Space::Oklab)
			return false;
		color.space = space;
		const StringList rest(arguments.begin() + 1, arguments.end());
		const String* components[3] = {};
		const String* alpha = nullptr;
		if (!SplitSpaceSyntax(rest, components, alpha))
			return false;
		for (int i = 0; i < 3; i++)
		{
			float value = 0.f;
			const Kind kind = ParseComponent(*components[i], value, true);
			if (kind == Kind::Invalid)
				return false;
			color.missing[i] = (kind == Kind::None);
			color.components[i] = (kind == Kind::Percentage ? value * 0.01f : value);
		}
		return !alpha || ParseAlpha(*alpha, color, true);
	}

} // namespace

void PropertyParserColour::Initialize()
{
	parser_data.Initialize();
}
void PropertyParserColour::Shutdown()
{
	parser_data.Shutdown();
}

PropertyParserColour::PropertyParserColour() {}

PropertyParserColour::~PropertyParserColour() {}

bool PropertyParserColour::IsCurrentColorKeyword(const Property& property)
{
	return property.value.GetType() == Variant::INT && property.value.Get<int>() == CurrentColor;
}

RMLUI_RTTI_Define(CurrentColorMixValue)

bool PropertyParserColour::IsCurrentColorMixValue(const Property& property)
{
	return property.value.GetType() == Variant::STYLEVALUEPTR &&
		rmlui_dynamic_cast<const CurrentColorMixValue*>(property.value.GetReference<StyleValuePtr>().get()) != nullptr;
}

// The text with each 'currentcolor' replaced by the colour (as #rrggbbaa).
static String SubstituteCurrentColor(const String& text, Colourb colour)
{
	static const char* digits = "0123456789abcdef";
	String hex = "#";
	for (int i = 0; i < 4; i++)
	{
		hex += digits[colour[i] >> 4];
		hex += digits[colour[i] & 15];
	}
	const String lower = StringUtilities::ToLower(text);
	String result;
	size_t from = 0;
	for (size_t at = lower.find("currentcolor"); at != String::npos; at = lower.find("currentcolor", from))
	{
		result += text.substr(from, at - from) + hex;
		from = at + 12;
	}
	result += text.substr(from);
	return result;
}

Colourb PropertyParserColour::ResolveCurrentColor(const String& mix, Colourb current_color)
{
	if (mix.empty())
		return current_color;
	Colourb colour = current_color;
	ParseColour(colour, SubstituteCurrentColor(mix, current_color));
	return colour;
}

Colourb PropertyParserColour::Resolve(const Property& property, Colourb current_color)
{
	if (IsCurrentColor(property))
		return current_color;
	if (IsCurrentColorMix(property))
		return ResolveCurrentColor(static_cast<const CurrentColorMixValue&>(*property.value.GetReference<StyleValuePtr>()).text, current_color);
	return property.value.Get<Colourb>();
}

bool PropertyParserColour::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	if (value.size() >= 12 && EqualsNoCase(Trim(value), "currentcolor"))
	{
		property.value = Variant(CurrentColor);
		property.unit = Unit::KEYWORD;
		return true;
	}

	// A color-mix() with a currentcolor operand: kept as written, resolved with the element's colour (it parses with any).
	const String lower = StringUtilities::ToLower(Trim(value));
	if (lower.compare(0, 10, "color-mix(") == 0 && lower.find("currentcolor") != String::npos)
	{
		Colourb probe;
		if (!ParseColour(probe, SubstituteCurrentColor(lower, Colourb(0, 0, 0, 255))))
			return false;
		property.value = Variant(StyleValuePtr(MakeShared<CurrentColorMixValue>(lower)));
		property.unit = Unit::STYLE_VALUE;
		return true;
	}

	Colourb colour;
	if (!ParseColour(colour, value))
		return false;

	property.value = Variant(colour);
	property.unit = Unit::COLOUR;

	return true;
}

bool PropertyParserColour::ParseColour(Colourb& colour, const String& value)
{
	if (value.empty())
		return false;

	if (value[0] == '#')
	{
		colour = {};
		return ParseHexColour(colour, value);
	}

	Color color;
	if (!ParseColor(color, value, 0))
		return false;
	colour = ColorSpaces::ToColourb(color);
	return true;
}

bool PropertyParserColour::ParseColor(Color& color, const String& in_value, int depth)
{
	if (depth > MaxColorDepth)
		return false;

	const String value = Trim(in_value);
	if (value.empty())
		return false;

	if (value[0] == '#')
	{
		Colourb colour;
		if (!ParseHexColour(colour, value))
			return false;
		color = ColorSpaces::FromColourb(colour);
		return true;
	}

	const size_t open = value.find('(');
	if (open != String::npos)
	{
		if (value.back() != ')')
			return false;
		const String name = StringUtilities::ToLower(value.substr(0, open));
		return ParseColorFunction(color, name, value.substr(open + 1, value.size() - open - 2), depth);
	}

	auto it = parser_data->html_colours.find(StringUtilities::ToLower(value));
	if (it == parser_data->html_colours.end())
		return false;
	color = ColorSpaces::FromColourb(it->second);
	return true;
}

bool PropertyParserColour::ParseHexColour(Colourb& colour, const String& value)
{
	char hex_values[4][2] = {{'f', 'f'}, {'f', 'f'}, {'f', 'f'}, {'f', 'f'}};

	switch (value.size())
	{
	// Single hex digit per channel, RGB and alpha.
	case 5:
		hex_values[3][0] = hex_values[3][1] = value[4];
		//-fallthrough
	// Single hex digit per channel, RGB only.
	case 4:
		hex_values[0][0] = hex_values[0][1] = value[1];
		hex_values[1][0] = hex_values[1][1] = value[2];
		hex_values[2][0] = hex_values[2][1] = value[3];
		break;

	// Two hex digits per channel, RGB and alpha.
	case 9:
		hex_values[3][0] = value[7];
		hex_values[3][1] = value[8];
		//-fallthrough
	// Two hex digits per channel, RGB only.
	case 7: memcpy(hex_values, &value.c_str()[1], sizeof(char) * 6); break;

	default: return false;
	}

	// Parse each of the colour elements.
	for (int i = 0; i < 4; i++)
	{
		int tens = Math::HexToDecimal(hex_values[i][0]);
		int ones = Math::HexToDecimal(hex_values[i][1]);
		if (tens == -1 || ones == -1)
			return false;

		colour[i] = (byte)(tens * 16 + ones);
	}

	return true;
}

bool PropertyParserColour::ParseColorFunction(Color& color, const String& name, const String& arguments, int depth)
{
	if (name == "color-mix")
		return ParseColorMix(color, arguments, depth);

	StringList values;
	bool comma = false;
	if (!SplitArguments(arguments, values, comma))
		return false;

	if (name == "rgb" || name == "rgba")
		return ParseRGB(color, values, comma);
	if (name == "hsl" || name == "hsla")
		return ParseHSL(color, values, comma, false);
	if (comma)
		return false;
	if (name == "hwb")
		return ParseHSL(color, values, comma, true);
	if (name == "lab")
		return ParseLabLike(color, values, Space::Lab, 100.f, 125.f);
	if (name == "lch")
		return ParseLabLike(color, values, Space::LCH, 100.f, 150.f);
	if (name == "oklab")
		return ParseLabLike(color, values, Space::Oklab, 1.f, 0.4f);
	if (name == "oklch")
		return ParseLabLike(color, values, Space::Oklch, 1.f, 0.4f);
	if (name == "color")
		return ParsePredefined(color, values);
	return false;
}

bool PropertyParserColour::ParseColorMix(Color& color, const String& arguments, int depth)
{
	// color-mix(in <space> [<hue-method> hue]?, <color> <percentage>?, <color> <percentage>?)
	StringList parts;
	bool comma = false;
	if (!SplitArguments(arguments, parts, comma) || !comma || parts.size() != 3)
		return false;

	StringList method;
	bool unused = false;
	if (!SplitArguments(parts[0], method, unused) || method.size() < 2 || !EqualsNoCase(method[0], "in"))
		return false;
	Space space = Space::SRGB;
	if (!ColorSpaces::ParseSpace(method[1], space))
		return false;
	ColorSpaces::HueInterpolation hue = ColorSpaces::HueInterpolation::Shorter;
	if (method.size() == 4)
	{
		if (!ColorSpaces::IsPolar(space) || !EqualsNoCase(method[3], "hue"))
			return false;
		if (EqualsNoCase(method[2], "shorter"))
			hue = ColorSpaces::HueInterpolation::Shorter;
		else if (EqualsNoCase(method[2], "longer"))
			hue = ColorSpaces::HueInterpolation::Longer;
		else if (EqualsNoCase(method[2], "increasing"))
			hue = ColorSpaces::HueInterpolation::Increasing;
		else if (EqualsNoCase(method[2], "decreasing"))
			hue = ColorSpaces::HueInterpolation::Decreasing;
		else
			return false;
	}
	else if (method.size() != 2)
		return false;

	Color colors[2];
	float percentages[2] = {};
	bool has_percentage[2] = {};
	for (int i = 0; i < 2; i++)
	{
		StringList tokens;
		if (!SplitArguments(parts[i + 1], tokens, unused) || tokens.empty() || tokens.size() > 2)
			return false;
		String color_text = tokens[0];
		if (tokens.size() == 2)
		{
			// The percentage before or after the color.
			float value = 0.f;
			if (ParsePercentage(tokens[1], value))
				color_text = tokens[0];
			else if (ParsePercentage(tokens[0], value))
				color_text = tokens[1];
			else
				return false;
			if (value < 0.f || value > 100.f)
				return false;
			percentages[i] = value;
			has_percentage[i] = true;
		}
		if (!ParseColor(colors[i], color_text, depth + 1))
			return false;
	}

	// CSS Color 5, 3.1: normalizing the percentages.
	if (!has_percentage[0] && !has_percentage[1])
		percentages[0] = percentages[1] = 50.f;
	else if (!has_percentage[0])
		percentages[0] = 100.f - percentages[1];
	else if (!has_percentage[1])
		percentages[1] = 100.f - percentages[0];
	const float sum = percentages[0] + percentages[1];
	if (sum <= 0.f)
		return false;
	const float alpha_multiplier = (sum < 100.f ? sum * 0.01f : 1.f);

	color = ColorSpaces::Mix(space, hue, colors[0], colors[1], percentages[0] / sum, alpha_multiplier);
	return true;
}

} // namespace Rml
