#include "TextJustification.h"
#include "TextUnicode.h"
#include <algorithm>
#include <cstdint>

namespace Rml {
namespace TextJustification {

	namespace {
		struct CodepointRange {
			char32_t first;
			char32_t last;
		};

		// The UAX #29 Grapheme_Cluster_Break values, and Extended_Pictographic.
		enum class GraphemeBreak : uint8_t { Other, CR, LF, Control, Extend, ZWJ, SpacingMark, Prepend, Regional_Indicator, L, V, T, LV, LVT, ExtPict };
		struct GraphemeBreakRange {
			char32_t first;
			char32_t last;
			GraphemeBreak value;
		};
		// Indic_Conjunct_Break (GB9c).
		enum class IndicConjunctBreak : uint8_t { None, Consonant, Extend, Linker };
		struct IndicConjunctBreakRange {
			char32_t first;
			char32_t last;
			IndicConjunctBreak value;
		};

#include "UnicodeJustificationTables.inl"

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

		GraphemeBreak GetGraphemeBreak(char32_t c)
		{
			// (printable ASCII is Other: the common case)
			if (c >= 0x20 && c < 0x7F)
				return GraphemeBreak::Other;
			const GraphemeBreakRange* range = FindRange(grapheme_break_ranges, c);
			return range ? range->value : GraphemeBreak::Other;
		}

		IndicConjunctBreak GetIndicConjunctBreak(char32_t c)
		{
			// (the first one is U+0300, a combining mark)
			if (c < 0x300)
				return IndicConjunctBreak::None;
			const IndicConjunctBreakRange* range = FindRange(indic_conjunct_break_ranges, c);
			return range ? range->value : IndicConjunctBreak::None;
		}
	} // namespace

	bool IsCjkIdeographOrSymbol(Character c)
	{
		// Below U+02C7 is the common case (Chromium's fast path).
		const char32_t v = (char32_t)c;
		return v >= 0x2C7 && FindRange(cjk_ideograph_or_symbol_ranges, v) != nullptr;
	}

	bool IsCursiveScript(Character c)
	{
		const char32_t v = (char32_t)c;
		return v >= 0x600 && FindRange(cursive_script_ranges, v) != nullptr;
	}

	bool IsDefaultIgnorable(Character c)
	{
		const char32_t v = (char32_t)c;
		if (v < 0x100)
			return v == 0xAD;
		return FindRange(default_ignorable_ranges, v) != nullptr;
	}

	void GetClusters(StringView text, Vector<Cluster>& out)
	{
		const char* begin = text.begin();
		const char* end = text.end();
		const char* p = begin;

		GraphemeBreak previous = GraphemeBreak::Other;
		bool first = true;
		bool in_pictographic = false;     // after Extended_Pictographic Extend*
		bool pictographic_joiner = false; // after Extended_Pictographic Extend* ZWJ (GB11)
		int regional_indicators = 0;      // the regional indicators before this character, in a row (GB12, GB13)
		// GB9c: after InCB=Consonant [InCB=Extend InCB=Linker]* (1), with a Linker among them (2)
		int conjunct = 0;

		while (p < end)
		{
			const char* start = p;
			const Character c = TextUnicode::DecodeUTF8(p, end);
			const GraphemeBreak value = GetGraphemeBreak((char32_t)c);
			const IndicConjunctBreak incb = GetIndicConjunctBreak((char32_t)c);

			bool joins = false;
			if (!first)
			{
				using G = GraphemeBreak;
				if (previous == G::CR && value == G::LF)
					joins = true; // GB3
				else if (previous == G::CR || previous == G::LF || previous == G::Control || value == G::CR || value == G::LF ||
					value == G::Control)
					joins = false; // GB4, GB5
				else if (previous == G::L && (value == G::L || value == G::V || value == G::LV || value == G::LVT))
					joins = true; // GB6
				else if ((previous == G::LV || previous == G::V) && (value == G::V || value == G::T))
					joins = true; // GB7
				else if ((previous == G::LVT || previous == G::T) && value == G::T)
					joins = true; // GB8
				else if (value == G::Extend || value == G::ZWJ || value == G::SpacingMark || previous == G::Prepend)
					joins = true; // GB9, GB9a, GB9b
				else if (conjunct == 2 && incb == IndicConjunctBreak::Consonant)
					joins = true; // GB9c
				else if (pictographic_joiner && value == G::ExtPict)
					joins = true; // GB11
				else if (value == G::Regional_Indicator && regional_indicators % 2 == 1)
					joins = true; // GB12, GB13
			}

			if (!joins)
				out.push_back(Cluster{int(start - begin), c});

			pictographic_joiner = (in_pictographic && value == GraphemeBreak::ZWJ);
			in_pictographic = (value == GraphemeBreak::ExtPict || (in_pictographic && value == GraphemeBreak::Extend));
			regional_indicators = (value == GraphemeBreak::Regional_Indicator ? regional_indicators + 1 : 0);
			if (incb == IndicConjunctBreak::Consonant)
				conjunct = 1;
			else if (conjunct != 0 && incb == IndicConjunctBreak::Linker)
				conjunct = 2;
			else if (conjunct != 0 && incb != IndicConjunctBreak::Extend)
				conjunct = 0;
			previous = value;
			first = false;
		}
	}

} // namespace TextJustification

JustificationContext::Opportunities JustificationContext::Check(Style::TextJustify method, Character c)
{
	using namespace TextJustification;

	if (IsDefaultIgnorable(c))
		return {};

	Type type = Type::Normal;
	if (c == ObjectReplacementCharacter)
		type = Type::AtomicInline;
	else if (IsCursiveScript(c))
		type = Type::Cursive;
	const Type type_before = previous_type;
	previous_type = type;

	switch (method)
	{
	case Style::TextJustify::None: after_opportunity = false; return {};

	case Style::TextJustify::InterCharacter:
	{
		if (type != Type::Normal)
		{
			// Atomic inlines and cursive letters: before one when the character before is of another type, never after
			// (the next character may be of the same type).
			const bool before = (!after_opportunity && type_before != type);
			after_opportunity = false;
			return {before, false};
		}
		// before this character after an atomic inline or a cursive letter
		const bool before = !after_opportunity;
		after_opportunity = true;
		return {before, true};
	}

	case Style::TextJustify::InterWord:
		if (TreatAsSpace(c))
		{
			after_opportunity = true;
			return {false, true};
		}
		after_opportunity = false;
		return {};

	case Style::TextJustify::Auto: break;
	}

	if (TreatAsSpace(c))
	{
		after_opportunity = true;
		return {false, true};
	}
	if (!IsCjkIdeographOrSymbol(c))
	{
		after_opportunity = false;
		return {};
	}
	// Ideographs: before (unless the character before has one after it, or at the start) and after each one (JLREQ).
	const bool before = !after_opportunity;
	after_opportunity = true;
	return {before, true};
}

namespace {
	// Chromium's ShapeResultSpacing::NextExpansion: the same space for every opportunity, what is left for the last one.
	class JustificationExpansion {
	public:
		JustificationExpansion(float space, int opportunities) :
			remaining(space), per_opportunity(space / float(opportunities)), opportunities(opportunities)
		{}

		bool HasExpansion() const { return opportunities > 0; }

		float Next()
		{
			if (opportunities <= 0)
				return 0.f;
			if (--opportunities == 0)
			{
				const float last = remaining;
				remaining = 0.f;
				return last;
			}
			remaining -= per_opportunity;
			return per_opportunity;
		}

	private:
		float remaining;
		float per_opportunity;
		int opportunities;
	};

	template <typename Func>
	void VisitRuns(Vector<JustificationRun>& runs, bool reverse, Vector<TextJustification::Cluster>& clusters, Func&& func)
	{
		const int count = (int)runs.size();
		for (int r = 0; r < count; r++)
		{
			JustificationRun& run = runs[reverse ? count - 1 - r : r];
			if (run.atomic)
			{
				func(run, -1, TextJustification::ObjectReplacementCharacter);
				continue;
			}
			clusters.clear();
			TextJustification::GetClusters(run.text, clusters);
			const int num_clusters = (int)clusters.size();
			for (int i = 0; i < num_clusters; i++)
			{
				const TextJustification::Cluster& cluster = clusters[reverse ? num_clusters - 1 - i : i];
				func(run, cluster.offset, cluster.first);
			}
		}
	}
} // namespace

bool JustifyRuns(Vector<JustificationRun>& runs, float space, bool reverse)
{
	if (!(space > 0.f))
		return false;

	Vector<TextJustification::Cluster> clusters;

	// Count the opportunities.
	int opportunities = 0;
	{
		JustificationContext context;
		VisitRuns(runs, reverse, clusters, [&](JustificationRun& run, int /*offset*/, Character c) {
			const JustificationContext::Opportunities found = context.Check(run.method, c);
			opportunities += int(found.before) + int(found.after);
		});
		// none after the last character
		if (context.IsAfterOpportunity() && opportunities > 0)
			opportunities -= 1;
	}
	if (opportunities == 0)
		return false;

	// Share the space.
	JustificationExpansion expansion(space, opportunities);
	JustificationContext context;
	VisitRuns(runs, reverse, clusters, [&](JustificationRun& run, int offset, Character c) {
		const JustificationContext::Opportunities found = context.Check(run.method, c);
		float before = 0.f, after = 0.f;
		if (found.before)
			before = expansion.Next();
		if (found.after && expansion.HasExpansion())
			after = expansion.Next();
		if (before == 0.f && after == 0.f)
			return;
		if (run.atomic)
		{
			run.before += before;
			run.after += after;
			return;
		}
		run.spacing.push_back(JustificationSpacing{offset, before, after});
		run.before += before;
		run.after += after;
	});

	if (reverse)
	{
		for (JustificationRun& run : runs)
			std::reverse(run.spacing.begin(), run.spacing.end());
	}
	return true;
}

float GetJustificationBefore(Span<const JustificationSpacing> spacing, int end)
{
	float sum = 0.f;
	for (const JustificationSpacing& entry : spacing)
	{
		if (entry.offset >= end)
			break;
		sum += entry.before + entry.after;
	}
	return sum;
}

} // namespace Rml
