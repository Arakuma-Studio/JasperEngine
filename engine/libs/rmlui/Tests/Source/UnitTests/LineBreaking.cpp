#include "../../../Source/Core/Layout/LayoutDetails.h"
#include "../../../Source/Core/TextUnicode.h"
#include "FixedAdvanceFontEngine.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-text (docs/html-ui/phase-3-fork-patches.md 3.10): line-break opportunities inside words (CSS Text 3 §5,
// UAX #14 subset) with a font engine of fixed advances: every character 10px wide, zero-width characters 0px.

namespace {

// The lines of the text element under the element 'id'.
StringList LinesOf(ElementDocument* document, const String& id)
{
	Element* element = document->GetElementById(id);
	REQUIRE(element);
	StringList lines;
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		if (auto text = rmlui_dynamic_cast<ElementText*>(element->GetChild(i)))
		{
			for (const ElementText::Line& line : text->GetLines())
				lines.push_back(line.text);
		}
	}
	return lines;
}

float WidthOf(ElementDocument* document, const String& id)
{
	return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border).x;
}

const String zwsp = "\xe2\x80\x8b";  // U+200B
const String shy = "\xc2\xad";       // U+00AD
const String wj = "\xe2\x81\xa0";    // U+2060 word joiner
const String nbsp = "\xc2\xa0";      // U+00A0
const String zhong = "\xe4\xb8\xad";  // U+4E2D
const String wen = "\xe6\x96\x87";    // U+6587
const String zi = "\xe5\xad\x97";     // U+5B57
const String full_stop = "\xe3\x80\x82"; // U+3002

} // namespace

TEST_CASE("LineBreaking.pair_rules")
{
	// (a text of two characters, tailored as Chromium)
	auto opportunity = [](char32_t a, char32_t b, Style::WordBreak word_break = Style::WordBreak::Normal) {
		const Vector<Character> text{Character(a), Character(b)};
		Vector<bool> breaks;
		TextUnicode::GetLineBreakOpportunities(text, word_break, TextUnicode::LineBreakTailoring::Chromium, breaks);
		return bool(breaks[1]);
	};
	// after a zero-width space, a soft hyphen, a hyphen, a dash
	CHECK(opportunity(0x200B, 'a'));
	CHECK(opportunity(0x00AD, 'a'));
	CHECK(opportunity('-', 'a'));
	CHECK(opportunity(0x2013, 'a'));
	CHECK(opportunity(0x2014, 'a'));
	CHECK(opportunity('a', 0x2014));
	// not inside words, before a hyphen, before a number after a hyphen
	CHECK_FALSE(opportunity('a', 'b'));
	CHECK_FALSE(opportunity('a', '-'));
	CHECK_FALSE(opportunity('-', '5'));
	CHECK_FALSE(opportunity('a', 0x200B));
	// ideographs: around them, not before closing punctuation or after opening, not with keep-all
	CHECK(opportunity(0x4E2D, 0x6587));
	CHECK(opportunity('a', 0x4E2D));
	CHECK(opportunity(0x3042, 0x3044));
	CHECK_FALSE(opportunity(0x4E2D, 0x3002));
	CHECK_FALSE(opportunity(0x300C, 0x4E2D));
	CHECK_FALSE(opportunity(0x4E2D, 0x6587, Style::WordBreak::KeepAll));
	CHECK_FALSE(opportunity(0xD55C, 0xAE00, Style::WordBreak::KeepAll));
	CHECK(opportunity(0xD55C, 0xAE00));
	// joiners, glue, combining marks, emoji sequences
	CHECK_FALSE(opportunity('a', 0x2060));
	CHECK_FALSE(opportunity(0x2060, 0x4E2D));
	CHECK_FALSE(opportunity(0x00A0, 0x4E2D));
	CHECK_FALSE(opportunity(0x4E2D, 0x0301));
	CHECK_FALSE(opportunity(0x200D, 0x1F600));
	CHECK_FALSE(opportunity(0x1F466, 0x1F3FB));
	CHECK(opportunity(0x1F600, 0x1F600));
	// punctuation: not before '!', ',' or ')', not after '('
	CHECK_FALSE(opportunity(0x4E2D, '!'));
	CHECK_FALSE(opportunity(0x4E2D, ','));
	CHECK_FALSE(opportunity('(', 0x4E2D));
}

TEST_CASE("LineBreaking.zero_width_space")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load(".measure { display: inline-block; } .flex { display: flex; }",
		"<div id='narrow' style='width: 0'>HHH" + zwsp + "HH" + zwsp + "H</div>"                   //
			+ "<div id='wide' style='width: 100px'>HHH" + zwsp + "HH" + zwsp + "H</div>"            //
			+ "<div id='nowrap' style='width: 0; white-space: nowrap'>HH" + zwsp + "HH</div>"       //
			+ "<div><span class='measure' id='inline-block'>HH" + zwsp + "HHH</span></div>"         //
			+ "<div class='flex' style='width: 0'><div id='flex-item'>HH" + zwsp + "HH</div></div>" //
			+ "<div class='flex'><div id='flex-max'>HH" + zwsp + "HH</div></div>");

	// a line-break opportunity, invisible and zero-width
	CHECK(LinesOf(document, "narrow") == StringList{"HHH" + zwsp, "HH" + zwsp, "H"});
	CHECK(document->GetElementById("narrow")->GetBox().GetSize().y == 30.f);
	CHECK(LinesOf(document, "wide") == StringList{"HHH" + zwsp + "HH" + zwsp + "H"});
	CHECK(LinesOf(document, "nowrap") == StringList{"HH" + zwsp + "HH"});

	// intrinsic sizes: the widest piece, the whole line
	CHECK(WidthOf(document, "inline-block") == 50.f);
	CHECK(LayoutDetails::GetMinContentWidth(document->GetElementById("inline-block"), Vector2f(800.f, 600.f)) == 30.f);
	CHECK(WidthOf(document, "flex-max") == 40.f);

	document->Close();
}

TEST_CASE("LineBreaking.opportunities_inside_words")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("",
		String("<div id='hyphen' style='width: 50px'>foo-bar</div>")                                //
			+ "<div id='minus' style='width: 20px'>x-5</div>"                                 //
			+ "<div id='sign' style='width: 20px'>(-5)</div>"                                 //
			+ "<div id='cjk' style='width: 20px'>" + zhong + wen + zi + full_stop + "</div>"      //
			+ "<div id='keep-all' style='width: 20px; word-break: keep-all'>" + zhong + wen + zi + " ab</div>" //
			+ "<div id='joined' style='width: 0'>a" + wj + "b " + "c" + nbsp + "d</div>"       //
			+ "<div id='soft' style='width: 50px'>foo" + shy + "bar</div>"                    //
			+ "<div id='soft-fits' style='width: 100px'>foo" + shy + "bar</div>");

	CHECK(LinesOf(document, "hyphen") == StringList{"foo-", "bar"});
	// '-' before a digit: a minus sign unless a letter or digit is before it (Chromium's ASCII table)
	CHECK(LinesOf(document, "minus") == StringList{"x-", "5"});
	CHECK(LinesOf(document, "sign") == StringList{"(-5)"});
	// between ideographs, not before the full stop
	CHECK(LinesOf(document, "cjk") == StringList{zhong + wen, zi + full_stop});
	CHECK(LinesOf(document, "keep-all") == StringList{zhong + wen + zi, "ab"});
	// a word joiner and a no-break space hold their neighbours together
	CHECK(LinesOf(document, "joined") == StringList{"a" + wj + "b", "c" + nbsp + "d"});
	// a line that ends at a soft hyphen shows a hyphen; one that does not shows nothing
	CHECK(LinesOf(document, "soft") == StringList{"foo" + shy + "-", "bar"});
	CHECK(LinesOf(document, "soft-fits") == StringList{"foo" + shy + "bar"});

	document->Close();
}

TEST_CASE("LineBreaking.inline_box_content_area")
{
	// An inline box is as tall as its font's ascent plus descent, each rounded (CSS 2.1 §10.6.1, as browsers): 8 + 2 at
	// 10px with the fixed-advance font; a box split over three lines spans them (its boxes, getBoundingClientRect).
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("", "<div style='width: 0'><span id='span'>HHH" + zwsp + "HH" + zwsp + "H</span></div>");
	Element* span = document->GetElementById("span");
	CHECK(span->GetBox().GetSize().y == 10.f);
	REQUIRE(span->GetNumBoxes() == 3);
	Vector2f offset;
	span->GetBox(2, offset);
	CHECK(offset.y == 20.f);
	document->Close();
}

TEST_CASE("LineBreaking.flex_column_item_wraps_at_its_min_width")
{
	// A column flex item stretched to 100px but at least 200px wide formats its text at 200px: one line, not two
	// (Taffy flex/measure_child_with_min_size_greater_than_available_space).
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("",
		"<div style='display: flex; flex-direction: column; width: 100px'><div id='item' style='min-width: 200px'>HHHHHHHH" + zwsp +
			"HHHHHHHH</div></div>");
	Element* item = document->GetElementById("item");
	CHECK(item->GetBox().GetSize().x == 200.f);
	CHECK(item->GetBox().GetSize().y == 10.f);
	CHECK(LinesOf(document, "item") == StringList{"HHHHHHHH" + zwsp + "HHHHHHHH"});
	document->Close();
}

TEST_CASE("LineBreaking.shrink_to_fit_is_at_least_the_min_content_width")
{
	// CSS 2.1 §10.3.5: an inline-block in a box narrower than its longest word is as wide as that word (min(max-content,
	// max(min-content, available))), not as narrow as the box.
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load(".measure { display: inline-block; }",
		"<div style='width: 0'><span class='measure' id='zero'>HH" + zwsp + "HHH</span></div>"
		"<div style='width: 40px'><span class='measure' id='narrow'>HH" + zwsp + "HHH</span></div>"
		"<div style='width: 100px'><span class='measure' id='wide'>HH" + zwsp + "HHH</span></div>");
	CHECK(WidthOf(document, "zero") == 30.f);
	CHECK(WidthOf(document, "narrow") == 40.f);
	CHECK(WidthOf(document, "wide") == 50.f);
	document->Close();
}

TEST_CASE("LineBreaking.ascii_text_as_the_table")
{
	// A text of printable ASCII and white space asks Chromium's ASCII table directly (no table of opportunities kept,
	// ElementText's ComputeLineBreaks); with a character past ASCII the whole text goes through UAX #14: at width 0 every
	// opportunity is a line, the same in both.
	FixedAdvanceFontScope scope;
	const String ascii = "a-b?c(d) x-1 (-1) [e]{f} h/i.j,k;l:m!n%o#p=r*s+t|u~v\\w^x_y`z \"q\"";
	const String e_acute = "\xc3\xa9"; // U+00E9
	ElementDocument* document = scope.Load("",
		"<div id='ascii' style='width: 0'>" + ascii + "</div><div id='table' style='width: 0'>" + ascii + " " + e_acute + "</div>" +
			"<div id='pre' style='width: 0; white-space: pre-wrap'>" + ascii + "\t" + ascii + "\n" + ascii + "</div>");

	StringList ascii_lines = LinesOf(document, "ascii");
	StringList table_lines = LinesOf(document, "table");
	REQUIRE(table_lines.size() == ascii_lines.size() + 1);
	CHECK(table_lines.back() == e_acute);
	table_lines.pop_back();
	CHECK(table_lines == ascii_lines);
	CHECK(ascii_lines.size() >= 10);
	CHECK(LinesOf(document, "pre").size() >= 3 * ascii_lines.size());

	document->Close();
}

TEST_CASE("LineBreaking.hanging_spaces_alone_on_a_line")
{
	// Preserved spaces after a preserved newline hang at the end of a line of their own when nothing fits (width 0): the
	// layout went on asking for that line, which consumed them but held no text, without end.
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("",
		"<div id='tab' style='width: 0; white-space: pre-wrap'>a\n\tb</div><div id='spaces' style='width: 0; white-space: pre-wrap'>a\n  b</div>");
	for (const char* id : {"tab", "spaces"})
	{
		INFO(id);
		const StringList lines = LinesOf(document, id);
		REQUIRE(lines.size() == 3);
		CHECK(lines.back() == "b");
	}
	document->Close();
}
