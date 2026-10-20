#pragma once

#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <doctest.h>

// A font engine of fixed advances for the text layout tests of phase 3 (p3-text): every character is as wide as the font
// size (like Ahem), the default-ignorable characters (zero-width space, joiners, soft hyphen, BOM) take no space, a
// space is as wide as a letter. Letter spacing is added after each character that takes space, word spacing after each
// space and no-break space. GenerateString draws one quad per character that takes space, so text geometry can be
// inspected.
class FixedAdvanceFontEngine : public Rml::FontEngineInterface {
public:
	static bool IsZeroWidth(char32_t c)
	{
		return c == 0x200B || c == 0x200C || c == 0x200D || c == 0x2060 || c == 0x00AD || c == 0xFEFF || c < 0x20 || (c >= 0x202A && c <= 0x202E) ||
			(c >= 0x2066 && c <= 0x2069) || c == 0x200E || c == 0x200F;
	}

	bool LoadFontFace(const Rml::String&, int, bool, Rml::Style::FontWeight) override { return true; }
	bool LoadFontFace(const Rml::String&, int, const Rml::String&, Rml::Style::FontStyle, Rml::Style::FontWeight, bool) override { return true; }
	bool LoadFontFace(Rml::Span<const Rml::byte>, int, const Rml::String&, Rml::Style::FontStyle, Rml::Style::FontWeight, bool) override
	{
		return true;
	}

	Rml::FontFaceHandle GetFontFaceHandle(const Rml::String&, Rml::Style::FontStyle, Rml::Style::FontWeight, int size) override
	{
		auto it = metrics.find(size);
		if (it == metrics.end())
		{
			Rml::FontMetrics m = {};
			m.size = size;
			m.ascent = 0.8f * size;
			m.descent = 0.2f * size;
			m.line_spacing = (float)size;
			m.x_height = 0.5f * size;
			m.underline_position = 0.1f * size;
			m.underline_thickness = 1.f;
			m.has_ellipsis = true;
			it = metrics.emplace(size, m).first;
		}
		return (Rml::FontFaceHandle)size;
	}

	Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle, const Rml::FontEffectList&) override { return 0; }

	const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override { return metrics[(int)handle]; }

	// The advance of each character, and with a justified line (p6-justify) the space before each one ('offsets').
	float Measure(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context, Rml::Vector<float>* advances,
		Rml::Vector<float>* offsets = nullptr)
	{
		const float size = (float)(int)handle;
		float width = 0.f;
		size_t spacing_index = 0;
		for (auto it = Rml::StringIteratorU8(string.begin(), string.begin(), string.end()); it; ++it)
		{
			const char32_t c = (char32_t)*it;
			float advance = 0.f, offset = 0.f;
			if (!IsZeroWidth(c))
			{
				advance = size + context.letter_spacing;
				if (c == ' ' || c == 0xA0)
					advance += context.word_spacing;
			}
			const int byte_offset = (int)(it.get() - string.begin());
			while (spacing_index < context.justification.size() && context.justification[spacing_index].offset < byte_offset)
				spacing_index++;
			if (spacing_index < context.justification.size() && context.justification[spacing_index].offset == byte_offset)
			{
				advance += context.justification[spacing_index].before + context.justification[spacing_index].after;
				offset = context.justification[spacing_index].before;
			}
			if (advances)
				advances->push_back(advance);
			if (offsets)
				offsets->push_back(offset);
			width += advance;
		}
		return width;
	}

	int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context, Rml::Character) override
	{
		return (int)Rml::Math::Round(Measure(handle, string, context, nullptr));
	}

	int GenerateString(Rml::RenderManager&, Rml::FontFaceHandle handle, Rml::FontEffectsHandle, Rml::StringView string, Rml::Vector2f position,
		Rml::ColourbPremultiplied colour, float, const Rml::TextShapingContext& context, Rml::TexturedMeshList& mesh_list) override
	{
		generated.push_back(Rml::String(string.begin(), string.end()));
		generated_positions.push_back(position);
		Rml::Vector<float> advances, offsets;
		const float width = Measure(handle, string, context, &advances, &offsets);
		const float size = (float)(int)handle;
		mesh_list.emplace_back();
		float x = position.x;
		for (size_t i = 0; i < advances.size(); i++)
		{
			if (advances[i] > 0.f)
				Rml::MeshUtilities::GenerateQuad(mesh_list.back().mesh, Rml::Vector2f(x + offsets[i], position.y - 0.8f * size), Rml::Vector2f(size, size),
					colour);
			x += advances[i];
		}
		return (int)Rml::Math::Round(width);
	}

	// A small UAX #9 for the tests (fork, P6-L1): Hebrew letters are strong right-to-left, ASCII letters left-to-right, the
	// rest neutral (the direction of the strong characters around it when they agree, the embedding's otherwise); LRO and
	// RLO ... PDF override the characters between them one embedding level up. Runs in visual order (rule L2).
	void GetBidiRuns(Rml::StringView string, const Rml::TextShapingContext& context, Rml::Vector<Rml::TextBidiRun>& runs) override
	{
		struct Unit {
			int begin, end;
			int level;    // embedding level
			int strong;   // 1 L, -1 R, 0 neutral (resolved below)
			bool control; // an override or PDF: it takes the level it opens or closes
		};
		Rml::Vector<Unit> units;
		const int base = (context.text_direction == Rml::Style::Direction::Rtl ? 1 : 0);
		struct Embedding {
			int level;
			int override_direction;
		};
		Rml::Vector<Embedding> stack{{base, 0}};
		for (auto it = Rml::StringIteratorU8(string.begin(), string.begin(), string.end()); it; ++it)
		{
			const char32_t c = (char32_t)*it;
			const int begin = (int)(it.get() - string.begin());
			Rml::StringIteratorU8 next = it;
			++next;
			const int end = (next ? (int)(next.get() - string.begin()) : (int)string.size());
			if (c == 0x202D || c == 0x202E)
			{
				const int level = stack.back().level;
				const int inner = (c == 0x202E ? (level % 2 == 0 ? level + 1 : level + 2) : (level % 2 == 0 ? level + 2 : level + 1));
				stack.push_back({inner, c == 0x202E ? -1 : 1});
				units.push_back({begin, end, inner, c == 0x202E ? -1 : 1, true});
				continue;
			}
			if (c == 0x202C)
			{
				units.push_back({begin, end, stack.back().level, stack.back().override_direction, true});
				if (stack.size() > 1)
					stack.pop_back();
				continue;
			}
			int strong = 0;
			if (c >= 0x5D0 && c <= 0x5EA)
				strong = -1;
			else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
				strong = 1;
			if (stack.back().override_direction != 0)
				strong = stack.back().override_direction;
			units.push_back({begin, end, stack.back().level, strong, false});
		}
		// Neutrals: the direction of the strong characters on both sides (in the same embedding) when they agree.
		for (size_t i = 0; i < units.size(); i++)
		{
			if (units[i].strong != 0 || units[i].control)
				continue;
			const int embedding = (units[i].level % 2 == 0 ? 1 : -1);
			int before = embedding, after = embedding;
			for (size_t k = i; k-- > 0;)
			{
				if (units[k].level != units[i].level)
					break;
				if (units[k].strong != 0 && !units[k].control)
				{
					before = units[k].strong;
					break;
				}
			}
			for (size_t k = i + 1; k < units.size(); k++)
			{
				if (units[k].level != units[i].level)
					break;
				if (units[k].strong != 0 && !units[k].control)
				{
					after = units[k].strong;
					break;
				}
			}
			units[i].strong = (before == after ? before : embedding);
		}
		// Implicit levels.
		for (Unit& unit : units)
		{
			if (unit.control)
				continue;
			const bool odd = (unit.level % 2 == 1);
			if (!odd && unit.strong < 0)
				unit.level += 1;
			else if (odd && unit.strong > 0)
				unit.level += 1;
		}
		// Runs of one level in logical order, then reversed from the highest level to the lowest odd one (L2).
		Rml::Vector<Rml::TextBidiRun> logical;
		Rml::Vector<int> levels;
		for (const Unit& unit : units)
		{
			if (!logical.empty() && levels.back() == unit.level)
				logical.back().end = unit.end;
			else
			{
				logical.push_back({unit.begin, unit.end, unit.level % 2 == 1});
				levels.push_back(unit.level);
			}
		}
		int highest = 0, lowest_odd = 1000;
		for (int level : levels)
		{
			highest = std::max(highest, level);
			if (level % 2 == 1)
				lowest_odd = std::min(lowest_odd, level);
		}
		Rml::Vector<int> order(logical.size());
		for (size_t i = 0; i < order.size(); i++)
			order[i] = (int)i;
		for (int level = highest; level >= lowest_odd && level > 0; level--)
		{
			for (size_t i = 0; i < order.size();)
			{
				if (levels[order[i]] < level)
				{
					i++;
					continue;
				}
				size_t j = i;
				while (j < order.size() && levels[order[j]] >= level)
					j++;
				std::reverse(order.begin() + i, order.begin() + j);
				i = j;
			}
		}
		runs.clear();
		for (int index : order)
			runs.push_back(logical[index]);
		if (runs.empty())
			runs.push_back({0, (int)string.size(), base == 1});
	}

	int GetVersion(Rml::FontFaceHandle) override { return 1; }

	// every string GenerateString was given, in order, and its baseline origin (relative to what draws it)
	Rml::StringList generated;
	Rml::Vector<Rml::Vector2f> generated_positions;

private:
	Rml::UnorderedMap<int, Rml::FontMetrics> metrics;
};

// RmlUi set up with the fixed-advance font engine for the scope's lifetime; the tests shell is shut down first (its next
// use initialises it again with its own fonts).
struct FixedAdvanceFontScope {
	TestsSystemInterface system_interface;
	TestsRenderInterface render_interface;
	FixedAdvanceFontEngine font_engine;
	Rml::Context* context = nullptr;

	FixedAdvanceFontScope(Rml::Vector2i dimensions = Rml::Vector2i(800, 600))
	{
		TestsShell::ShutdownShell();
		Rml::SetSystemInterface(&system_interface);
		Rml::SetRenderInterface(&render_interface);
		Rml::SetFontEngineInterface(&font_engine);
		REQUIRE(Rml::Initialise());
		context = Rml::CreateContext("fixed-advance", dimensions);
		REQUIRE(context);
	}
	~FixedAdvanceFontScope() { Rml::Shutdown(); }

	// a document whose body holds 'body' (RML), with 'style' in its head and a 10px font
	Rml::ElementDocument* Load(const Rml::String& style, const Rml::String& body)
	{
		const Rml::String rml = "<rml><head><style>body { display: block; font-family: fixed; font-size: 10px; line-height: 10px; width: 800px; } "
								"div { display: block; } " +
			style + "</style></head><body>" + body + "</body></rml>";
		Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml);
		REQUIRE(document);
		document->Show();
		context->Update();
		context->Render();
		return document;
	}
};
