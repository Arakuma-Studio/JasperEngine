#include "TextUnicode.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <algorithm>
#include <array>
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

		// The UAX #14 classes (LB1 applied by the generator: AI, SG, XX are AL, SA is CM or AL), QU split by General_Category
		// Pi and Pf; in the order of Utilities/gen_case_tables.py's LINE_BREAK_CLASSES.
		enum class LineBreakClass : uint8_t {
			AL, AK, AP, AS, B2, BA, BB, BK, CB, CJ, CL, CM, CP, CR, EB, EM, EX, GL, H2, H3, HL, HY, ID, IN, IS, JL, JT, JV, LF, NL, NS,
			NU, OP, PO, PR, QU, QU_Pi, QU_Pf, RI, SP, SY, VF, VI, WJ, ZW, ZWJ
		};
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

		LineBreakClass LookUpLineBreakClass(char32_t c)
		{
			const LineBreakRange* range = FindRange(line_break_ranges, c);
			return range ? range->line_break_class : LineBreakClass::AL;
		}

		LineBreakClass GetLineBreakClass(char32_t c)
		{
			// ASCII from a table built from the generated ranges once
			static const auto ascii = [] {
				std::array<LineBreakClass, 128> table{};
				for (char32_t i = 0; i < 128; i++)
					table[i] = LookUpLineBreakClass(i);
				return table;
			}();
			return c < 128 ? ascii[c] : LookUpLineBreakClass(c);
		}

		bool IsEastAsian(char32_t c)
		{
			return c >= 0x1100 && FindRange(east_asian_ranges, c) != nullptr;
		}

		bool IsUnassignedPictographic(char32_t c)
		{
			return c >= 0x1F000 && FindRange(unassigned_pictographic_ranges, c) != nullptr;
		}

		// 'keep-all' keeps these together (CSS Text 3 §5.2: letters, digits, ideographs and Hangul).
		bool IsLetterLike(LineBreakClass c)
		{
			using C = LineBreakClass;
			switch (c)
			{
			case C::AL:
			case C::HL:
			case C::NU:
			case C::ID:
			case C::CJ:
			case C::EB:
			case C::EM:
			case C::H2:
			case C::H3:
			case C::JL:
			case C::JV:
			case C::JT: return true;
			default: return false;
			}
		}

		// Chromium's break_iterator.cc: the break opportunities between two printable ASCII characters, a row per first
		// character ('!' to '~'), a bit per second character (measured on Chromium 153 with tools/htmlui/linebreak-oracle.mjs:
		// a break before '(' '<' '[' '{' after a symbol or punctuation, after '-' and '?' before most characters).
		bool AsciiBreakBetween(char32_t before_previous, char32_t previous, char32_t c)
		{
			// '-' before a digit is a minus sign unless a letter or digit is before it ("ABCD-1234", "1234-5678")
			if (previous == '-' && c >= '0' && c <= '9')
				return (before_previous >= '0' && before_previous <= '9') || (before_previous >= 'a' && before_previous <= 'z') ||
					(before_previous >= 'A' && before_previous <= 'Z');
			const auto is_opening = [](char32_t x) { return x == '(' || x == '<' || x == '[' || x == '{'; };
			switch (previous)
			{
			case '!':
			case '"':
			case '#':
			case '%':
			case '&':
			case ')':
			case '*':
			case '+':
			case ',':
			case '.':
			case ':':
			case ';':
			case '=':
			case '>':
			case '\\':
			case ']':
			case '|':
			case '}':
			case '~': return is_opening(c);
			case '-':
				switch (c)
				{
				case '!':
				case '$':
				case ')':
				case ',':
				case '.':
				case '/':
				case ':':
				case ';':
				case '?':
				case ']':
				case '}': return false;
				default: return true;
				}
			case '?':
				switch (c)
				{
				case '!':
				case '"':
				case '\'':
				case ')':
				case ',':
				case '.':
				case '/':
				case ':':
				case ';':
				case '?':
				case ']':
				case '}': return false;
				default: return true;
				}
			default: return false;
			}
		}

		// The rules of UAX #14 over one text, a boundary at a time.
		class LineBreaker {
		public:
			LineBreaker(Span<const Character> text, LineBreakTailoring tailoring) : text(text), tailoring(tailoring)
			{
				using C = LineBreakClass;
				const int n = (int)text.size();
				raw.resize(n);
				cls.resize(n);
				start.resize(n);
				for (int i = 0; i < n; i++)
				{
					C c = GetLineBreakClass((char32_t)text[i]);
					// LB1: CJ is NS (strict); CSS 'line-break: normal', as Chromium: ID
					if (c == C::CJ)
						c = (tailoring == LineBreakTailoring::Chromium ? C::ID : C::NS);
					raw[i] = c;
				}
				// LB9: X (CM | ZWJ)* is X, for X other than BK, CR, LF, NL, SP and ZW; LB10: the others are AL
				for (int i = 0; i < n; i++)
				{
					const bool combining = (raw[i] == C::CM || raw[i] == C::ZWJ);
					if (combining && i > 0 && !IsBreakOrSpace(cls[i - 1]))
					{
						cls[i] = cls[i - 1];
						start[i] = start[i - 1];
					}
					else
					{
						cls[i] = (combining ? C::AL : raw[i]);
						start[i] = i;
					}
				}
			}

			bool BreakBefore(int k) const
			{
				using C = LineBreakClass;
				const int n = (int)raw.size();
				const C a = raw[k - 1], b = raw[k];
				const bool chromium = (tailoring == LineBreakTailoring::Chromium);

				// Chromium: two printable ASCII characters are its table's alone
				if (chromium)
				{
					const char32_t previous = (char32_t)text[k - 1], c = (char32_t)text[k];
					if (previous >= 0x21 && previous <= 0x7E && c >= 0x21 && c <= 0x7E)
						return AsciiBreakBetween(k >= 2 ? (char32_t)text[k - 2] : 0, previous, c);
				}

				// LB4, LB5: hard line breaks
				if (a == C::BK)
					return true;
				if (a == C::CR && b == C::LF)
					return false;
				if (a == C::CR || a == C::LF || a == C::NL)
					return true;
				// LB6: not before one
				if (b == C::BK || b == C::CR || b == C::LF || b == C::NL)
					return false;
				// Chromium: not before U+3000 IDEOGRAPHIC SPACE
				if (chromium && (char32_t)text[k] == 0x3000)
					return false;
				// LB7: not before spaces or a zero width space
				if (b == C::SP || b == C::ZW)
					return false;
				// LB8: after a zero width space, even after spaces
				{
					int j = k - 1;
					while (j >= 0 && raw[j] == C::SP)
						j--;
					if (j >= 0 && raw[j] == C::ZW)
						return true;
				}
				// LB8a: not after a zero width joiner
				if (a == C::ZWJ)
					return false;
				// LB9: not inside a combining sequence
				if (start[k] != k)
					return false;

				// From here on: the classes after LB9 and LB10; the item before the boundary starts at 'p'
				const C A = cls[k - 1], B = cls[k];
				const int p = start[k - 1];
				const C before_previous = (p > 0 ? cls[p - 1] : C::BK); // (sot reads as BK where the rules list sot beside BK)
				const bool at_start = (p == 0);
				int next = k + 1;
				while (next < n && start[next] != next)
					next++;
				const bool has_next = (next < n);
				const C after = (has_next ? cls[next] : C::BK);

				// LB11: word joiners
				if (A == C::WJ || B == C::WJ)
					return false;
				// LB12: after glue
				if (A == C::GL)
					return false;
				// LB12a: before glue, except after spaces and hyphens
				if (B == C::GL && A != C::SP && A != C::BA && A != C::HY)
					return false;
				// LB13: before closing punctuation, '!', '/'
				if (B == C::CL || B == C::CP || B == C::EX || B == C::SY)
					return false;
				// LB14: after opening punctuation, even after spaces
				const C before_spaces = ClassBeforeSpaces(k);
				if (before_spaces == C::OP)
					return false;
				// LB15a: after an initial quotation mark at the start of a line, after a space or an opening, even after spaces
				if (before_spaces == C::QU_Pi)
				{
					int j = k - 1;
					while (j >= 0 && cls[j] == C::SP)
						j--;
					const int q = start[j];
					if (q == 0)
						return false;
					const C x = cls[q - 1];
					if (x == C::BK || x == C::CR || x == C::LF || x == C::NL || x == C::OP || IsQuotation(x) || x == C::GL || x == C::SP ||
						x == C::ZW)
						return false;
				}
				// LB15b: before a final quotation mark at the end of a line, before a space or a prohibited break
				if (B == C::QU_Pf)
				{
					if (!has_next)
						return false;
					switch (after)
					{
					case C::SP:
					case C::GL:
					case C::WJ:
					case C::CL:
					case C::QU:
					case C::QU_Pi:
					case C::QU_Pf:
					case C::CP:
					case C::EX:
					case C::IS:
					case C::SY:
					case C::BK:
					case C::CR:
					case C::LF:
					case C::NL:
					case C::ZW: return false;
					default: break;
					}
				}
				// LB15c: before a decimal mark after a space
				if (A == C::SP && B == C::IS && has_next && after == C::NU)
					return true;
				// LB15d: otherwise not before an infix separator
				if (B == C::IS)
					return false;
				// LB16: between closing punctuation and a nonstarter, even after spaces
				if (B == C::NS && (before_spaces == C::CL || before_spaces == C::CP))
					return false;
				// LB17: inside an em dash pair, even after spaces
				if (B == C::B2 && before_spaces == C::B2)
					return false;
				// LB18: after spaces
				if (A == C::SP)
					return true;
				// LB19: not before a quotation mark that is not initial, nor after one that is not final
				if (B == C::QU || B == C::QU_Pf)
					return false;
				if (A == C::QU || A == C::QU_Pi)
					return false;
				// LB19a: around quotation marks unless East Asian characters are on both sides
				if (IsQuotation(B))
				{
					if (!IsEastAsianItem(p))
						return false;
					if (!has_next || !IsEastAsianItem(next))
						return false;
				}
				if (IsQuotation(A))
				{
					if (!IsEastAsianItem(k))
						return false;
					if (at_start || !IsEastAsianItem(start[p - 1]))
						return false;
				}
				// LB20: around contingent breaks
				if (A == C::CB || B == C::CB)
					return true;
				// LB20a: not after a word-initial hyphen (before a Hebrew letter too, as Chromium's ICU)
				if ((A == C::HY || IsItemCharacter(p, 0x2010)) && (B == C::AL || (chromium && B == C::HL)))
				{
					if (at_start)
						return false;
					switch (before_previous)
					{
					case C::BK:
					case C::CR:
					case C::LF:
					case C::NL:
					case C::SP:
					case C::ZW:
					case C::CB:
					case C::GL: return false;
					default: break;
					}
				}
				// LB21: before hyphens and nonstarters, after acute accents
				if (B == C::BA || B == C::HY || B == C::NS || A == C::BB)
					return false;
				// LB21a: after the hyphen in Hebrew + hyphen + non-Hebrew
				if ((A == C::HY || (A == C::BA && !IsEastAsianItem(p))) && B != C::HL && !at_start && before_previous == C::HL)
					return false;
				// LB21b: between a solidus and Hebrew
				if (A == C::SY && B == C::HL)
					return false;
				// LB22: before ellipses
				if (B == C::IN)
					return false;
				// LB23: between digits and letters
				if ((A == C::AL || A == C::HL) && B == C::NU)
					return false;
				if (A == C::NU && (B == C::AL || B == C::HL))
					return false;
				// LB23a: between numeric prefixes and ideographs, ideographs and numeric postfixes
				if (A == C::PR && (B == C::ID || B == C::EB || B == C::EM))
					return false;
				if ((A == C::ID || A == C::EB || A == C::EM) && B == C::PO)
					return false;
				// LB24: between numeric prefixes or postfixes and letters
				if ((A == C::PR || A == C::PO) && (B == C::AL || B == C::HL))
					return false;
				if ((A == C::AL || A == C::HL) && (B == C::PR || B == C::PO))
					return false;
				// LB25: inside numbers
				if (B == C::PO || B == C::PR)
				{
					int j = p;
					if (cls[j] == C::CL || cls[j] == C::CP)
						j = (j > 0 ? start[j - 1] : -1);
					while (j >= 0 && (cls[j] == C::SY || cls[j] == C::IS))
						j = (j > 0 ? start[j - 1] : -1);
					if (j >= 0 && cls[j] == C::NU)
						return false;
				}
				if ((A == C::PO || A == C::PR) && B == C::OP && has_next)
				{
					if (after == C::NU)
						return false;
					if (after == C::IS)
					{
						int after_next = next + 1;
						while (after_next < n && start[after_next] != after_next)
							after_next++;
						if (after_next < n && cls[after_next] == C::NU)
							return false;
					}
				}
				if ((A == C::PO || A == C::PR || A == C::HY || A == C::IS) && B == C::NU)
					return false;
				if (B == C::NU)
				{
					int j = p;
					while (j >= 0 && (cls[j] == C::SY || cls[j] == C::IS))
						j = (j > 0 ? start[j - 1] : -1);
					if (j >= 0 && cls[j] == C::NU)
						return false;
				}
				// LB26: inside Korean syllables
				if (A == C::JL && (B == C::JL || B == C::JV || B == C::H2 || B == C::H3))
					return false;
				if ((A == C::JV || A == C::H2) && (B == C::JV || B == C::JT))
					return false;
				if ((A == C::JT || A == C::H3) && B == C::JT)
					return false;
				// LB27: a Korean syllable block is an ideograph before a postfix, after a prefix
				const auto korean = [](C x) { return x == C::JL || x == C::JV || x == C::JT || x == C::H2 || x == C::H3; };
				if (korean(A) && B == C::PO)
					return false;
				if (A == C::PR && korean(B))
					return false;
				// LB28: between letters
				if ((A == C::AL || A == C::HL) && (B == C::AL || B == C::HL))
					return false;
				// LB28a: inside the orthographic syllables of Brahmic scripts (U+25CC DOTTED CIRCLE stands for a consonant)
				{
					const bool b_consonant = (B == C::AK || B == C::AS || IsItemCharacter(k, 0x25CC));
					const bool a_consonant = (A == C::AK || A == C::AS || IsItemCharacter(p, 0x25CC));
					if (A == C::AP && b_consonant)
						return false;
					if (a_consonant && (B == C::VF || B == C::VI))
						return false;
					if (A == C::VI && (B == C::AK || IsItemCharacter(k, 0x25CC)) && !at_start &&
						(before_previous == C::AK || before_previous == C::AS || IsItemCharacter(start[p - 1], 0x25CC)))
						return false;
					if (a_consonant && b_consonant && has_next && after == C::VF)
						return false;
				}
				// LB29: between numeric punctuation and letters
				if (A == C::IS && (B == C::AL || B == C::HL))
					return false;
				// LB30: between letters or digits and parentheses that are not East Asian
				if ((A == C::AL || A == C::HL || A == C::NU) && B == C::OP && !IsEastAsianItem(k))
					return false;
				if (A == C::CP && !IsEastAsianItem(p) && (B == C::AL || B == C::HL || B == C::NU))
					return false;
				// LB30a: regional indicators in pairs
				if (A == C::RI && B == C::RI)
				{
					int count = 0;
					for (int j = p; j >= 0 && cls[j] == C::RI; j = (j > 0 ? start[j - 1] : -1))
						count++;
					if (count % 2 == 1)
						return false;
				}
				// LB30b: an emoji base (or an unassigned pictograph) and an emoji modifier
				if (B == C::EM && (A == C::EB || (raw[p] != C::CM && raw[p] != C::ZWJ && IsUnassignedPictographic((char32_t)text[p]))))
					return false;
				// LB31: everywhere else
				return true;
			}

		private:
			static bool IsBreakOrSpace(LineBreakClass c)
			{
				using C = LineBreakClass;
				return c == C::BK || c == C::CR || c == C::LF || c == C::NL || c == C::SP || c == C::ZW;
			}
			static bool IsQuotation(LineBreakClass c) { return c == LineBreakClass::QU || c == LineBreakClass::QU_Pi || c == LineBreakClass::QU_Pf; }
			// The class before the spaces before the boundary at k (BK at the start of the text).
			LineBreakClass ClassBeforeSpaces(int k) const
			{
				int j = k - 1;
				while (j >= 0 && cls[j] == LineBreakClass::SP)
					j--;
				return j >= 0 ? cls[j] : LineBreakClass::BK;
			}
			// The East_Asian_Width of the item starting at i (LB10: a lone combining mark is as 'A', not East Asian).
			bool IsEastAsianItem(int i) const
			{
				if (raw[i] == LineBreakClass::CM || raw[i] == LineBreakClass::ZWJ)
					return false;
				return IsEastAsian((char32_t)text[i]);
			}
			bool IsItemCharacter(int i, char32_t c) const { return (char32_t)text[i] == c; }

			Span<const Character> text;
			LineBreakTailoring tailoring;
			Vector<LineBreakClass> raw; // the classes of LB1 (CJ resolved)
			Vector<LineBreakClass> cls; // the classes with LB9 and LB10 applied
			Vector<int> start;          // the first code point of the item (LB9's combining sequence) each code point is in
		};
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

	void GetLineBreakOpportunities(Span<const Character> text, Style::WordBreak word_break, LineBreakTailoring tailoring, Vector<bool>& out_breaks)
	{
		const int n = (int)text.size();
		out_breaks.assign(n + 1, false);
		if (n == 0)
			return;
		out_breaks[n] = true;
		const LineBreaker breaker(text, tailoring);
		for (int k = 1; k < n; k++)
		{
			bool allowed = breaker.BreakBefore(k);
			// 'keep-all': not between letters, digits and ideographs
			if (allowed && word_break == Style::WordBreak::KeepAll &&
				IsLetterLike(GetLineBreakClass((char32_t)text[k - 1])) && IsLetterLike(GetLineBreakClass((char32_t)text[k])))
				allowed = false;
			out_breaks[k] = allowed;
		}
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
