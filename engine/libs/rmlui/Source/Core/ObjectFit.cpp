#include "../../Include/RmlUi/Core/ObjectFit.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Math.h"
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "CalculationResolver.h"
#endif

namespace Rml {

bool ComputeObjectFitRect(Vector2f box_size, Vector2f intrinsic_size, Style::ObjectFit fit, Style::LengthPercentage position_x,
	Style::LengthPercentage position_y, ObjectFitRect& out)
{
	using Style::ObjectFit;
	if (box_size.x <= 0.f || box_size.y <= 0.f)
		return false;

	Vector2f size = box_size;
	if (fit != ObjectFit::Fill && intrinsic_size.x > 0.f && intrinsic_size.y > 0.f)
	{
		const float contain = Math::Min(box_size.x / intrinsic_size.x, box_size.y / intrinsic_size.y);
		const float cover = Math::Max(box_size.x / intrinsic_size.x, box_size.y / intrinsic_size.y);
		switch (fit)
		{
		case ObjectFit::Contain: size = intrinsic_size * contain; break;
		case ObjectFit::Cover: size = intrinsic_size * cover; break;
		case ObjectFit::None: size = intrinsic_size; break;
		case ObjectFit::ScaleDown: size = (contain < 1.f ? intrinsic_size * contain : intrinsic_size); break;
		case ObjectFit::Fill: break;
		}
	}

	// Percentages of the free space, which is negative when the object is larger than the box (ResolveValue refuses a
	// negative basis).
	auto resolve = [](const Style::LengthPercentage& value, float free_space) {
		switch (value.type)
		{
		case Style::LengthPercentage::Length: return value.value;
		case Style::LengthPercentage::Percentage: return value.value * 0.01f * free_space;
#ifdef RMLUI_MATH_EXPRESSIONS
		case Style::LengthPercentage::Calculation:
		{
			float result = 0.f;
			if (value.calculation && ResolveUsedCalculation(*value.calculation, free_space, result))
				return result;
			return 0.f;
		}
#endif
		}
		return 0.f;
	};
	const Vector2f position = {resolve(position_x, box_size.x - size.x), resolve(position_y, box_size.y - size.y)};

	// Cropped to the content box.
	const Vector2f top_left = Math::Max(position, Vector2f(0.f));
	const Vector2f bottom_right = Math::Min(position + size, box_size);
	if (bottom_right.x - top_left.x <= 1e-4f || bottom_right.y - top_left.y <= 1e-4f || size.x <= 0.f || size.y <= 0.f)
		return false;

	out.rect = Rectanglef::FromCorners(top_left, bottom_right);
	out.uv = Rectanglef::FromCorners((top_left - position) / size, (bottom_right - position) / size);
	out.size = size;
	return true;
}

} // namespace Rml

#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "PaintProperties.h"
#include "PropertyShorthandDefinition.h"

namespace Rml {

bool ParsePosition(const String& value, String& out_x, String& out_y)
{
	// Words separated by white space outside of parentheses (calc()).
	StringList tokens;
	{
		String word;
		int depth = 0;
		for (char c : value)
		{
			if (c == '(')
				depth++;
			else if (c == ')')
				depth--;
			if (depth == 0 && (c == ' ' || c == '\t' || c == '\n' || c == '\r'))
			{
				if (!word.empty())
					tokens.push_back(StringUtilities::ToLower(word));
				word.clear();
				continue;
			}
			word += c;
		}
		if (!word.empty())
			tokens.push_back(StringUtilities::ToLower(word));
	}
	if (tokens.empty() || tokens.size() > 4)
		return false;

	auto is_keyword = [](const String& t) { return t == "left" || t == "right" || t == "top" || t == "bottom" || t == "center"; };
	auto is_x = [](const String& t) { return t == "left" || t == "right"; };
	auto is_y = [](const String& t) { return t == "top" || t == "bottom"; };
	auto edge = [](const String& keyword) {
		if (keyword == "left" || keyword == "top")
			return String("0%");
		if (keyword == "center")
			return String("50%");
		return String("100%");
	};
	// An offset from an edge: from the left or top as it is, from the right or bottom as calc(100% - offset).
	auto offset_from = [](const String& keyword, const String& offset) {
		if (keyword == "left" || keyword == "top")
			return offset;
		return "calc(100% - " + offset + ")";
	};

	if (tokens.size() == 1)
	{
		const String& t = tokens[0];
		if (is_y(t))
		{
			out_x = "50%";
			out_y = edge(t);
		}
		else
		{
			out_x = is_keyword(t) ? edge(t) : t;
			out_y = "50%";
		}
		return true;
	}

	if (tokens.size() == 2)
	{
		String a = tokens[0], b = tokens[1];
		// A vertical keyword first or a horizontal one second: two keywords in the other order ('top left').
		if (is_y(a) || is_x(b))
		{
			if (!is_keyword(a) || !is_keyword(b) || is_y(b) || is_x(a))
				return false;
			std::swap(a, b);
		}
		if (is_y(a) || is_x(b))
			return false;
		out_x = is_keyword(a) ? edge(a) : a;
		out_y = is_keyword(b) ? edge(b) : b;
		return true;
	}

	// Three or four values: two keywords, each with an offset or not ('center' without one).
	struct Part {
		String keyword, offset;
	};
	Vector<Part> parts;
	for (size_t i = 0; i < tokens.size();)
	{
		if (!is_keyword(tokens[i]))
			return false;
		Part part{tokens[i], String()};
		if (i + 1 < tokens.size() && !is_keyword(tokens[i + 1]))
		{
			if (part.keyword == "center")
				return false;
			part.offset = tokens[i + 1];
		}
		i += (part.offset.empty() ? 1 : 2);
		parts.push_back(std::move(part));
	}
	if (parts.size() != 2)
		return false;
	// 'center' takes the axis the other keyword leaves.
	const bool first_is_x = is_x(parts[0].keyword) || (parts[0].keyword == "center" && is_y(parts[1].keyword));
	const bool second_is_x = is_x(parts[1].keyword) || (parts[1].keyword == "center" && !first_is_x);
	if (first_is_x == second_is_x)
		return false;
	String x, y;
	bool has_x = true, has_y = true;
	for (const Part& part : parts)
	{
		const String axis_value = part.offset.empty() ? edge(part.keyword) : offset_from(part.keyword, part.offset);
		(&part == &parts[0] ? first_is_x : second_is_x) ? (x = axis_value) : (y = axis_value);
	}
	out_x = has_x ? x : "50%";
	out_y = has_y ? y : "50%";
	return true;
}

bool ParsePositionShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	if (definition.items.size() != 2)
		return false;
	String x, y;
	if (!ParsePosition(value, x, y))
		return false;
	PropertyDictionary parsed;
	if (!specification.ParsePropertyDeclaration(parsed, definition.items[0].property_id, x) ||
		!specification.ParsePropertyDeclaration(parsed, definition.items[1].property_id, y))
		return false;
	for (const auto& id_property : parsed.GetProperties())
		dictionary.SetProperty(id_property.first, id_property.second);
	return true;
}

} // namespace Rml
