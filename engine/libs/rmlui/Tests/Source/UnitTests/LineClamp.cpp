#include "FixedAdvanceFontEngine.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Fork, phase 6, P6-L1: Chromium 153's legacy line clamping ('display: -webkit-box; -webkit-box-orient: vertical;
// -webkit-line-clamp: N'). The fixed-advance font engine is Ahem: every character 10px wide, ascent 8, descent 2, and the
// expected values are Chromium 153's for the same documents in Ahem 10px with 'line-height: 1' (measured with the repo's
// pinned headless shell: getBoundingClientRect, and the ellipsis's place in the screenshot).

namespace {

const String ellipsis = "\xe2\x80\xa6"; // U+2026

const String clamp_style = ".c { display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2; overflow: hidden; width: 55px; } "
						   ".ib { display: inline-block; width: 20px; height: 10px; } .flow-root { display: flow-root; } ";

// The lines of the text elements under the element 'id', in order.
StringList LinesOf(Element* element)
{
	StringList lines;
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		Element* child = element->GetChild(i);
		if (auto text = rmlui_dynamic_cast<ElementText*>(child))
		{
			for (const ElementText::Line& line : text->GetLines())
				lines.push_back(line.text);
		}
		else
		{
			const StringList child_lines = LinesOf(child);
			lines.insert(lines.end(), child_lines.begin(), child_lines.end());
		}
	}
	return lines;
}

Rectanglef BoxOf(ElementDocument* document, const String& id)
{
	Element* element = document->GetElementById(id);
	REQUIRE(element);
	return Rectanglef::FromPositionSize(element->GetAbsoluteOffset(BoxArea::Border), element->GetBox().GetSize(BoxArea::Border));
}

float HeightOf(ElementDocument* document, const String& id)
{
	return BoxOf(document, id).Height();
}

// The ellipses the last render drew: the baseline origin GenerateString was given (relative to what draws it).
Vector<Vector2f> Ellipses(const FixedAdvanceFontEngine& font_engine)
{
	Vector<Vector2f> result;
	for (size_t i = 0; i < font_engine.generated.size(); i++)
	{
		if (font_engine.generated[i] == ellipsis)
			result.push_back(font_engine.generated_positions[i]);
	}
	return result;
}

// The text element of 'element' (its first child that is one).
ElementText* TextOf(Element* element)
{
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		if (auto text = rmlui_dynamic_cast<ElementText*>(element->GetChild(i)))
			return text;
	}
	return nullptr;
}

// One clamp container in a document of its own: its id is 'c'. Returns the absolute x of the ellipsis drawn by the text
// element 'owner' (or by the container when 'owner' is null), or NaN when there is none.
struct Clamped {
	FixedAdvanceFontScope& scope;
	ElementDocument* document = nullptr;

	Clamped(FixedAdvanceFontScope& scope, const String& style, const String& body) : scope(scope)
	{
		scope.font_engine.generated.clear();
		scope.font_engine.generated_positions.clear();
		document = scope.Load(clamp_style + style, body);
	}
	~Clamped() { document->Close(); }

	Element* Get(const String& id)
	{
		Element* element = document->GetElementById(id);
		REQUIRE(element);
		return element;
	}
	size_t NumEllipses() const { return Ellipses(scope.font_engine).size(); }
	// the ellipsis's absolute baseline origin, drawn by the text of 'owner_id' or by the block 'owner_id'
	Vector2f EllipsisOf(const String& owner_id, bool block_drawn = false)
	{
		const Vector<Vector2f> ellipses = Ellipses(scope.font_engine);
		REQUIRE(ellipses.size() == 1);
		Element* owner = Get(owner_id);
		if (block_drawn)
			return owner->GetAbsoluteOffset(BoxArea::Border) + ellipses[0];
		ElementText* text = TextOf(owner);
		REQUIRE(text);
		return text->GetAbsoluteOffset() + ellipses[0];
	}
	void Update()
	{
		scope.font_engine.generated.clear();
		scope.font_engine.generated_positions.clear();
		scope.context->Update();
		scope.context->Render();
	}
};

} // namespace

TEST_CASE("LineClamp.parsing")
{
	FixedAdvanceFontScope scope;
	auto parses = [](const String& name, const String& value) {
		PropertyDictionary dictionary;
		return StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value);
	};
	// Chromium 153: none or an integer from 1
	CHECK(parses("-webkit-line-clamp", "none"));
	CHECK(parses("-webkit-line-clamp", "1"));
	CHECK(parses("-webkit-line-clamp", "12"));
	CHECK_FALSE(parses("-webkit-line-clamp", "0"));
	CHECK_FALSE(parses("-webkit-line-clamp", "-1"));
	CHECK_FALSE(parses("-webkit-line-clamp", "2.5"));
	CHECK_FALSE(parses("-webkit-line-clamp", "2px"));
	CHECK_FALSE(parses("-webkit-line-clamp", "auto"));
	// its computed keywords (compat writes the aliases inline-axis and block-axis as them)
	for (const char* value : {"horizontal", "vertical"})
		CHECK(parses("-webkit-box-orient", value));
	CHECK_FALSE(parses("-webkit-box-orient", "diagonal"));
	CHECK(parses("display", "-webkit-box"));
	CHECK(parses("display", "-webkit-inline-box"));
	// the standard shorthand and its longhands: Chromium 153 does not parse them either
	CHECK_FALSE(parses("line-clamp", "2"));
	CHECK_FALSE(parses("max-lines", "2"));
	CHECK_FALSE(parses("block-ellipsis", "auto"));
	CHECK_FALSE(parses("continue", "discard"));
}

TEST_CASE("LineClamp.computed_display")
{
	// Chromium's StyleAdjuster: a -webkit-box with a vertical orient and a line clamp is 'flow-root' (an inline one
	// 'inline-block') and clamps; any other -webkit-box is a flex container; the clamp alone does nothing.
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("",
		"<div id='clamped' style='display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 3'>x</div>"
		"<div id='block-axis' style='display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 1'>x</div>"
		"<span id='inline' style='display: -webkit-inline-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2'>x</span>"
		"<div id='horizontal' style='display: -webkit-box; -webkit-box-orient: horizontal; -webkit-line-clamp: 2'>x</div>"
		"<div id='no-clamp' style='display: -webkit-box; -webkit-box-orient: vertical'>x</div>"
		"<span id='inline-flex' style='display: -webkit-inline-box'>x</span>"
		"<div id='block' style='display: block; -webkit-box-orient: vertical; -webkit-line-clamp: 2'>x</div>");
	auto computed = [&](const String& id) -> const Style::ComputedValues& { return document->GetElementById(id)->GetComputedValues(); };

	CHECK(computed("clamped").display() == Style::Display::FlowRoot);
	CHECK(computed("clamped").line_clamp() == 3);
	CHECK(computed("block-axis").display() == Style::Display::FlowRoot);
	CHECK(computed("block-axis").line_clamp() == 1);
	CHECK(computed("inline").display() == Style::Display::InlineBlock);
	CHECK(computed("inline").line_clamp() == 2);
	CHECK(computed("horizontal").display() == Style::Display::Flex);
	CHECK(computed("horizontal").line_clamp() == 0);
	CHECK(computed("no-clamp").display() == Style::Display::Flex);
	CHECK(computed("inline-flex").display() == Style::Display::InlineFlex);
	CHECK(computed("block").display() == Style::Display::Block);
	CHECK(computed("block").line_clamp() == 0);
	CHECK(computed("block").webkit_line_clamp() == 2);
	document->Close();
}

TEST_CASE("LineClamp.text")
{
	FixedAdvanceFontScope scope;

	SUBCASE("ellipsis after the last line shown")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXX XXX XXX XXX XXX</div>");
		Element* c = clamped.Get("c");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		// every line is laid out (the scrollable overflow keeps them), the ellipsis follows the second one's text
		CHECK(LinesOf(c) == StringList{"XXX", "XXX", "XXX", "XXX", "XXX"});
		CHECK(c->GetScrollHeight() == 50.f);
		const Vector2f at = clamped.EllipsisOf("c");
		CHECK(at.x == 30.f);
		CHECK(at.y == 18.f); // the second line's baseline
	}
	SUBCASE("the line is cut to fit the ellipsis")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXXXX XXXXX XXX</div>");
		CHECK(LinesOf(clamped.Get("c")) == StringList{"XXXXX", "XXXX", "XXX"});
		CHECK(clamped.EllipsisOf("c").x == 40.f);
	}
	SUBCASE("cut at characters, not words")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XX XX XX XX XX XX</div>");
		CHECK(LinesOf(clamped.Get("c")) == StringList{"XX XX", "XX X", "XX XX"});
		CHECK(clamped.EllipsisOf("c").x == 40.f);
	}
	SUBCASE("fewer lines: no ellipsis")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 10.f);
		CHECK(clamped.NumEllipses() == 0);
	}
	SUBCASE("exactly the clamped lines: laid out again without clamping, no ellipsis")
	{
		Clamped clamped(scope, "", "<div class='c' id='c' style='-webkit-line-clamp: 3'>XXX XXX XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 30.f);
		CHECK(clamped.NumEllipses() == 0);
		CHECK(LinesOf(clamped.Get("c")) == StringList{"XXX", "XXX", "XXX"});
	}
	SUBCASE("an empty block after the clamp line is content after it: the ellipsis stays")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXX XXX<div id='empty'></div></div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		CHECK(clamped.EllipsisOf("c").x == 30.f);
	}
	SUBCASE("an empty inline box on the clamp line is not")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXX XXX<span></span></div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		CHECK(clamped.NumEllipses() == 0);
	}
	SUBCASE("overflow visible: the lines after the clamp point overflow, painted")
	{
		Clamped clamped(scope, "", "<div class='c' id='c' style='overflow: visible'>XXX XXX XXX XXX XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		CHECK(clamped.EllipsisOf("c").x == 30.f);
	}
	SUBCASE("a display other than -webkit-box does not clamp")
	{
		Clamped clamped(scope, "", "<div class='c' id='c' style='display: block'>XXX XXX XXX XXX XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 50.f);
		CHECK(clamped.NumEllipses() == 0);
	}
	SUBCASE("padding and borders are outside the clamped lines")
	{
		Clamped clamped(scope, "", "<div class='c' id='c' style='padding: 3px; border: 2px solid #f00'>XXX XXX XXX XXX XXX</div>");
		CHECK(BoxOf(clamped.document, "c").Size() == Vector2f(65.f, 30.f));
		CHECK(clamped.EllipsisOf("c").x - BoxOf(clamped.document, "c").Left() == 35.f);
	}
	SUBCASE("a definite height wins; the lines it shows after the clamp line have no ellipsis")
	{
		Clamped clamped(scope, "", "<div class='c' id='c' style='height: 40px'>XXX XXX XXX XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 40.f);
		CHECK(clamped.EllipsisOf("c").y == 18.f);
	}
	SUBCASE("max-height and min-height clamp the clamped height")
	{
		{
			Clamped clamped(scope, "", "<div class='c' id='c' style='max-height: 15px'>XXX XXX XXX XXX</div>");
			CHECK(HeightOf(clamped.document, "c") == 15.f);
		}
		{
			Clamped clamped(scope, "", "<div class='c' id='c' style='min-height: 35px'>XXX XXX XXX XXX</div>");
			CHECK(HeightOf(clamped.document, "c") == 35.f);
		}
	}
	SUBCASE("white-space: pre lines and <br>")
	{
		{
			Clamped clamped(scope, "", "<div class='c' id='c' style='white-space: pre'>X\nXX\nXXX</div>");
			CHECK(HeightOf(clamped.document, "c") == 20.f);
			CHECK(clamped.EllipsisOf("c").x == 20.f);
		}
		{
			Clamped clamped(scope, "", "<div class='c' id='c'>XXX<br/>XXX<br/>XXX</div>");
			CHECK(HeightOf(clamped.document, "c") == 20.f);
			CHECK(clamped.NumEllipses() == 1);
		}
	}
	SUBCASE("a line of nothing but a <br> counts: the ellipsis alone, drawn by the block")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXX<br/><br/>XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		const Vector2f at = clamped.EllipsisOf("c", true);
		CHECK(at.x == BoxOf(clamped.document, "c").Left());
		CHECK(at.y - BoxOf(clamped.document, "c").Top() == 18.f);
	}
	SUBCASE("nowrap: one line, nothing after it, no clamp ellipsis; text-overflow ellipsizes it")
	{
		{
			Clamped clamped(scope, "", "<div class='c' id='c' style='white-space: nowrap; -webkit-line-clamp: 1'>XXXXXXXXXX</div>");
			CHECK(HeightOf(clamped.document, "c") == 10.f);
			CHECK(clamped.NumEllipses() == 0);
		}
		{
			Clamped clamped(scope, "",
				"<div class='c' id='c' style='white-space: nowrap; -webkit-line-clamp: 1; text-overflow: ellipsis'>XXXXXXXXXX</div>");
			CHECK(clamped.NumEllipses() == 1);
		}
	}
	SUBCASE("text-overflow ellipsizes the other lines, the clamp line keeps its ellipsis")
	{
		Clamped clamped(scope, "", "<div class='c' id='c' style='text-overflow: ellipsis'>XXXXXXXXXX XXX XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		CHECK(clamped.NumEllipses() == 2);
	}
	SUBCASE("text-align: the offset of the uncut line; the ellipsis may end outside (clipped)")
	{
		{
			Clamped clamped(scope, "", "<div class='c' id='c' style='text-align: center'>XXX XXX XXX</div>");
			CHECK(clamped.EllipsisOf("c").x == 42.5f);
		}
		{
			Clamped clamped(scope, "", "<div class='c' id='c' style='text-align: right'>XXX XXX XXX</div>");
			CHECK(clamped.EllipsisOf("c").x == 55.f);
		}
	}
	SUBCASE("right to left: the ellipsis on the left, the line's end cut on its left")
	{
		{
			Clamped clamped(scope, "", "<div class='c' id='c' style='-rmlui-direction: rtl'>XXX XXX XXX XXX</div>");
			CHECK(HeightOf(clamped.document, "c") == 20.f);
			CHECK(clamped.EllipsisOf("c").x == 15.f);
		}
		{
			Clamped clamped(scope, "", "<div class='c' id='c' style='-rmlui-direction: rtl'>XXXXX XXXXX XXX</div>");
			CHECK(LinesOf(clamped.Get("c")) == StringList{"XXXXX", "XXXX", "XXX"});
			CHECK(clamped.EllipsisOf("c").x == 5.f);
			ElementText* text = TextOf(clamped.Get("c"));
			REQUIRE(text->GetLines().size() == 3);
			CHECK(text->GetAbsoluteOffset().x + text->GetLines()[1].position.x == 15.f);
		}
		{
			// a line too long for the box is start-aligned: it overflows on its left; its visual left is cut
			Clamped clamped(scope, "", "<div class='c' id='c' style='-rmlui-direction: rtl'>XX XpXpXX XXX</div>");
			CHECK(LinesOf(clamped.Get("c")) == StringList{"XX", "XpXX", "XXX"});
			CHECK(clamped.EllipsisOf("c").x == 5.f);
			ElementText* text = TextOf(clamped.Get("c"));
			CHECK(text->GetAbsoluteOffset().x + text->GetLines()[1].position.x == 15.f);
			CHECK(text->GetAbsoluteOffset().x + text->GetLines()[2].position.x == 25.f);
		}
		{
			// right-to-left text (Hebrew letters are 10px wide here too): its logical end is on the left, cut there
			const String alef = "\xd7\x90", bet = "\xd7\x91";
			Clamped clamped(scope, "", "<div class='c' id='c' style='-rmlui-direction: rtl'>" + alef + alef + alef + " " + alef + bet + alef + bet +
				alef + " " + alef + "</div>");
			CHECK(LinesOf(clamped.Get("c")) == StringList{alef + alef + alef, alef + bet + alef + bet, alef});
		}
	}
}

TEST_CASE("LineClamp.inline_content")
{
	FixedAdvanceFontScope scope;

	SUBCASE("a span: the ellipsis follows its text, over its right edges")
	{
		Clamped clamped(scope, "",
			"<div class='c' id='c'>XXX <span id='s' style='padding: 0 4px; border-right: 3px #0f0'>XXX</span> XXX</div>");
		CHECK(clamped.EllipsisOf("s").x == 34.f);
	}
	SUBCASE("lines of a span split across the clamp point")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'><span id='s'>XXX XXX XXX</span> XXX XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		// its boxes span the three lines (getBoundingClientRect: 30px tall); the one open at the clamp line's end covers the
		// ellipsis (Chromium appends the ellipsis to the line: 40px wide)
		CHECK(clamped.Get("s")->GetNumBoxes() == 3);
		Vector2f offset;
		CHECK(clamped.Get("s")->GetBox(1, offset).GetSize(BoxArea::Border).x == 40.f);
		CHECK(clamped.EllipsisOf("s").x == 30.f);
	}
	SUBCASE("an atomic inline that fits: the ellipsis after it, drawn by the block")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXX XX<span class='ib' id='ib'></span> XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 22.f);
		CHECK(clamped.EllipsisOf("c", true).x == 40.f);
		CHECK(scope.context->GetElementAtPoint(BoxOf(clamped.document, "ib").Center()) == clamped.Get("ib"));
	}
	SUBCASE("an atomic inline that does not fit is hidden, not hit: the ellipsis after the text before it")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXX XXX<span class='ib' id='ib'></span> XXX</div>");
		CHECK(BoxOf(clamped.document, "ib").Left() - BoxOf(clamped.document, "c").Left() == 30.f);
		CHECK(scope.context->GetElementAtPoint(BoxOf(clamped.document, "ib").Center()) == clamped.Get("c"));
		CHECK(TextOf(clamped.Get("c")) != nullptr);
		const Vector<Vector2f> ellipses = Ellipses(scope.font_engine);
		REQUIRE(ellipses.size() == 1);
	}
	SUBCASE("an inline-block's own lines do not count")
	{
		Clamped clamped(scope, "",
			"<div class='c' id='c'>XXX <span id='ib' style='display: inline-block; width: 30px'>XXX XXX</span> XXX XXX</div>");
		CHECK(HeightOf(clamped.document, "c") == 30.f);
		CHECK(clamped.EllipsisOf("c", true).x - BoxOf(clamped.document, "c").Left() == 30.f);
	}
}

TEST_CASE("LineClamp.indented")
{
	// 'text-indent' (P6-J2) on the clamp line: the ellipsis follows what fits in the width the indent leaves (Chromium's
	// values, the browser-parity fixture p3/text/line-clamp-indent).
	FixedAdvanceFontScope scope;
	const String indented = ".i { width: 75px; -webkit-line-clamp: 1; text-indent: 20px; } ";

	SUBCASE("the indented first line")
	{
		Clamped clamped(scope, indented, "<div class='c i' id='c'>XpX XX XXX</div>");
		CHECK(clamped.EllipsisOf("c").x == 50.f);
	}
	SUBCASE("cut beside the indent")
	{
		Clamped clamped(scope, indented, "<div class='c i' id='c'>XpXpXpXpX XX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XpXp");
		CHECK(clamped.EllipsisOf("c").x == 60.f);
	}
	SUBCASE("hanging: the second line is indented")
	{
		Clamped clamped(scope, indented, "<div class='c i' id='c' style='-webkit-line-clamp: 2; text-indent: 20px hanging'>XpX XX XXX XpX</div>");
		CHECK(clamped.EllipsisOf("c").x == 50.f);
	}
	SUBCASE("right to left: the line ends before the indent")
	{
		Clamped clamped(scope, indented, "<div class='c i' id='c' style='-rmlui-direction: rtl'>XpX XX XXX</div>");
		CHECK(clamped.EllipsisOf("c").x == 15.f);
	}
	SUBCASE("justified in what the indent leaves, then cut")
	{
		Clamped clamped(scope, indented, "<div class='c i' id='c' style='text-indent: 15px; text-align: justify'>XpX X XXXX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XpX ");
		CHECK(clamped.EllipsisOf("c").x == 65.f);
	}
	SUBCASE("a line of nothing but a <br>: the ellipsis at the indent")
	{
		Clamped clamped(scope, indented, "<div class='c i' id='c'><br/>XX</div>");
		CHECK(clamped.EllipsisOf("c", true).x - BoxOf(clamped.document, "c").Left() == 20.f);
	}
}

TEST_CASE("LineClamp.justified")
{
	// Chromium justifies the clamp line (P6-J1) before its LineTruncator runs: the ellipsis takes the place of what no
	// longer fits on the justified line, a space keeping its share (the browser-parity fixture p3/text/line-clamp-justify).
	FixedAdvanceFontScope scope;
	const String justified = ".j { width: 75px; text-align: justify; } ";

	SUBCASE("the justified line is cut: the ellipsis after the characters that fit with their space")
	{
		Clamped clamped(scope, justified, "<div class='c j' id='c' style='-webkit-line-clamp: 1'>XpX XX XXX XpX</div>");
		CHECK(LinesOf(clamped.Get("c")) == StringList{"XpX X", "XXX XpX"});
		CHECK(clamped.EllipsisOf("c").x == 65.f);
	}
	SUBCASE("a kept space keeps its share")
	{
		Clamped clamped(scope, justified, "<div class='c j' id='c'>XpX XX XXXX X XpX</div>");
		CHECK(LinesOf(clamped.Get("c")) == StringList{"XpX XX", "XXXX ", "XpX"});
		CHECK(clamped.EllipsisOf("c") == Vector2f(65.f, 18.f));
		ElementText* text = TextOf(clamped.Get("c"));
		REQUIRE(text->GetLines().size() == 3);
		CHECK(text->GetJustification(text->GetLines()[1]).size() == 1);
	}
	SUBCASE("inter-character")
	{
		Clamped clamped(scope, justified, "<div class='c j' id='c' style='-webkit-line-clamp: 1; text-justify: inter-character'>XpXXpX XX XX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XpXXp");
		CHECK(clamped.EllipsisOf("c").x == 65.f);
	}
	SUBCASE("the clamp line that ends a paragraph: text-align-last justifies it, then it is cut")
	{
		Clamped clamped(scope, ".j { width: 75px; text-align-last: justify; } ",
			"<div class='c j' id='c'><div id='p'>XpX XX XXX XpX</div><div>XX XX</div></div>");
		CHECK(LinesOf(clamped.Get("p")) == StringList{"XpX XX", "XXX Xp"});
		CHECK(clamped.EllipsisOf("p").x == 65.f);
	}
	SUBCASE("right to left: the kept part stays where justification placed it")
	{
		Clamped clamped(scope, justified, "<div class='c j' id='c' style='-rmlui-direction: rtl'>XpX XX XXX Xp XXX XX</div>");
		CHECK(LinesOf(clamped.Get("c")) == StringList{"XpX XX", "XX Xp", "XXX XX"});
		CHECK(clamped.EllipsisOf("c").x == 0.f);
		ElementText* text = TextOf(clamped.Get("c"));
		CHECK(text->GetAbsoluteOffset().x + text->GetLines()[1].position.x == 10.f);
	}
}

TEST_CASE("LineClamp.blocks")
{
	FixedAdvanceFontScope scope;

	SUBCASE("lines are counted across nested blocks; blocks after the clamp point are as tall as their first line")
	{
		for (int lines : {1, 2, 3, 4})
		{
			Clamped clamped(scope, ".p { display: block; }",
				"<div class='c' id='c' style='-webkit-line-clamp: " + ToString(lines) +
					"'><div class='p' id='p1'>XXX XXX</div><div class='p' id='p2'>XXX XXX XXX</div><div class='p' id='p3'>XXX XXX</div></div>");
			const float top = BoxOf(clamped.document, "c").Top();
			auto rel = [&](const String& id) { return BoxOf(clamped.document, id).Top() - top; };
			CAPTURE(lines);
			switch (lines)
			{
			case 1:
				CHECK(HeightOf(clamped.document, "c") == 10.f);
				CHECK(HeightOf(clamped.document, "p1") == 10.f);
				CHECK(rel("p2") == 10.f);
				CHECK(HeightOf(clamped.document, "p2") == 10.f);
				CHECK(rel("p3") == 20.f);
				CHECK(HeightOf(clamped.document, "p3") == 10.f);
				break;
			case 2:
				CHECK(HeightOf(clamped.document, "c") == 20.f);
				CHECK(HeightOf(clamped.document, "p1") == 20.f);
				CHECK(HeightOf(clamped.document, "p2") == 10.f);
				CHECK(rel("p3") == 30.f);
				break;
			case 3:
				CHECK(HeightOf(clamped.document, "c") == 30.f);
				CHECK(HeightOf(clamped.document, "p2") == 10.f);
				CHECK(rel("p3") == 30.f);
				CHECK(HeightOf(clamped.document, "p3") == 10.f);
				break;
			case 4:
				CHECK(HeightOf(clamped.document, "c") == 40.f);
				CHECK(HeightOf(clamped.document, "p2") == 20.f);
				CHECK(rel("p3") == 40.f);
				break;
			}
			CHECK(clamped.NumEllipses() == 1);
		}
	}
	SUBCASE("margins after the clamp point are not part of the height")
	{
		Clamped clamped(scope, ".p { display: block; margin: 3px 0; }",
			"<div class='c' id='c'><div class='p' id='p1'>XXX XXX</div><div class='p' id='p2'>XXX XXX</div><div id='p3'>XXX</div></div>");
		const float top = BoxOf(clamped.document, "c").Top();
		CHECK(HeightOf(clamped.document, "c") == 23.f);
		CHECK(BoxOf(clamped.document, "p1").Top() - top == 3.f);
		CHECK(BoxOf(clamped.document, "p2").Top() - top == 26.f);
		CHECK(HeightOf(clamped.document, "p2") == 10.f);
		CHECK(clamped.EllipsisOf("p1").x == 30.f);
	}
	SUBCASE("a definite height and an empty block after the clamp point")
	{
		Clamped clamped(scope, ".p { display: block; }",
			"<div class='c' id='c'><div class='p' id='p1'>XXX XXX</div><div class='p' id='p2' style='height: 30px'>XXX</div>"
			"<div class='p' id='p3'>XXX XXX</div><div class='p' id='p4'></div></div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		CHECK(HeightOf(clamped.document, "p2") == 30.f);
		CHECK(HeightOf(clamped.document, "p3") == 10.f);
		CHECK(HeightOf(clamped.document, "p4") == 0.f);
	}
	SUBCASE("a block that establishes its own formatting context: its lines are not counted")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'>XXX<div class='flow-root' id='fr'>XXX XXX XXX</div>XXX XXX</div>");
		CHECK(HeightOf(clamped.document, "fr") == 30.f);
		CHECK(HeightOf(clamped.document, "c") == 50.f);
		CHECK(clamped.NumEllipses() == 1);
	}
	SUBCASE("floats below the clamp point are not enclosed")
	{
		Clamped clamped(scope, "", "<div class='c' id='c'><div style='float: left; width: 20px; height: 35px'></div>XX XX XX XX XX XX</div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
	}
}

TEST_CASE("LineClamp.dynamic")
{
	FixedAdvanceFontScope scope;
	Clamped clamped(scope, "", "<div class='c' id='c'>XXX XXX XXX XXX XXX</div>");
	Element* c = clamped.Get("c");
	CHECK(HeightOf(clamped.document, "c") == 20.f);

	c->SetProperty("-webkit-line-clamp", "3");
	clamped.Update();
	CHECK(HeightOf(clamped.document, "c") == 30.f);
	CHECK(clamped.EllipsisOf("c").y == 28.f);

	c->SetProperty("-webkit-line-clamp", "none");
	clamped.Update();
	CHECK(c->GetComputedValues().display() == Style::Display::Flex);
	CHECK(clamped.NumEllipses() == 0);

	c->SetProperty("-webkit-line-clamp", "1");
	clamped.Update();
	CHECK(c->GetComputedValues().display() == Style::Display::FlowRoot);
	CHECK(HeightOf(clamped.document, "c") == 10.f);
	CHECK(clamped.EllipsisOf("c").y == 8.f);

	// a <br> line's ellipsis goes when the display changes
	c->SetInnerRML("XXX<br/><br/>XXX");
	c->SetProperty("-webkit-line-clamp", "2");
	clamped.Update();
	CHECK(clamped.NumEllipses() == 1);
	c->SetProperty("display", "block");
	clamped.Update();
	CHECK(clamped.NumEllipses() == 0);
	CHECK(HeightOf(clamped.document, "c") == 30.f);
}

TEST_CASE("LineClamp.containers")
{
	FixedAdvanceFontScope scope;

	SUBCASE("a flex item: blockified, clamped")
	{
		Clamped clamped(scope, "", "<div style='display: flex; align-items: flex-start'><div class='c' id='c'>XXX XXX XXX XXX</div><div id='other' style='height: 50px; width: 10px'></div></div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		CHECK(clamped.NumEllipses() == 1);
	}
	SUBCASE("a stretched flex item: as tall as the line, the clamp is unchanged")
	{
		Clamped clamped(scope, "", "<div style='display: flex'><div class='c' id='c'>XXX XXX XXX XXX</div><div style='height: 50px; width: 10px'></div></div>");
		CHECK(HeightOf(clamped.document, "c") == 50.f);
		CHECK(clamped.NumEllipses() == 1);
	}
	SUBCASE("a grid item")
	{
		Clamped clamped(scope, "", "<div style='display: grid; grid-template-columns: 55px; align-items: start'><div class='c' id='c'>XXX XXX XXX XXX</div></div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		CHECK(clamped.NumEllipses() == 1);
	}
	SUBCASE("-webkit-inline-box: an inline-block, its baseline the clamp line's (overflow visible)")
	{
		Clamped clamped(scope, "",
			"<div id='line'>X<span class='c' id='c' style='display: -webkit-inline-box; overflow: visible'>XXX XXX XXX XXX</span></div>");
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		// the line's baseline is the inline-block's second line's, not its fourth: the line is as tall as the clamped box
		CHECK(HeightOf(clamped.document, "line") == 20.f);
	}
	SUBCASE("-webkit-inline-box with overflow hidden: its baseline is its bottom margin edge")
	{
		Clamped clamped(scope, "", "<div id='line'>X<span class='c' id='c' style='display: -webkit-inline-box'>XXX XXX XXX XXX</span></div>");
		CHECK(HeightOf(clamped.document, "line") == 22.f);
	}
	SUBCASE("baseline alignment of a flex item: its first line's")
	{
		Clamped clamped(scope, "",
			"<div style='display: flex; align-items: baseline'><div class='c' id='c' style='overflow: visible'>XXX XXX XXX XXX</div>"
			"<div id='o' style='width: 10px'>X</div></div>");
		CHECK(BoxOf(clamped.document, "o").Top() == BoxOf(clamped.document, "c").Top());
	}
	SUBCASE("a flex-grown item: the clamp counts its lines at its flexed width")
	{
		Clamped clamped(scope, "", "<div style='display: flex'><div class='c' id='c' style='flex: 1; width: auto'>XXX XXX XXX XXX</div></div>");
		CHECK(HeightOf(clamped.document, "c") == 10.f);
		CHECK(clamped.NumEllipses() == 0);
	}
}

TEST_CASE("LineClamp.transition_is_discrete")
{
	FixedAdvanceFontScope scope;
	scope.system_interface.SetManualTime(0.0);

	SUBCASE("without allow-discrete: no transition")
	{
		Clamped clamped(scope, "", "<div class='c' id='c' style='transition: -webkit-line-clamp 1s linear'>XXX XXX XXX XXX XXX</div>");
		clamped.Get("c")->SetProperty("-webkit-line-clamp", "4");
		clamped.Update();
		CHECK(HeightOf(clamped.document, "c") == 40.f);
	}
	SUBCASE("with allow-discrete: the new value from half the duration")
	{
		Clamped clamped(scope, "", "<div class='c' id='c' style='transition: -webkit-line-clamp 1s linear allow-discrete'>XXX XXX XXX XXX XXX</div>");
		clamped.Get("c")->SetProperty("-webkit-line-clamp", "4");
		clamped.Update();
		scope.system_interface.SetManualTime(0.4);
		clamped.Update();
		CHECK(HeightOf(clamped.document, "c") == 20.f);
		scope.system_interface.SetManualTime(0.6);
		clamped.Update();
		CHECK(HeightOf(clamped.document, "c") == 40.f);
	}
}

TEST_CASE("LineClamp.grapheme_clusters")
{
	// The ellipsis cuts between extended grapheme clusters (UAX #29), as Chromium (the browser-parity fixture
	// p3/text/line-clamp-graphemes, the characters Ahem's .notdef boxes, one em each).
	FixedAdvanceFontScope scope;
	const String one = ".g { width: 75px; -webkit-line-clamp: 1; } ";
	SUBCASE("an emoji with a skin-tone modifier")
	{
		Clamped clamped(scope, one, "<div class='c g' id='c'>XpX\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbdXX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XpX\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd");
		CHECK(clamped.EllipsisOf("c").x == 50.f);
	}
	SUBCASE("a flag: two regional indicators")
	{
		Clamped clamped(scope, one, "<div class='c g' id='c'>XpXXX\xf0\x9f\x87\xab\xf0\x9f\x87\xb7\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa XX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XpXXX");
		CHECK(clamped.EllipsisOf("c").x == 50.f);
	}
	SUBCASE("an Indic conjunct: KA VIRAMA SSA")
	{
		Clamped clamped(scope, one, "<div class='c g' id='c'>XpXXX\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7XX XX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XpXXX");
		CHECK(clamped.EllipsisOf("c").x == 50.f);
	}
}

TEST_CASE("LineClamp.bidi")
{
	// The runs of the clamp line as the font engine lays them out (UAX #9), cut from the line's end as Chromium cuts a line's
	// items: a run against the line's direction keeps its end (the browser-parity fixture p3/text/line-clamp-bidi).
	FixedAdvanceFontScope scope;
	const String one = ".b { width: 75px; -webkit-line-clamp: 1; } ";
	const String rlo = "\xe2\x80\xae", pdf = "\xe2\x80\xac";
	const String alef = "\xd7\x90", bet = "\xd7\x91", gimel = "\xd7\x92";
	SUBCASE("left to right: a right-to-left run at the end keeps its logical end, its visual left")
	{
		Clamped clamped(scope, one, "<div class='c b' id='c'>XX " + rlo + "XXpp" + pdf + " XpX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XX " + rlo + "Xpp" + pdf);
		CHECK(clamped.EllipsisOf("c").x == 60.f);
	}
	SUBCASE("left to right: Hebrew at the end")
	{
		Clamped clamped(scope, one, "<div class='c b' id='c'>XpX " + alef + bet + gimel + " XX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XpX " + bet + gimel);
		CHECK(clamped.EllipsisOf("c").x == 60.f);
	}
	SUBCASE("right to left: a right-to-left run on the left keeps its logical start, the kept part stays at the right")
	{
		Clamped clamped(scope, one, "<div class='c b' id='c' style='-rmlui-direction: rtl'>XXp " + rlo + "Xpp" + pdf + " XX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == "XXp " + rlo + "Xp" + pdf);
		CHECK(clamped.EllipsisOf("c").x == 5.f);
	}
	SUBCASE("right to left: a left-to-right run on the left keeps its logical end")
	{
		Clamped clamped(scope, one, "<div class='c b' id='c' style='-rmlui-direction: rtl'>" + alef + bet + " XXpp XX</div>");
		CHECK(LinesOf(clamped.Get("c")).front() == alef + bet + " Xpp");
		CHECK(clamped.EllipsisOf("c").x == 5.f);
	}
}

TEST_CASE("LineClamp.ellipsis_style")
{
	// Chromium paints the ellipsis with the line's style: the block container's text shadows (not the cut run's), and no
	// text decoration of the block's or the run's (the browser-parity fixture p3/text/line-clamp-style).
	FixedAdvanceFontScope scope;
	const String one = ".s { width: 75px; -webkit-line-clamp: 1; } ";
	const auto ellipses = [&]() {
		Vector<Vector2f> result;
		for (size_t i = 0; i < scope.font_engine.generated.size(); i++)
			if (scope.font_engine.generated[i] == ellipsis)
				result.push_back(scope.font_engine.generated_positions[i]);
		return result;
	};
	SUBCASE("the block's shadow, offset")
	{
		Clamped clamped(scope, one, "<div class='c s' id='c' style='text-shadow: 3px 2px 0 #0a0'><span style='text-shadow: none'>XpX XX XXX</span></div>");
		const Vector<Vector2f> drawn = ellipses();
		REQUIRE(drawn.size() == 2);
		CHECK(drawn[1] - drawn[0] == Vector2f(3.f, 2.f)); // (the glyphs are generated first)
	}
	SUBCASE("the cut run's shadow is not the ellipsis's")
	{
		Clamped clamped(scope, one, "<div class='c s' id='c'><span style='text-shadow: 3px 2px 0 #00d'>XpX XX XXX</span></div>");
		CHECK(ellipses().size() == 1);
	}
	SUBCASE("the block's shadow on an ellipsis the block draws (after an inline-block)")
	{
		Clamped clamped(scope, one + ".ib10 { display: inline-block; width: 10px; height: 10px; } ",
			"<div class='c s' id='c' style='text-shadow: 3px 2px 0 #0a0'><span style='text-shadow: none'>XpX X</span><span class='ib10'></span> XX</div>");
		const Vector<Vector2f> drawn = ellipses();
		REQUIRE(drawn.size() == 2);
		CHECK(drawn[1] - drawn[0] == Vector2f(3.f, 2.f)); // (the glyphs are generated first)
	}
}

TEST_CASE("LineClamp.paint_order")
{
	// An ellipsis the block container draws is painted after the atomic inline it follows and its content (Chromium paints
	// the line's items in order), before the text after it in the block flow: the order the geometry is generated in on the
	// first render (the browser-parity fixture p3/text/line-clamp-paint-order).
	FixedAdvanceFontScope scope;
	Clamped clamped(scope, ".o { width: 75px; -webkit-line-clamp: 1; } .ibq { display: inline-block; width: 10px; } ",
		"<div class='c o' id='c'><div>XpX X<span class='ibq'>Q</span> XX</div><div>RR</div></div>");
	const StringList& generated = scope.font_engine.generated;
	const auto index_of = [&](const String& value) { return (int)(std::find(generated.begin(), generated.end(), value) - generated.begin()); };
	REQUIRE(index_of(ellipsis) < (int)generated.size());
	CHECK(index_of("XpX X") < index_of(ellipsis));
	CHECK(index_of("Q") < index_of(ellipsis));
	CHECK(index_of(ellipsis) < index_of("RR"));
}
