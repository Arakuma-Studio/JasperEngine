#include "GridStyleValues.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/StyleTypes.h"
#include "PropertyShorthandDefinition.h"
#include <algorithm>
#include <cmath>
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "Calculation.h"
#endif

namespace Rml {

// ---- Value types ----------------------------------------------------------------------------------------------------

static String NumberToString(float number)
{
	return ToString(number);
}

bool GridTrackBreadth::operator==(const GridTrackBreadth& other) const
{
	if (kind != other.kind)
		return false;
	switch (kind)
	{
	case Kind::Length: return length == other.length;
	case Kind::Fr: return fr == other.fr;
	default: return true;
	}
}

String GridTrackBreadth::ToString() const
{
	switch (kind)
	{
	case Kind::Length: return length.ToString();
	case Kind::Auto: return "auto";
	case Kind::MinContent: return "min-content";
	case Kind::MaxContent: return "max-content";
	case Kind::Fr: return NumberToString(fr) + "fr";
	}
	return String();
}

bool GridTrackBreadth::UsesPercentage() const
{
	if (kind != Kind::Length)
		return false;
#ifdef RMLUI_MATH_EXPRESSIONS
	if (length.calculation)
		return Any(length.calculation->GetDependencyMask() & Unit::PERCENT);
#endif
	return length.value.unit == Unit::PERCENT;
}

bool GridTrackSize::operator==(const GridTrackSize& other) const
{
	return form == other.form && min == other.min && max == other.max && (form != Form::FitContent || fit_content_limit == other.fit_content_limit);
}

String GridTrackSize::ToString() const
{
	switch (form)
	{
	case Form::Breadth: return max.ToString();
	case Form::MinMax: return "minmax(" + min.ToString() + ", " + max.ToString() + ")";
	case Form::FitContent: return "fit-content(" + fit_content_limit.ToString() + ")";
	}
	return String();
}

bool GridTrackSize::HasFixedComponent() const
{
	if (form == Form::FitContent)
		return false;
	return min.IsLength() || max.IsLength();
}

Units GridTrackSize::GetUnitDependencies() const
{
	Units units = Unit::UNKNOWN;
	if (min.IsLength())
		units = units | min.length.GetUnitDependencies();
	if (max.IsLength())
		units = units | max.length.GetUnitDependencies();
	if (form == Form::FitContent)
		units = units | fit_content_limit.GetUnitDependencies();
	return units;
}

static String LineNamesToString(const StringList& names)
{
	String result = "[";
	for (size_t i = 0; i < names.size(); i++)
		result += (i > 0 ? " " : "") + names[i];
	return result + "]";
}

// Appends "[names] " when the list is not empty.
static void AppendLineNames(String& out, const StringList& names)
{
	if (names.empty())
		return;
	if (!out.empty())
		out += ' ';
	out += LineNamesToString(names);
}

bool GridTrackRepeat::operator==(const GridTrackRepeat& other) const
{
	return count == other.count && (count != Count::Fixed || repetitions == other.repetitions) && tracks == other.tracks &&
		line_names == other.line_names;
}

String GridTrackRepeat::ToString() const
{
	String result;
	switch (count)
	{
	case Count::Fixed: result = Rml::ToString(repetitions); break;
	case Count::AutoFill: result = "auto-fill"; break;
	case Count::AutoFit: result = "auto-fit"; break;
	}
	result += ",";
	String list;
	for (size_t i = 0; i < tracks.size(); i++)
	{
		AppendLineNames(list, line_names[i]);
		list += (list.empty() ? "" : " ") + tracks[i].ToString();
	}
	AppendLineNames(list, line_names.back());
	return "repeat(" + result + " " + list + ")";
}

bool GridTemplateComponent::operator==(const GridTemplateComponent& other) const
{
	if (is_repeat != other.is_repeat)
		return false;
	return is_repeat ? repeat == other.repeat : track == other.track;
}

String GridTrackListValue::ToString() const
{
	if (none)
		return "none";
	String result;
	for (size_t i = 0; i < components.size(); i++)
	{
		AppendLineNames(result, line_names[i]);
		const GridTemplateComponent& component = components[i];
		result += (result.empty() ? "" : " ") + (component.is_repeat ? component.repeat.ToString() : component.track.ToString());
	}
	AppendLineNames(result, line_names.back());
	return result;
}

bool GridTrackListValue::Equals(const StyleValue& other_value) const
{
	const GridTrackListValue& other = static_cast<const GridTrackListValue&>(other_value);
	return none == other.none && components == other.components && line_names == other.line_names;
}

Units GridTrackListValue::GetUnitDependencies() const
{
	Units units = Unit::UNKNOWN;
	for (const GridTemplateComponent& component : components)
	{
		if (component.is_repeat)
		{
			for (const GridTrackSize& track : component.repeat.tracks)
				units = units | track.GetUnitDependencies();
		}
		else
			units = units | component.track.GetUnitDependencies();
	}
	return units;
}

const GridTrackRepeat* GridTrackListValue::GetAutoRepeat() const
{
	for (const GridTemplateComponent& component : components)
	{
		if (component.is_repeat && component.repeat.IsAutoRepeat())
			return &component.repeat;
	}
	return nullptr;
}

String GridAutoTracksValue::ToString() const
{
	String result;
	for (const GridTrackSize& track : tracks)
		result += (result.empty() ? "" : " ") + track.ToString();
	return result;
}

bool GridAutoTracksValue::Equals(const StyleValue& other) const
{
	return tracks == static_cast<const GridAutoTracksValue&>(other).tracks;
}

Units GridAutoTracksValue::GetUnitDependencies() const
{
	Units units = Unit::UNKNOWN;
	for (const GridTrackSize& track : tracks)
		units = units | track.GetUnitDependencies();
	return units;
}

bool GridTemplateArea::operator==(const GridTemplateArea& other) const
{
	return name == other.name && row_start == other.row_start && row_end == other.row_end && column_start == other.column_start &&
		column_end == other.column_end;
}

String GridTemplateAreasValue::ToString() const
{
	if (rows.empty())
		return "none";
	String result;
	for (const String& row : rows)
		result += (result.empty() ? "\"" : " \"") + row + "\"";
	return result;
}

bool GridTemplateAreasValue::Equals(const StyleValue& other) const
{
	return rows == static_cast<const GridTemplateAreasValue&>(other).rows;
}

String GridLineValue::ToString() const
{
	switch (kind)
	{
	case Kind::Auto: return "auto";
	case Kind::Line: return Rml::ToString(index);
	case Kind::NamedLine: return index == 0 ? name : Rml::ToString(index) + " " + name;
	case Kind::Span: return "span " + Rml::ToString(index);
	case Kind::NamedSpan: return index <= 1 ? "span " + name : "span " + Rml::ToString(index) + " " + name;
	}
	return String();
}

bool GridLineValue::Equals(const StyleValue& other_value) const
{
	const GridLineValue& other = static_cast<const GridLineValue&>(other_value);
	return kind == other.kind && index == other.index && name == other.name;
}

// ---- Tokens -----------------------------------------------------------------------------------------------------------

namespace {
	struct GridToken {
		enum class Type { Word, Names, String, Slash };
		Type type;
		String text;      // Word: the word; String: its content without the quotes
		StringList names; // Names: the line names in the brackets
	};
	using GridTokens = Vector<GridToken>;

	bool IsNameCodePoint(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (unsigned char)c >= 0x80;
	}

	// A CSS <custom-ident> usable as a line or area name: an identifier that is none of the CSS-wide keywords nor
	// the keywords of the grammar where names appear.
	bool IsCustomIdent(const String& word)
	{
		if (word.empty())
			return false;
		size_t i = 0;
		if (word[0] == '-')
		{
			i = 1;
			if (word.size() == 1)
				return false;
		}
		const char first = word[i];
		if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_' || first == '-' || (unsigned char)first >= 0x80))
			return false;
		for (char c : word)
		{
			if (!IsNameCodePoint(c))
				return false;
		}
		const String lower = StringUtilities::ToLower(word);
		static const char* const reserved[] = {"initial", "inherit", "unset", "revert", "revert-layer", "default", "auto", "span"};
		for (const char* keyword : reserved)
		{
			if (lower == keyword)
				return false;
		}
		return true;
	}

	// Splits a grid value into words (parentheses and quotes kept together), bracketed line-name lists, quoted strings
	// and '/' separators.
	bool Tokenize(const String& value, GridTokens& tokens)
	{
		tokens.clear();
		const size_t n = value.size();
		size_t i = 0;
		while (i < n)
		{
			const char c = value[i];
			if (StringUtilities::IsWhitespace(c))
			{
				i++;
				continue;
			}
			if (c == '[')
			{
				const size_t end = value.find(']', i + 1);
				if (end == String::npos)
					return false;
				GridToken token{GridToken::Type::Names, String(), StringList()};
				StringList words;
				SplitWhitespaceList(value.substr(i + 1, end - i - 1), words);
				for (const String& word : words)
				{
					if (!IsCustomIdent(word))
						return false;
					token.names.push_back(word);
				}
				tokens.push_back(std::move(token));
				i = end + 1;
				continue;
			}
			if (c == '"' || c == '\'')
			{
				const size_t end = value.find(c, i + 1);
				if (end == String::npos)
					return false;
				tokens.push_back(GridToken{GridToken::Type::String, value.substr(i + 1, end - i - 1), StringList()});
				i = end + 1;
				continue;
			}
			if (c == '/')
			{
				tokens.push_back(GridToken{GridToken::Type::Slash, "/", StringList()});
				i++;
				continue;
			}
			if (c == ']' || c == ')')
				return false;

			// A word, up to white space, a bracket, a quote or a slash outside parentheses.
			const size_t begin = i;
			int depth = 0;
			while (i < n)
			{
				const char d = value[i];
				if (d == '(')
					depth++;
				else if (d == ')')
				{
					if (depth == 0)
						return false;
					depth--;
				}
				else if (depth == 0 && (StringUtilities::IsWhitespace(d) || d == '[' || d == ']' || d == '"' || d == '\'' || d == '/'))
					break;
				i++;
			}
			if (depth != 0)
				return false;
			tokens.push_back(GridToken{GridToken::Type::Word, value.substr(begin, i - begin), StringList()});
		}
		return true;
	}

	// The text of a function call 'name(...)' (case-insensitive name): its arguments, or false.
	bool ReadFunction(const String& word, const char* name, String& arguments)
	{
		const size_t name_length = strlen(name);
		if (word.size() < name_length + 2 || word.back() != ')' || word[name_length] != '(')
			return false;
		if (StringUtilities::ToLower(word.substr(0, name_length)) != name)
			return false;
		arguments = word.substr(name_length + 1, word.size() - name_length - 2);
		return true;
	}

	// A non-negative <flex> ('1fr', '0.5fr').
	bool ParseFlex(const String& word, float& result)
	{
		if (word.size() < 3)
			return false;
		const String suffix = StringUtilities::ToLower(word.substr(word.size() - 2));
		if (suffix != "fr")
			return false;
		const String number = word.substr(0, word.size() - 2);
		char* end = nullptr;
		const double value = strtod(number.c_str(), &end);
		if (end != number.c_str() + number.size() || value < 0.0 || !std::isfinite(value))
			return false;
		result = float(value);
		return true;
	}

	// A non-negative <length-percentage> (or a calc() of one).
	bool ParseNonNegativeLengthPercentage(const String& word, LengthPercentageValue& result)
	{
		if (!ParseLengthPercentage(word, result))
			return false;
#ifdef RMLUI_MATH_EXPRESSIONS
		if (result.calculation)
			return true;
#endif
		return result.value.number >= 0.f;
	}

	bool ParseBreadth(const String& word, GridTrackBreadth& result, bool allow_flex)
	{
		const String lower = StringUtilities::ToLower(word);
		result = GridTrackBreadth();
		if (lower == "auto")
			result.kind = GridTrackBreadth::Kind::Auto;
		else if (lower == "min-content")
			result.kind = GridTrackBreadth::Kind::MinContent;
		else if (lower == "max-content")
			result.kind = GridTrackBreadth::Kind::MaxContent;
		else if (ParseFlex(word, result.fr))
		{
			if (!allow_flex)
				return false;
			result.kind = GridTrackBreadth::Kind::Fr;
		}
		else if (ParseNonNegativeLengthPercentage(word, result.length))
			result.kind = GridTrackBreadth::Kind::Length;
		else
			return false;
		return true;
	}

	bool ParseTrackSize(const String& word, GridTrackSize& result)
	{
		result = GridTrackSize();
		String arguments;
		if (ReadFunction(word, "minmax", arguments))
		{
			StringList parts;
			SplitCommaList(arguments, parts);
			if (parts.size() != 2)
				return false;
			// The minimum is an <inflexible-breadth>: no 'fr'.
			if (!ParseBreadth(parts[0], result.min, false) || !ParseBreadth(parts[1], result.max, true))
				return false;
			result.form = GridTrackSize::Form::MinMax;
			return true;
		}
		if (ReadFunction(word, "fit-content", arguments))
		{
			if (!ParseNonNegativeLengthPercentage(StringUtilities::StripWhitespace(arguments), result.fit_content_limit))
				return false;
			result.form = GridTrackSize::Form::FitContent;
			result.min.kind = GridTrackBreadth::Kind::Auto;
			result.max.kind = GridTrackBreadth::Kind::MaxContent;
			return true;
		}
		GridTrackBreadth breadth;
		if (!ParseBreadth(word, breadth, true))
			return false;
		result.form = GridTrackSize::Form::Breadth;
		result.min = breadth;
		result.max = breadth;
		return true;
	}

	// Reads '[names]? size [names]? size ... [names]?' from tokens: the line names between the track sizes (repeat() and the
	// explicit track list of 'grid-template'). Two name lists in a row are invalid.
	bool ParseNamedTrackSizes(const GridTokens& tokens, Vector<GridTrackSize>& tracks, Vector<StringList>& line_names)
	{
		tracks.clear();
		line_names.assign(1, StringList());
		for (const GridToken& token : tokens)
		{
			if (token.type == GridToken::Type::Names)
			{
				if (!line_names.back().empty())
					return false;
				line_names.back() = token.names;
			}
			else if (token.type == GridToken::Type::Word)
			{
				GridTrackSize track;
				if (!ParseTrackSize(token.text, track))
					return false;
				tracks.push_back(track);
				line_names.push_back(StringList());
			}
			else
				return false;
		}
		return !tracks.empty();
	}

	bool ParseRepeat(const String& arguments, GridTrackRepeat& result)
	{
		// 'repeat(<count>, <tracks>)': the count before the first top-level comma.
		StringList parts;
		SplitCommaList(arguments, parts);
		if (parts.size() != 2)
			return false;
		const String count = StringUtilities::ToLower(parts[0]);
		if (count == "auto-fill")
			result.count = GridTrackRepeat::Count::AutoFill;
		else if (count == "auto-fit")
			result.count = GridTrackRepeat::Count::AutoFit;
		else
		{
			char* end = nullptr;
			const long repetitions = strtol(count.c_str(), &end, 10);
			if (count.empty() || end != count.c_str() + count.size() || repetitions < 1)
				return false;
			result.count = GridTrackRepeat::Count::Fixed;
			result.repetitions = int(std::min(repetitions, 10000L));
		}

		GridTokens tokens;
		if (!Tokenize(parts[1], tokens))
			return false;
		for (const GridToken& token : tokens)
		{
			// No nested repeat().
			String nested;
			if (token.type == GridToken::Type::Word && ReadFunction(token.text, "repeat", nested))
				return false;
		}
		if (!ParseNamedTrackSizes(tokens, result.tracks, result.line_names))
			return false;
		// Auto-repeated tracks have a definite size (a <fixed-size>).
		if (result.IsAutoRepeat())
		{
			for (const GridTrackSize& track : result.tracks)
			{
				if (!track.HasFixedComponent())
					return false;
			}
		}
		return true;
	}
} // namespace

bool ParseGridTrackList(const String& value, GridTrackListValue& result, bool allow_none, bool allow_auto_repeat)
{
	result = GridTrackListValue();
	GridTokens tokens;
	if (!Tokenize(value, tokens) || tokens.empty())
		return false;

	if (tokens.size() == 1 && tokens[0].type == GridToken::Type::Word && StringUtilities::ToLower(tokens[0].text) == "none")
	{
		if (!allow_none)
			return false;
		result.none = true;
		return true;
	}

	result.none = false;
	result.line_names.assign(1, StringList());
	int num_auto_repeats = 0;
	for (const GridToken& token : tokens)
	{
		if (token.type == GridToken::Type::Names)
		{
			if (!result.line_names.back().empty())
				return false;
			result.line_names.back() = token.names;
			continue;
		}
		if (token.type != GridToken::Type::Word)
			return false;

		GridTemplateComponent component;
		String arguments;
		if (ReadFunction(token.text, "repeat", arguments))
		{
			component.is_repeat = true;
			if (!ParseRepeat(arguments, component.repeat))
				return false;
			if (component.repeat.IsAutoRepeat())
			{
				if (!allow_auto_repeat)
					return false;
				num_auto_repeats++;
			}
		}
		else if (!ParseTrackSize(token.text, component.track))
			return false;

		result.components.push_back(std::move(component));
		result.line_names.push_back(StringList());
	}

	if (result.components.empty())
		return false;

	// An <auto-track-list>: one auto repeat, every other track a <fixed-size> (CSS Grid 2, 7.2).
	if (num_auto_repeats > 1)
		return false;
	if (num_auto_repeats == 1)
	{
		for (const GridTemplateComponent& component : result.components)
		{
			if (component.is_repeat)
			{
				for (const GridTrackSize& track : component.repeat.tracks)
				{
					if (!track.HasFixedComponent())
						return false;
				}
			}
			else if (!component.track.HasFixedComponent())
				return false;
		}
	}
	return true;
}

bool ParseGridAutoTracks(const String& value, GridAutoTracksValue& result)
{
	result.tracks.clear();
	GridTokens tokens;
	if (!Tokenize(value, tokens) || tokens.empty())
		return false;
	for (const GridToken& token : tokens)
	{
		GridTrackSize track;
		if (token.type != GridToken::Type::Word || !ParseTrackSize(token.text, track))
			return false;
		result.tracks.push_back(track);
	}
	return true;
}

bool ParseGridTemplateAreas(const StringList& strings, GridTemplateAreasValue& result)
{
	result = GridTemplateAreasValue();
	if (strings.empty())
		return false;

	// Each string: named cell tokens (name code points) and null cell tokens (runs of '.'), separated by white space.
	Vector<StringList> cells;
	for (const String& row_string : strings)
	{
		StringList row;
		size_t i = 0;
		const size_t n = row_string.size();
		while (i < n)
		{
			const char c = row_string[i];
			if (StringUtilities::IsWhitespace(c))
			{
				i++;
				continue;
			}
			const size_t begin = i;
			if (c == '.')
			{
				while (i < n && row_string[i] == '.')
					i++;
				row.push_back(".");
			}
			else if (IsNameCodePoint(c))
			{
				while (i < n && IsNameCodePoint(row_string[i]))
					i++;
				row.push_back(row_string.substr(begin, i - begin));
			}
			else
				return false; // a trash token
		}
		if (row.empty())
			return false;
		if (!cells.empty() && row.size() != cells[0].size())
			return false;
		cells.push_back(std::move(row));
	}

	result.num_rows = int(cells.size());
	result.num_columns = int(cells[0].size());
	for (const StringList& row : cells)
	{
		String text;
		for (const String& cell : row)
			text += (text.empty() ? "" : " ") + cell;
		result.rows.push_back(std::move(text));
	}

	// Each name forms one filled rectangle.
	for (int r = 0; r < result.num_rows; r++)
	{
		for (int c = 0; c < result.num_columns; c++)
		{
			const String& name = cells[r][c];
			if (name == ".")
				continue;
			const bool known = std::any_of(result.areas.begin(), result.areas.end(), [&](const GridTemplateArea& area) { return area.name == name; });
			if (known)
				continue;

			int column_end = c + 1;
			while (column_end < result.num_columns && cells[r][column_end] == name)
				column_end++;
			int row_end = r + 1;
			while (row_end < result.num_rows && cells[row_end][c] == name)
				row_end++;

			// Every cell of the rectangle has the name, and no cell outside it.
			for (int rr = 0; rr < result.num_rows; rr++)
			{
				for (int cc = 0; cc < result.num_columns; cc++)
				{
					const bool inside = (rr >= r && rr < row_end && cc >= c && cc < column_end);
					if ((cells[rr][cc] == name) != inside)
						return false;
				}
			}
			result.areas.push_back(GridTemplateArea{name, r + 1, row_end + 1, c + 1, column_end + 1});
		}
	}
	return true;
}

bool ParseGridLine(const String& value, GridLineValue& result)
{
	result = GridLineValue();
	StringList words;
	SplitWhitespaceList(value, words);
	if (words.empty() || words.size() > 3)
		return false;

	if (words.size() == 1 && StringUtilities::ToLower(words[0]) == "auto")
	{
		result.kind = GridLineValue::Kind::Auto;
		return true;
	}

	bool span = false;
	bool has_number = false;
	int number = 0;
	String name;
	for (const String& word : words)
	{
		const String lower = StringUtilities::ToLower(word);
		if (lower == "span")
		{
			// 'span' comes first or last ('span 2 a', '2 a span').
			if (span)
				return false;
			span = true;
			continue;
		}
		char* end = nullptr;
		const long parsed = strtol(word.c_str(), &end, 10);
		if (!word.empty() && end == word.c_str() + word.size())
		{
			if (has_number || parsed == 0)
				return false;
			has_number = true;
			number = int(Math::Clamp(parsed, -10000L, 10000L));
			continue;
		}
		if (!name.empty() || !IsCustomIdent(word))
			return false;
		name = word;
	}
	// 'span' may not sit between the number and the name.
	if (span && words.size() == 3 && StringUtilities::ToLower(words[1]) == "span")
		return false;

	if (span)
	{
		if (has_number && number < 0)
			return false;
		result.kind = name.empty() ? GridLineValue::Kind::Span : GridLineValue::Kind::NamedSpan;
		result.index = has_number ? number : 1;
		result.name = name;
		return has_number || !name.empty();
	}
	if (!name.empty())
	{
		result.kind = GridLineValue::Kind::NamedLine;
		result.index = has_number ? number : 0;
		result.name = name;
		return true;
	}
	if (!has_number)
		return false;
	result.kind = GridLineValue::Kind::Line;
	result.index = number;
	return true;
}

// ---- Property parsers ---------------------------------------------------------------------------------------------------

namespace {
	void SetStyleValue(Property& property, StyleValuePtr value)
	{
		property.value = Variant(std::move(value));
		property.unit = Unit::STYLE_VALUE;
	}

	class PropertyParserGridTrackList final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			auto result = MakeShared<GridTrackListValue>();
			if (!ParseGridTrackList(value, *result))
				return false;
			SetStyleValue(property, std::move(result));
			return true;
		}
	};

	class PropertyParserGridAutoTracks final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			auto result = MakeShared<GridAutoTracksValue>();
			if (!ParseGridAutoTracks(value, *result))
				return false;
			SetStyleValue(property, std::move(result));
			return true;
		}
	};

	class PropertyParserGridTemplateAreas final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			auto result = MakeShared<GridTemplateAreasValue>();
			GridTokens tokens;
			if (!Tokenize(value, tokens) || tokens.empty())
				return false;
			if (tokens.size() == 1 && tokens[0].type == GridToken::Type::Word && StringUtilities::ToLower(tokens[0].text) == "none")
			{
				SetStyleValue(property, std::move(result));
				return true;
			}
			StringList strings;
			for (const GridToken& token : tokens)
			{
				if (token.type != GridToken::Type::String)
					return false;
				strings.push_back(token.text);
			}
			if (!ParseGridTemplateAreas(strings, *result))
				return false;
			SetStyleValue(property, std::move(result));
			return true;
		}
	};

	class PropertyParserGridLine final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			auto result = MakeShared<GridLineValue>();
			if (!ParseGridLine(value, *result))
				return false;
			SetStyleValue(property, std::move(result));
			return true;
		}
	};

	// 'grid-auto-flow': [ row | column ] || dense. The keyword values are those of Style::GridAutoFlow, named by the
	// parameters ("row=0, column=1, row dense=2, column dense=3").
	class PropertyParserGridAutoFlow final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			StringList words;
			SplitWhitespaceList(StringUtilities::ToLower(value), words);
			if (words.empty() || words.size() > 2)
				return false;
			bool dense = false, column = false, has_direction = false;
			for (const String& word : words)
			{
				if (word == "dense" && !dense)
					dense = true;
				else if ((word == "row" || word == "column") && !has_direction)
				{
					has_direction = true;
					column = (word == "column");
				}
				else
					return false;
			}
			property.value = Variant(int(column ? (dense ? Style::GridAutoFlow::ColumnDense : Style::GridAutoFlow::Column)
													: (dense ? Style::GridAutoFlow::RowDense : Style::GridAutoFlow::Row)));
			property.unit = Unit::KEYWORD;
			return true;
		}
	};

	// 'row-gap', 'column-gap': 'normal' (0 in flex and grid layout, CSS Box Alignment 3, 8.1) or a non-negative length-percentage.
	class PropertyParserGap final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override
		{
			if (StringUtilities::ToLower(StringUtilities::StripWhitespace(value)) == "normal")
			{
				property.value = Variant(0.f);
				property.unit = Unit::PX;
				return true;
			}
			PropertyParser* length_percent = StyleSheetSpecification::GetParser("length_percent");
			if (!length_percent || !length_percent->ParseValue(property, value, parameters))
				return false;
			if (Any(property.unit & Unit::NUMERIC) && property.Get<float>() < 0.f)
				return false;
			return true;
		}
	};

	struct GridParsers {
		PropertyParserGridTrackList track_list;
		PropertyParserGridAutoTracks auto_tracks;
		PropertyParserGridTemplateAreas template_areas;
		PropertyParserGridLine line;
		PropertyParserGridAutoFlow auto_flow;
		PropertyParserGap gap;
	};
	GridParsers grid_parsers;

	// ---- Shorthands ----

	// Splits tokens at the top-level slashes.
	Vector<GridTokens> SplitAtSlashes(const GridTokens& tokens)
	{
		Vector<GridTokens> parts(1);
		for (const GridToken& token : tokens)
		{
			if (token.type == GridToken::Type::Slash)
				parts.emplace_back();
			else
				parts.back().push_back(token);
		}
		return parts;
	}

	String TokensToString(const GridTokens& tokens)
	{
		String result;
		for (const GridToken& token : tokens)
		{
			if (!result.empty())
				result += ' ';
			switch (token.type)
			{
			case GridToken::Type::Word: result += token.text; break;
			case GridToken::Type::Names: result += LineNamesToString(token.names); break;
			case GridToken::Type::String: result += '"' + token.text + '"'; break;
			case GridToken::Type::Slash: result += '/'; break;
			}
		}
		return result;
	}

	// Parses each longhand from its value into a scratch dictionary that starts with every longhand of the shorthand at
	// its initial value, and copies the result when all parse.
	class ShorthandWriter {
	public:
		ShorthandWriter(const ShorthandDefinition& definition, const PropertySpecification& specification) : specification(specification)
		{
			specification.SetShorthandDefaults(parsed, definition);
		}
		bool Set(PropertyId id, const String& value) { return specification.ParsePropertyDeclaration(parsed, id, value); }
		bool Commit(PropertyDictionary& dictionary)
		{
			for (const auto& id_property : parsed.GetProperties())
				dictionary.SetProperty(id_property.first, id_property.second);
			return true;
		}

	private:
		const PropertySpecification& specification;
		PropertyDictionary parsed;
	};

	PropertyId ItemProperty(const ShorthandDefinition& definition, size_t index)
	{
		if (index >= definition.items.size() || definition.items[index].type != ShorthandItemType::Property)
			return PropertyId::Invalid;
		return definition.items[index].property_id;
	}

	bool IsLoneCustomIdent(const String& value)
	{
		GridLineValue line;
		return ParseGridLine(value, line) && line.IsCustomIdent();
	}

	// 'grid-row', 'grid-column': <grid-line> [ / <grid-line> ]?. An omitted end is the start when the start is a lone
	// <custom-ident>, else 'auto'.
	bool ParseGridLineShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
		const PropertySpecification& specification)
	{
		const PropertyId start_id = ItemProperty(definition, 0), end_id = ItemProperty(definition, 1);
		if (start_id == PropertyId::Invalid || end_id == PropertyId::Invalid)
			return false;
		GridTokens tokens;
		if (!Tokenize(value, tokens))
			return false;
		const Vector<GridTokens> parts = SplitAtSlashes(tokens);
		if (parts.size() > 2 || parts[0].empty() || (parts.size() == 2 && parts[1].empty()))
			return false;
		const String start = TokensToString(parts[0]);
		const String end = parts.size() == 2 ? TokensToString(parts[1]) : (IsLoneCustomIdent(start) ? start : String("auto"));
		ShorthandWriter writer(definition, specification);
		return writer.Set(start_id, start) && writer.Set(end_id, end) && writer.Commit(dictionary);
	}

	// 'grid-area': <grid-line> [ / <grid-line> ]{0,3}: row-start / column-start / row-end / column-end, omitted lines copied
	// from the opposite start line when it is a lone <custom-ident>, else 'auto'.
	bool ParseGridAreaShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
		const PropertySpecification& specification)
	{
		PropertyId ids[4];
		for (size_t i = 0; i < 4; i++)
		{
			ids[i] = ItemProperty(definition, i);
			if (ids[i] == PropertyId::Invalid)
				return false;
		}
		GridTokens tokens;
		if (!Tokenize(value, tokens))
			return false;
		const Vector<GridTokens> parts = SplitAtSlashes(tokens);
		if (parts.size() > 4)
			return false;
		String lines[4];
		for (size_t i = 0; i < parts.size(); i++)
		{
			if (parts[i].empty())
				return false;
			lines[i] = TokensToString(parts[i]);
		}
		auto Copy = [](const String& from) { return IsLoneCustomIdent(from) ? from : String("auto"); };
		if (parts.size() < 2)
			lines[1] = Copy(lines[0]);
		if (parts.size() < 3)
			lines[2] = Copy(lines[0]);
		if (parts.size() < 4)
			lines[3] = Copy(lines[1]);
		ShorthandWriter writer(definition, specification);
		for (size_t i = 0; i < 4; i++)
		{
			if (!writer.Set(ids[i], lines[i]))
				return false;
		}
		return writer.Commit(dictionary);
	}

	// The 'grid-template' value into its rows, columns and areas. False if invalid.
	bool SplitGridTemplate(const GridTokens& tokens, String& rows, String& columns, String& areas)
	{
		if (tokens.empty())
			return false;
		if (tokens.size() == 1 && tokens[0].type == GridToken::Type::Word && StringUtilities::ToLower(tokens[0].text) == "none")
		{
			rows = columns = areas = "none";
			return true;
		}

		const Vector<GridTokens> parts = SplitAtSlashes(tokens);
		const bool has_strings = std::any_of(tokens.begin(), tokens.end(), [](const GridToken& token) { return token.type == GridToken::Type::String; });
		if (!has_strings)
		{
			// <'grid-template-rows'> / <'grid-template-columns'>
			if (parts.size() != 2 || parts[0].empty() || parts[1].empty())
				return false;
			rows = TokensToString(parts[0]);
			columns = TokensToString(parts[1]);
			areas = "none";
			return true;
		}

		// [ <line-names>? <string> <track-size>? <line-names>? ]+ [ / <explicit-track-list> ]?
		if (parts.size() > 2 || (parts.size() == 2 && parts[1].empty()))
			return false;
		const GridTokens& left = parts[0];
		String rows_text, areas_text;
		StringList pending_names; // names before the next line: the end names of a row and the start names of the next merge
		bool row_open = false;     // a string was read, its track size may follow
		bool row_has_size = false;
		auto CloseRow = [&]() {
			if (row_open && !row_has_size)
				rows_text += " auto";
			row_open = false;
			row_has_size = false;
		};
		for (size_t i = 0; i < left.size(); i++)
		{
			const GridToken& token = left[i];
			switch (token.type)
			{
			case GridToken::Type::Names:
				if (row_open)
				{
					// The end names of the row.
					CloseRow();
					pending_names.insert(pending_names.end(), token.names.begin(), token.names.end());
				}
				else
				{
					// Start names: only before a string, and not twice in a row (the end names of the previous row merge).
					if (i + 1 >= left.size() || left[i + 1].type != GridToken::Type::String)
						return false;
					pending_names.insert(pending_names.end(), token.names.begin(), token.names.end());
				}
				break;
			case GridToken::Type::String:
				CloseRow();
				if (!pending_names.empty())
				{
					rows_text += (rows_text.empty() ? "" : " ") + LineNamesToString(pending_names);
					pending_names.clear();
				}
				areas_text += (areas_text.empty() ? "\"" : " \"") + token.text + "\"";
				row_open = true;
				break;
			case GridToken::Type::Word:
			{
				if (!row_open || row_has_size)
					return false;
				GridTrackSize size;
				if (!ParseTrackSize(token.text, size))
					return false;
				rows_text += (rows_text.empty() ? "" : " ") + token.text;
				row_has_size = true;
				break;
			}
			case GridToken::Type::Slash: return false;
			}
		}
		CloseRow();
		if (rows_text.empty())
			rows_text = "auto";
		if (!pending_names.empty())
			rows_text += " " + LineNamesToString(pending_names);
		// The row list starts with ' auto' when the first row had no size.
		rows = StringUtilities::StripWhitespace(rows_text);
		areas = areas_text;

		if (parts.size() == 2)
		{
			// An <explicit-track-list>: line names and track sizes, no repeat().
			for (const GridToken& token : parts[1])
			{
				String arguments;
				if (token.type == GridToken::Type::String || (token.type == GridToken::Type::Word && ReadFunction(token.text, "repeat", arguments)))
					return false;
			}
			columns = TokensToString(parts[1]);
		}
		else
			columns = "none";
		return true;
	}

	bool ParseGridTemplateShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
		const PropertySpecification& specification)
	{
		GridTokens tokens;
		String rows, columns, areas;
		if (!Tokenize(value, tokens) || !SplitGridTemplate(tokens, rows, columns, areas))
			return false;
		ShorthandWriter writer(definition, specification);
		return writer.Set(PropertyId::GridTemplateRows, rows) && writer.Set(PropertyId::GridTemplateColumns, columns) &&
			writer.Set(PropertyId::GridTemplateAreas, areas) && writer.Commit(dictionary);
	}

	// Reads '[ auto-flow && dense? ] <tracks>?' from one side of the 'grid' shorthand.
	bool ReadAutoFlowSide(const GridTokens& tokens, bool& dense, String& tracks)
	{
		bool auto_flow = false;
		dense = false;
		GridTokens rest;
		size_t i = 0;
		for (; i < tokens.size() && i < 2; i++)
		{
			const GridToken& token = tokens[i];
			if (token.type != GridToken::Type::Word)
				break;
			const String lower = StringUtilities::ToLower(token.text);
			if (lower == "auto-flow" && !auto_flow)
				auto_flow = true;
			else if (lower == "dense" && !dense)
				dense = true;
			else
				break;
		}
		if (!auto_flow)
			return false;
		rest.assign(tokens.begin() + i, tokens.end());
		tracks = rest.empty() ? String("auto") : TokensToString(rest);
		return true;
	}

	bool ContainsAutoFlow(const GridTokens& tokens)
	{
		return std::any_of(tokens.begin(), tokens.end(),
			[](const GridToken& token) { return token.type == GridToken::Type::Word && StringUtilities::ToLower(token.text) == "auto-flow"; });
	}

	// 'grid': <'grid-template'> | <'grid-template-rows'> / [ auto-flow && dense? ] <'grid-auto-columns'>? |
	// [ auto-flow && dense? ] <'grid-auto-rows'>? / <'grid-template-columns'>. Every other longhand is reset.
	bool ParseGridShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
		const PropertySpecification& specification)
	{
		GridTokens tokens;
		if (!Tokenize(value, tokens) || tokens.empty())
			return false;
		ShorthandWriter writer(definition, specification);
		const Vector<GridTokens> parts = SplitAtSlashes(tokens);
		if (parts.size() == 2 && (ContainsAutoFlow(parts[0]) || ContainsAutoFlow(parts[1])))
		{
			if (parts[0].empty() || parts[1].empty())
				return false;
			bool dense = false;
			String auto_tracks;
			if (ReadAutoFlowSide(parts[1], dense, auto_tracks))
			{
				if (ContainsAutoFlow(parts[0]))
					return false;
				return writer.Set(PropertyId::GridTemplateRows, TokensToString(parts[0])) && writer.Set(PropertyId::GridTemplateColumns, "none") &&
					writer.Set(PropertyId::GridTemplateAreas, "none") && writer.Set(PropertyId::GridAutoColumns, auto_tracks) &&
					writer.Set(PropertyId::GridAutoFlow, dense ? "column dense" : "column") && writer.Commit(dictionary);
			}
			if (ReadAutoFlowSide(parts[0], dense, auto_tracks))
			{
				if (ContainsAutoFlow(parts[1]))
					return false;
				return writer.Set(PropertyId::GridTemplateRows, "none") && writer.Set(PropertyId::GridTemplateColumns, TokensToString(parts[1])) &&
					writer.Set(PropertyId::GridTemplateAreas, "none") && writer.Set(PropertyId::GridAutoRows, auto_tracks) &&
					writer.Set(PropertyId::GridAutoFlow, dense ? "row dense" : "row") && writer.Commit(dictionary);
			}
			return false;
		}

		String rows, columns, areas;
		if (!SplitGridTemplate(tokens, rows, columns, areas))
			return false;
		return writer.Set(PropertyId::GridTemplateRows, rows) && writer.Set(PropertyId::GridTemplateColumns, columns) &&
			writer.Set(PropertyId::GridTemplateAreas, areas) && writer.Commit(dictionary);
	}
} // namespace

void RegisterGridParsers()
{
	StyleSheetSpecification::RegisterParser("grid_track_list", &grid_parsers.track_list);
	StyleSheetSpecification::RegisterParser("grid_auto_tracks", &grid_parsers.auto_tracks);
	StyleSheetSpecification::RegisterParser("grid_template_areas", &grid_parsers.template_areas);
	StyleSheetSpecification::RegisterParser("grid_line", &grid_parsers.line);
	StyleSheetSpecification::RegisterParser("grid_auto_flow", &grid_parsers.auto_flow);
	StyleSheetSpecification::RegisterParser("gap", &grid_parsers.gap);
}

void RegisterGridProperties(PropertySpecification& specification)
{
	// clang-format off
	specification.RegisterProperty("grid-template-columns", "none", false, true, PropertyId::GridTemplateColumns).AddParser("grid_track_list");
	specification.RegisterProperty("grid-template-rows", "none", false, true, PropertyId::GridTemplateRows).AddParser("grid_track_list");
	specification.RegisterProperty("grid-template-areas", "none", false, true, PropertyId::GridTemplateAreas).SetValueAsWritten().AddParser("grid_template_areas");
	specification.RegisterProperty("grid-auto-columns", "auto", false, true, PropertyId::GridAutoColumns).AddParser("grid_auto_tracks");
	specification.RegisterProperty("grid-auto-rows", "auto", false, true, PropertyId::GridAutoRows).AddParser("grid_auto_tracks");
	specification.RegisterProperty("grid-auto-flow", "row", false, true, PropertyId::GridAutoFlow).AddParser("grid_auto_flow", "row=0, column=1, row dense=2, column dense=3");
	specification.RegisterProperty("grid-row-start", "auto", false, true, PropertyId::GridRowStart).AddParser("grid_line");
	specification.RegisterProperty("grid-row-end", "auto", false, true, PropertyId::GridRowEnd).AddParser("grid_line");
	specification.RegisterProperty("grid-column-start", "auto", false, true, PropertyId::GridColumnStart).AddParser("grid_line");
	specification.RegisterProperty("grid-column-end", "auto", false, true, PropertyId::GridColumnEnd).AddParser("grid_line");

	specification.RegisterShorthand("grid-row", "grid-row-start, grid-row-end", ShorthandType::Custom, ShorthandId::GridRow);
	specification.RegisterShorthand("grid-column", "grid-column-start, grid-column-end", ShorthandType::Custom, ShorthandId::GridColumn);
	specification.RegisterShorthand("grid-area", "grid-row-start, grid-column-start, grid-row-end, grid-column-end", ShorthandType::Custom, ShorthandId::GridArea);
	specification.RegisterShorthand("grid-template", "grid-template-rows, grid-template-columns, grid-template-areas", ShorthandType::Custom, ShorthandId::GridTemplate);
	specification.RegisterShorthand("grid", "grid-template-rows, grid-template-columns, grid-template-areas, grid-auto-rows, grid-auto-columns, grid-auto-flow",
		ShorthandType::Custom, ShorthandId::Grid);
	// The legacy names of the gaps (CSS Box Alignment 3, 8.3).
	specification.RegisterShorthand("grid-gap", "row-gap, column-gap", ShorthandType::Replicate, ShorthandId::GridGap);
	specification.RegisterShorthand("grid-row-gap", "row-gap", ShorthandType::FallThrough, ShorthandId::GridRowGap);
	specification.RegisterShorthand("grid-column-gap", "column-gap", ShorthandType::FallThrough, ShorthandId::GridColumnGap);
	// clang-format on

	specification.SetShorthandParser(ShorthandId::GridRow, &ParseGridLineShorthand);
	specification.SetShorthandParser(ShorthandId::GridColumn, &ParseGridLineShorthand);
	specification.SetShorthandParser(ShorthandId::GridArea, &ParseGridAreaShorthand);
	specification.SetShorthandParser(ShorthandId::GridTemplate, &ParseGridTemplateShorthand);
	specification.SetShorthandParser(ShorthandId::Grid, &ParseGridShorthand);
}

} // namespace Rml
