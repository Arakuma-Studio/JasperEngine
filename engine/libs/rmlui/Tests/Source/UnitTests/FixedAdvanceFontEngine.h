#pragma once

#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/StringUtilities.h>
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
		return c == 0x200B || c == 0x200C || c == 0x200D || c == 0x2060 || c == 0x00AD || c == 0xFEFF || c < 0x20;
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

	float Measure(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context, Rml::Vector<float>* advances)
	{
		const float size = (float)(int)handle;
		float width = 0.f;
		for (auto it = Rml::StringIteratorU8(string.begin(), string.begin(), string.end()); it; ++it)
		{
			const char32_t c = (char32_t)*it;
			float advance = 0.f;
			if (!IsZeroWidth(c))
			{
				advance = size + context.letter_spacing;
				if (c == ' ' || c == 0xA0)
					advance += context.word_spacing;
			}
			if (advances)
				advances->push_back(advance);
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
		Rml::Vector<float> advances;
		const float width = Measure(handle, string, context, &advances);
		const float size = (float)(int)handle;
		mesh_list.emplace_back();
		float x = position.x;
		for (float advance : advances)
		{
			if (advance > 0.f)
				Rml::MeshUtilities::GenerateQuad(mesh_list.back().mesh, Rml::Vector2f(x, position.y - 0.8f * size), Rml::Vector2f(size, size), colour);
			x += advance;
		}
		return (int)Rml::Math::Round(width);
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
