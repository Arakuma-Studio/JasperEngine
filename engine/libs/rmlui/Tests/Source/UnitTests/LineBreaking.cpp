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
	auto opportunity = [](char32_t a, char32_t b, Style::WordBreak word_break = Style::WordBreak::Normal) {
		return TextUnicode::IsLineBreakOpportunity(Character(a), Character(b), word_break);
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
			+ "<div id='cjk' style='width: 20px'>" + zhong + wen + zi + full_stop + "</div>"      //
			+ "<div id='keep-all' style='width: 20px; word-break: keep-all'>" + zhong + wen + zi + " ab</div>" //
			+ "<div id='joined' style='width: 0'>a" + wj + "b " + "c" + nbsp + "d</div>"       //
			+ "<div id='soft' style='width: 50px'>foo" + shy + "bar</div>"                    //
			+ "<div id='soft-fits' style='width: 100px'>foo" + shy + "bar</div>");

	CHECK(LinesOf(document, "hyphen") == StringList{"foo-", "bar"});
	CHECK(LinesOf(document, "minus") == StringList{"x-5"});
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
