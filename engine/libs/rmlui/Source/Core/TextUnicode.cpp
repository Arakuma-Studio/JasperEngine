#include "TextUnicode.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <algorithm>
#include <cstdint>

namespace Rml {
namespace TextUnicode {

	namespace {
		struct CaseMapping {
			char32_t code;
			char32_t mapping;
		};
		struct CaseSequence {
			char32_t code;
			uint16_t first;
			uint8_t length;
		};
		struct CodepointRange {
			char32_t first;
			char32_t last;
		};

		// The UAX #14 classes the line breaker tells apart; every other class is read as AL.
		enum class LineBreakClass : uint8_t { AL, ZW, GL, BA, HY, B2, ID, CL, CP, EX, IS, NS, OP, QU, CM, ZWJ, NU, IN, EB, EM, BB, H2, H3, JL, JV, JT };
		struct LineBreakRange {
			char32_t first;
			char32_t last;
			LineBreakClass line_break_class;
		};

#include "UnicodeCaseTables.inl"
#include "UnicodeLineBreakTables.inl"

		template <typename T, size_t N>
		const T* FindCode(const T (&table)[N], char32_t c)
		{
			const T* end = table + N;
			const T* it = std::lower_bound(table, end, c, [](const T& entry, char32_t value) { return entry.code < value; });
			return (it != end && it->code == c) ? it : nullptr;
		}

		template <typename T, size_t N>
		const T* FindRange(const T (&table)[N], char32_t c)
		{
			const T* end = table + N;
			const T* it = std::upper_bound(table, end, c, [](char32_t value, const T& entry) { return value < entry.first; });
			if (it == table)
				return nullptr;
			--it;
			return (c <= it->last) ? it : nullptr;
		}

		void AppendUTF8(String& out, char32_t c)
		{
			if (c < 0x80)
				out += static_cast<char>(c);
			else if (c < 0x800)
			{
				out += static_cast<char>(0xC0 | (c >> 6));
				out += static_cast<char>(0x80 | (c & 0x3F));
			}
			else if (c < 0x10000)
			{
				out += static_cast<char>(0xE0 | (c >> 12));
				out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (c & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | (c >> 18));
				out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (c & 0x3F));
			}
		}

		enum class CaseKind { Upper, Lower };

		template <size_t NS, size_t NM>
		void AppendMapped(String& out, char32_t c, CaseKind kind, const CaseSequence (&sequences)[NS], const CaseMapping (&mappings)[NM])
		{
			if (c < 0x80)
			{
				// ASCII without the tables
				if (kind == CaseKind::Upper && c >= 'a' && c <= 'z')
					c -= 'a' - 'A';
				else if (kind == CaseKind::Lower && c >= 'A' && c <= 'Z')
					c += 'a' - 'A';
				out += static_cast<char>(c);
				return;
			}
			if (const CaseSequence* sequence = FindCode(sequences, c))
			{
				for (int i = 0; i < sequence->length; i++)
					AppendUTF8(out, case_sequence_pool[sequence->first + i]);
				return;
			}
			const CaseMapping* mapping = FindCode(mappings, c);
			AppendUTF8(out, mapping ? mapping->mapping : c);
		}

		LineBreakClass GetLineBreakClass(char32_t c)
		{
			if (c < 0x80)
			{
				// the ASCII classes, without a search
				switch (c)
				{
				case '-': return LineBreakClass::HY;
				case ')':
				case ']': return LineBreakClass::CP;
				case '}': return LineBreakClass::CL;
				case '!':
				case '?': return LineBreakClass::EX;
				case ',':
				case '.':
				case ':':
				case ';': return LineBreakClass::IS;
				case '(':
				case '[':
				case '{': return LineBreakClass::OP;
				case '"':
				case '\'': return LineBreakClass::QU;
				case '/': return LineBreakClass::AL; // SY: no break opportunity of its own here
				default: break;
				}
				if (c >= '0' && c <= '9')
					return LineBreakClass::NU;
				if (c < 0x20 || c == 0x7F)
					return LineBreakClass::CM;
				return LineBreakClass::AL;
			}
			const LineBreakRange* range = FindRange(line_break_ranges, c);
			return range ? range->line_break_class : LineBreakClass::AL;
		}

		bool IsIdeographic(LineBreakClass c)
		{
			switch (c)
			{
			case LineBreakClass::ID:
			case LineBreakClass::EB:
			case LineBreakClass::EM:
			case LineBreakClass::H2:
			case LineBreakClass::H3:
			case LineBreakClass::JL:
			case LineBreakClass::JV:
			case LineBreakClass::JT: return true;
			default: return false;
			}
		}

		bool IsLetterLike(LineBreakClass c)
		{
			return IsIdeographic(c) || c == LineBreakClass::AL || c == LineBreakClass::NU;
		}
	} // namespace

	void AppendUppercase(String& out, Character c)
	{
		AppendMapped(out, static_cast<char32_t>(c), CaseKind::Upper, case_sequence_upper, case_mapping_upper);
	}

	void AppendLowercase(String& out, Character c)
	{
		AppendMapped(out, static_cast<char32_t>(c), CaseKind::Lower, case_sequence_lower, case_mapping_lower);
	}

	void AppendTitlecase(String& out, Character c)
	{
		// the simple titlecase mapping, else the simple uppercase one ('capitalize' as Chromium: 'ß' stays)
		const char32_t code = static_cast<char32_t>(c);
		const CaseMapping* mapping = (code >= 0x80) ? FindCode(case_mapping_title, code) : nullptr;
		AppendUTF8(out, mapping ? mapping->mapping : static_cast<char32_t>(ToUpper(c)));
	}

	Character ToUpper(Character c)
	{
		const char32_t code = static_cast<char32_t>(c);
		if (code < 0x80)
			return (code >= 'a' && code <= 'z') ? static_cast<Character>(code - ('a' - 'A')) : c;
		const CaseMapping* mapping = FindCode(case_mapping_upper, code);
		return mapping ? static_cast<Character>(mapping->mapping) : c;
	}

	Character ToLower(Character c)
	{
		const char32_t code = static_cast<char32_t>(c);
		if (code < 0x80)
			return (code >= 'A' && code <= 'Z') ? static_cast<Character>(code + ('a' - 'A')) : c;
		const CaseMapping* mapping = FindCode(case_mapping_lower, code);
		return mapping ? static_cast<Character>(mapping->mapping) : c;
	}

	bool IsWordCharacter(Character c)
	{
		const char32_t code = static_cast<char32_t>(c);
		if (code < 0x80)
			return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9');
		return FindRange(word_character_ranges, code) != nullptr;
	}

	bool ContinuesCluster(Character c)
	{
		const char32_t code = static_cast<char32_t>(c);
		if (code < 0x300)
			return false;
		const LineBreakClass line_break_class = GetLineBreakClass(code);
		return line_break_class == LineBreakClass::CM || line_break_class == LineBreakClass::ZWJ || line_break_class == LineBreakClass::EM;
	}

	int FirstStrongDirection(StringView text)
	{
		const char* p = text.begin();
		while (p < text.end())
		{
			const char32_t code = static_cast<char32_t>(DecodeUTF8(p, text.end()));
			// Hebrew, Arabic, Syriac, Thaana, NKo, Samaritan, Mandaic and their presentation forms; the RTL supplementary blocks
			if ((code >= 0x0590 && code <= 0x08FF) || (code >= 0xFB1D && code <= 0xFDFF) || (code >= 0xFE70 && code <= 0xFEFF) ||
				(code >= 0x10800 && code <= 0x10FFF) || (code >= 0x1E800 && code <= 0x1EFFF))
				return -1;
			if (IsWordCharacter(static_cast<Character>(code)) && GetLineBreakClass(code) != LineBreakClass::NU && !ContinuesCluster(static_cast<Character>(code)))
				return 1;
		}
		return 0;
	}

	bool IsWordStart(Character before_previous, Character previous, Character c)
	{
		// A connector ('_') joins the characters around it into one word (UAX #29 WB13a/b).
		auto is_connector = [](char32_t x) {
			return x == '_' || x == 0x203F || x == 0x2040 || x == 0x2054 || x == 0xFE33 || x == 0xFE34 || (x >= 0xFE4D && x <= 0xFE4F) || x == 0xFF3F;
		};
		auto is_word = [&](Character x) { return IsWordCharacter(x) || is_connector(static_cast<char32_t>(x)); };
		auto is_digit = [](Character x) {
			const char32_t d = static_cast<char32_t>(x);
			return (d >= '0' && d <= '9') || (d >= 0x0660 && d <= 0x0669) || (d >= 0xFF10 && d <= 0xFF19);
		};

		if (!IsWordCharacter(c))
			return false;
		if (previous == Character::Null)
			return true;
		if (is_word(previous))
			return false;

		const char32_t p = static_cast<char32_t>(previous);
		// Apostrophes and middle dots inside a word ("don't"), as Chromium's word breaking: a period or a colon between
		// letters starts a new word there ("E.G.").
		const bool mid_letter = (p == '\'' || p == 0x2019 || p == 0x00B7 || p == 0x0387 || p == 0x05F4 || p == 0x2027 || p == 0xFF07);
		if (mid_letter && IsWordCharacter(before_previous) && !is_digit(before_previous) && !is_digit(c))
			return false;
		// Separators inside a number ("1,5", "1.5").
		const bool mid_number = (p == ',' || p == '.' || p == ';' || p == '\'' || p == 0x2019 || p == 0x066C || p == 0xFE50 || p == 0xFE52 ||
			p == 0xFF0C || p == 0xFF0E);
		if (mid_number && is_digit(before_previous) && is_digit(c))
			return false;
		return true;
	}

	bool IsLineBreakOpportunity(Character before, Character after, Style::WordBreak word_break)
	{
		using C = LineBreakClass;
		C a = GetLineBreakClass(static_cast<char32_t>(before));
		const C b = GetLineBreakClass(static_cast<char32_t>(after));

		// LB8: after a zero-width space
		if (a == C::ZW)
			return true;
		// LB8a, LB9: inside an emoji sequence, before a combining mark
		if (a == C::ZWJ || b == C::CM || b == C::ZWJ)
			return false;
		// LB10: a lone combining mark is a letter
		if (a == C::CM)
			a = C::AL;
		// LB11, LB12, LB12a: word joiners and glue
		if (a == C::GL || b == C::GL)
			return false;
		// LB13: before closing punctuation, exclamations and infix separators
		if (b == C::CL || b == C::CP || b == C::EX || b == C::IS)
			return false;
		// LB14: after opening punctuation
		if (a == C::OP)
			return false;
		// LB19: around quotation marks
		if (a == C::QU || b == C::QU)
			return false;
		// LB21: before hyphens, break-after characters and non-starters; after break-before characters
		if (b == C::BA || b == C::HY || b == C::NS || a == C::BB)
			return false;
		// LB22: before inseparable characters (ellipsis)
		if (b == C::IN)
			return false;
		// LB17: between em dashes
		if (a == C::B2 && b == C::B2)
			return false;
		// LB25 (subset): a hyphen before a number is a sign
		if (a == C::HY && b == C::NU)
			return false;
		// LB26, LB27: Hangul syllable blocks
		if (a == C::JL && (b == C::JL || b == C::JV || b == C::H2 || b == C::H3))
			return false;
		if ((a == C::JV || a == C::H2) && (b == C::JV || b == C::JT))
			return false;
		if ((a == C::JT || a == C::H3) && b == C::JT)
			return false;
		// LB30b: emoji base and modifier
		if (a == C::EB && b == C::EM)
			return false;
		// LB31 for the rest: after hyphens, dashes and break-after characters (a soft hyphen is one), around em dashes
		if (a == C::BA || a == C::HY || a == C::B2 || b == C::B2)
			return true;
		// around ideographs and emoji; 'word-break: keep-all' keeps them together with letters (CSS Text 3 §5.2)
		if (IsIdeographic(a) || IsIdeographic(b))
			return !(word_break == Style::WordBreak::KeepAll && IsLetterLike(a) && IsLetterLike(b));
		// LB28, LB23, LB24, LB29, LB30: letters, numbers and punctuation between them stay together
		return false;
	}

	Character DecodeUTF8(const char*& p, const char* end)
	{
		if (p >= end)
			return Character::Null;
		const char* begin = p;
		p = StringUtilities::SeekForwardUTF8(p + 1, end);
		return StringUtilities::ToCharacter(begin, p);
	}

} // namespace TextUnicode
} // namespace Rml
