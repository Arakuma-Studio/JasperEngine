#include "../../../Source/Core/TextJustification.h"
#include "FixedAdvanceFontEngine.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>
#include <functional>

using namespace Rml;

// p6-justify: 'text-align: justify', 'text-align-last' and 'text-justify' (CSS Text 3 §7), as Chromium 153 justifies. The
// expected positions are Chromium's for the same text in Ahem at 10px (the fixed-advance font engine's advances), read with
// getBoundingClientRect.

namespace {

Element* Get(ElementDocument* document, const String& id)
{
	Element* element = document->GetElementById(id);
	REQUIRE(element);
	return element;
}

float LeftOf(ElementDocument* document, const String& id)
{
	return Get(document, id)->GetAbsoluteOffset(BoxArea::Border).x;
}

float WidthOf(ElementDocument* document, const String& id)
{
	return Get(document, id)->GetBox().GetSize(BoxArea::Border).x;
}

// The text elements under 'id', in order.
Vector<ElementText*> TextsOf(ElementDocument* document, const String& id)
{
	Vector<ElementText*> texts;
	Element* element = Get(document, id);
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		if (auto text = rmlui_dynamic_cast<ElementText*>(element->GetChild(i)))
			texts.push_back(text);
	}
	return texts;
}

// The total space a line's justification adds.
float AddedSpace(const ElementText::Line& line)
{
	float sum = 0.f;
	for (const JustificationSpacing& spacing : line.justification)
		sum += spacing.before + spacing.after;
	return sum;
}

const String han = "\xe6\xbc\xa2";  // U+6F22
const String zi = "\xe5\xad\x97";   // U+5B57
const String nbsp = "\xc2\xa0";     // U+00A0
const String alef = "\xd8\xa7";     // U+0627 ARABIC LETTER ALEF
const String acute = "\xcc\x81";    // U+0301 COMBINING ACUTE ACCENT

// The opportunities of one text run, visited in order: {before, after} per grapheme cluster.
Vector<JustificationContext::Opportunities> OpportunitiesOf(const String& text, Style::TextJustify method)
{
	Vector<TextJustification::Cluster> clusters;
	TextJustification::GetClusters(text, clusters);
	JustificationContext context;
	Vector<JustificationContext::Opportunities> result;
	for (const TextJustification::Cluster& cluster : clusters)
		result.push_back(context.Check(method, cluster.first));
	return result;
}

int CountOf(const Vector<JustificationContext::Opportunities>& opportunities)
{
	int count = 0;
	for (auto& o : opportunities)
		count += int(o.before) + int(o.after);
	return count;
}

} // namespace

TEST_CASE("Justification.opportunities")
{
	using Style::TextJustify;

	// spaces and no-break spaces, after each one
	CHECK(CountOf(OpportunitiesOf("a b" + nbsp + "c", TextJustify::Auto)) == 2);
	CHECK(CountOf(OpportunitiesOf("a b" + nbsp + "c", TextJustify::InterWord)) == 2);
	CHECK(CountOf(OpportunitiesOf("a b c", TextJustify::None)) == 0);

	// ideographs: before and after each one with 'auto' (not before at the start or after an opportunity), not with 'inter-word'
	auto cjk = OpportunitiesOf("ab" + han + zi + "c", TextJustify::Auto);
	REQUIRE(cjk.size() == 5);
	CHECK((cjk[2].before && cjk[2].after));
	CHECK((!cjk[3].before && cjk[3].after));
	CHECK((!cjk[4].before && !cjk[4].after));
	CHECK((!OpportunitiesOf(han + zi, TextJustify::Auto)[0].before));
	CHECK(CountOf(OpportunitiesOf(han + zi, TextJustify::InterWord)) == 0);

	// inter-character: after every cluster (a combining mark is part of its letter), none inside cursive scripts
	CHECK(CountOf(OpportunitiesOf("ab c", TextJustify::InterCharacter)) == 4);
	CHECK(CountOf(OpportunitiesOf("ae" + acute + "c", TextJustify::InterCharacter)) == 3);
	CHECK(CountOf(OpportunitiesOf(alef + alef + alef, TextJustify::InterCharacter)) == 0);
	// the soft hyphen and other default-ignorables are never opportunities
	CHECK(CountOf(OpportunitiesOf("a\xc2\xad" "b", TextJustify::InterCharacter)) == 2);

	// atomic inlines: nothing with 'auto'; with 'inter-character', the character after one has an opportunity before it
	JustificationContext context;
	CHECK((!context.Check(TextJustify::InterCharacter, Character('a')).before));
	auto atomic = context.Check(TextJustify::InterCharacter, TextJustification::ObjectReplacementCharacter);
	CHECK((!atomic.before && !atomic.after));
	auto after_atomic = context.Check(TextJustify::InterCharacter, Character(' '));
	CHECK((after_atomic.before && after_atomic.after));

	// tables
	CHECK(TextJustification::IsCjkIdeographOrSymbol(Character(0x4E00)));
	CHECK(TextJustification::IsCjkIdeographOrSymbol(Character(0x3042)));   // hiragana
	CHECK(TextJustification::IsCjkIdeographOrSymbol(Character(0x1F600)));  // emoji presentation
	CHECK_FALSE(TextJustification::IsCjkIdeographOrSymbol(Character(0xAC00))); // Hangul is not
	CHECK_FALSE(TextJustification::IsCjkIdeographOrSymbol(Character('a')));
	CHECK(TextJustification::IsCursiveScript(Character(0x0627)));
	CHECK(TextJustification::IsCursiveScript(Character(0x0710))); // Syriac
	CHECK_FALSE(TextJustification::IsCursiveScript(Character(0x05D0))); // Hebrew
	CHECK(TextJustification::IsDefaultIgnorable(Character(0x200B)));
	CHECK(TextJustification::IsDefaultIgnorable(Character(0x00AD)));
	CHECK_FALSE(TextJustification::IsDefaultIgnorable(Character(0x00A0)));

	// clusters: marks, joined emoji, flags, Hangul syllables from jamo
	Vector<TextJustification::Cluster> clusters;
	TextJustification::GetClusters("e" + acute + "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9" "\xf0\x9f\x87\xab\xf0\x9f\x87\xb7" "\xe1\x84\x80\xe1\x85\xa1", clusters);
	REQUIRE(clusters.size() == 4);
	CHECK(clusters[0].offset == 0);
	CHECK(clusters[1].offset == 3);
	CHECK(clusters[2].offset == 14);
	CHECK(clusters[3].offset == 22);
}

TEST_CASE("Justification.runs")
{
	// 17px over "XX XXX X" + "XXXX": two spaces, the last one takes what is left; none after the last character
	Vector<JustificationRun> runs(2);
	runs[0].text = "XX XXX ";
	runs[1].text = "X XXXX";
	REQUIRE(JustifyRuns(runs, 17.f, false));
	REQUIRE(runs[0].spacing.size() == 2);
	REQUIRE(runs[1].spacing.size() == 1);
	CHECK(runs[0].spacing[0].offset == 2);
	CHECK(runs[0].spacing[1].offset == 6);
	CHECK(runs[1].spacing[0].offset == 1);
	CHECK(runs[0].GetTotal() + runs[1].GetTotal() == doctest::Approx(17.f));
	CHECK(runs[0].spacing[0].after == doctest::Approx(17.f / 3.f));

	// no opportunity, or no space: nothing changes
	Vector<JustificationRun> word(1);
	word[0].text = "XXXX";
	CHECK_FALSE(JustifyRuns(word, 10.f, false));
	CHECK(word[0].spacing.empty());
	Vector<JustificationRun> overflow(1);
	overflow[0].text = "X X";
	CHECK_FALSE(JustifyRuns(overflow, -1.f, false));
	CHECK_FALSE(JustifyRuns(overflow, 0.f, false));

	// a trailing opportunity is not used (inter-character: none after the last letter)
	Vector<JustificationRun> letters(1);
	letters[0].text = "XXXX";
	letters[0].method = Style::TextJustify::InterCharacter;
	REQUIRE(JustifyRuns(letters, 9.f, false));
	REQUIRE(letters[0].spacing.size() == 3);
	CHECK(letters[0].spacing[2].offset == 2);
	CHECK(letters[0].spacing[0].after == doctest::Approx(3.f));

	// right to left: visited from the end, the offsets stay sorted
	Vector<JustificationRun> rtl(2);
	rtl[0].text = "a b ";
	rtl[1].text = "c d";
	REQUIRE(JustifyRuns(rtl, 9.f, true));
	REQUIRE(rtl[0].spacing.size() == 2);
	CHECK(rtl[0].spacing[0].offset == 1);
	CHECK(rtl[0].spacing[1].offset == 3);
	CHECK(rtl[1].spacing.size() == 1);

	CHECK(GetJustificationBefore(rtl[0].spacing, 2) == doctest::Approx(3.f));
	CHECK(GetJustificationBefore(rtl[0].spacing, 0) == 0.f);
}

TEST_CASE("Justification.properties")
{
	FixedAdvanceFontScope scope;
	PropertyDictionary properties;
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(properties, "text-align-last", "justify"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(properties, "text-justify", "inter-character"));
	CHECK(StyleSheetSpecification::ParsePropertyDeclaration(properties, "text-justify", "distribute"));
	// Chromium parses neither
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(properties, "text-align", "justify-all"));
	CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(properties, "text-align-last", "match-parent"));

	ElementDocument* document = scope.Load(".j { text-align: justify; } #rtl { -rmlui-direction: rtl; }",
		"<div id='init'>x</div>"
		"<div id='dist' style='text-justify: distribute'>x</div>"
		"<div class='j' id='j'><span id='inherits'>x</span></div>"
		"<div id='last' style='text-align: center; text-align-last: end'>x</div>"
		"<div id='rtl' class='j'>x</div>");
	const ComputedValues& init = Get(document, "init")->GetComputedValues();
	CHECK(init.specified_text_align_last() == Style::TextAlignLast::Auto);
	CHECK(init.text_justify() == Style::TextJustify::Auto);
	CHECK(init.text_align_last() == Style::TextAlign::Left);
	CHECK(Get(document, "dist")->GetComputedValues().text_justify() == Style::TextJustify::InterCharacter);
	// 'auto' is 'text-align', but 'start' for 'justify'; 'start' and 'end' follow the direction
	CHECK(Get(document, "inherits")->GetComputedValues().text_align() == Style::TextAlign::Justify);
	CHECK(Get(document, "inherits")->GetComputedValues().text_align_last() == Style::TextAlign::Left);
	CHECK(Get(document, "last")->GetComputedValues().text_align_last() == Style::TextAlign::Right);
	CHECK(Get(document, "rtl")->GetComputedValues().text_align_last() == Style::TextAlign::Right);
	document->Close();
}

TEST_CASE("Justification.lines")
{
	FixedAdvanceFontScope scope;
	const String nbsp_entity = "&nbsp;";
	ElementDocument* document = scope.Load("div { width: 217px; text-align: justify; } span { display: inline; } .ib { display: inline-block; height: 5px; }",
		// 17px over five spaces on the first line, the last line is not justified
		"<div id='para'>XX <span id='a'>XXX</span> X XXXX XX <span id='b'>XXX</span> XXXXX XX X XXX XX</div>"
		// the line before a <br> is not justified, the one after it is; 'text-align-last: justify' justifies the last
		"<div id='br'>XX <span id='br1'>XX</span> X<br/>XX X XX XX XX XX XX XX XX XX XXX XX XXX <span id='br2'>X</span></div>"
		"<div id='tal' style='text-align-last: justify'>XX <span id='tal1'>XX</span> X</div>"
		// one word: nothing shares the space, the line is aligned to its start
		"<div id='word'><span id='w1'>XXXXXXXXXXXXXXXXXXXXX</span> XX</div>"
		// no-break spaces are opportunities
		"<div id='nbsp'>XX" + nbsp_entity + "<span id='n1'>XX</span> XX XXXXX XXXXX XX</div>"
		// the spaces of a span with 'text-justify: none' are not
		"<div id='tjn'>XX <span id='t1' style='text-justify: none'>X <span id='t2'>X</span></span> XX XXXX XXXXXXXXXX</div>"
		// atomic inlines are no opportunities, the spaces around them are; padding and borders take no space
		"<div id='ib'>XX <span id='ib1' class='ib' style='width: 23px'></span> XXX<span id='ib2' class='ib' style='width: 13px'></span>X XXXXXXXXXX XXX</div>"
		"<div id='pad'>XX <span id='pd1' style='padding: 0 3px; border-left: 2px solid #f00'>X XX</span> X XXXXXXXXXXXX XXX</div>"
		// letter-spacing and word-spacing are in the widths the space is shared from
		"<div id='ls' style='letter-spacing: 2px'>XX <span id='ls1'>XXX</span> X XXXX XX XXX XXXXX XX</div>"
		"<div id='ws' style='word-spacing: 3px'>XX <span id='ws1'>XXX</span> X XXXX XX XXX XXXXX XX</div>"
		// inter-character: after every letter but the last of the line; before an atomic inline's next character
		"<div id='ic' style='text-justify: inter-character'>XX <span id='ic1'>XXX</span> X XXXX XX XXX XXXXX XX</div>"
		"<div id='icb' style='text-justify: inter-character'>XX <span id='icb1' class='ib' style='width: 23px'></span> XXX XXXXXXXXXXXXXXXXX</div>"
		// ideographs: before and after each one; the space before the first is inside its span
		"<div id='cjk'>XX<span id='cj1'>" + han + zi + "</span>XX XXXXXXXXXXXXXXXXXXXXXXXX</div>"
		// a right-to-left line is filled the same way
		"<div id='rtl' style='-rmlui-direction: rtl'>XX <span id='rt1'>XXX</span> X XXXX XX XXX XXXXX XX</div>"
		// 'text-align-last' alone aligns the last line
		"<div id='center' style='text-align: left; text-align-last: center'>XX XX XX XX XX XX XX XX XX XX XXX XX XXX <span id='c1'>X</span></div>"
		// the lines of 'pre' end with forced breaks: all are last lines
		"<div id='pre' style='white-space: pre; text-align-last: justify'>XX <span id='pr1'>X</span>  XXX\nXXX <span id='pr2'>X</span></div>"
		// the last line before a block is a last line, the line after it is not
		"<div id='block'>XX <span id='bl1'>XX</span><div>X X</div>XX <span id='bl2'>XXX</span> XXXX XXXXX XX XXXX XXXXX XXXXX X</div>");

	// Chromium 153's positions and widths (getBoundingClientRect), to the LayoutUnit (1/64 px)
	const auto Check = [&](const char* id, float left, float width) {
		INFO(id);
		CHECK(LeftOf(document, id) == doctest::Approx(left).epsilon(0.0005));
		CHECK(WidthOf(document, id) == doctest::Approx(width).epsilon(0.0005));
	};
	Check("a", 33.4062f, 30.f);
	Check("b", 187.0156f, 30.f);
	Check("br1", 30.f, 20.f);
	Check("br2", 200.f, 10.f);
	Check("tal1", 103.5f, 20.f);
	Check("w1", 0.f, 210.f);
	Check("n1", 34.25f, 20.f);
	Check("t1", 55.6719f, 30.f);
	Check("t2", 75.6719f, 10.f);
	Check("ib1", 80.5f, 23.f);
	Check("ib2", 194.f, 13.f);
	Check("pd1", 69.6719f, 87.6719f);
	Check("ls1", 42.25f, 36.f);
	Check("ws1", 33.4062f, 30.f);
	Check("ic1", 32.6875f, 32.6875f);
	Check("icb1", 83.1562f, 23.f);
	Check("cj1", 20.f, 177.f);
	Check("rt1", 33.4062f, 30.f);
	Check("c1", 203.5f, 10.f);
	Check("pr1", 72.3438f, 10.f);
	Check("pr2", 207.f, 10.f);
	Check("bl1", 30.f, 20.f);
	Check("bl2", 34.25f, 30.f);

	// the text keeps the space for its glyphs: the texts of #para's first line add the space of their spaces (the space
	// before #b ends its text, the line ends after #b); the last text is all on the last line, not justified
	{
		Vector<ElementText*> texts = TextsOf(document, "para");
		REQUIRE(texts.size() == 3);
		REQUIRE(texts[0]->GetLines().size() == 1);
		CHECK(AddedSpace(texts[0]->GetLines()[0]) == doctest::Approx(3.4f));
		CHECK(texts[0]->GetLines()[0].width == 33);
		REQUIRE(texts[1]->GetLines().size() == 1);
		CHECK(texts[1]->GetLines()[0].justification.size() == 4);
		CHECK(AddedSpace(texts[1]->GetLines()[0]) == doctest::Approx(4.f * 3.4f));
		REQUIRE(texts[2]->GetLines().size() == 1);
		CHECK(texts[2]->GetLines()[0].justification.empty());
	}

	document->Close();
}

TEST_CASE("Justification.trailing_white_space")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("div { width: 217px; } span { display: inline; }",
		// a collapsible space at the end of a line is removed: from the span it ends, from the alignment
		"<div id='trail' style='text-align: justify'>XXXX XXX <span id='tr1'>XXX </span><span id='tr2'>XXXXXXXXXXXXXX</span> XX</div>"
		"<div id='right' style='text-align: right'>XXXX XXX <span id='r1'>XXX </span><span id='r2'>XXXXXXXXXXXXXX</span> XX</div>"
		"<div id='end' style='text-align: right'>XX <span id='e1' style='padding-right: 5px'>XX </span> </div>"
		// preserved spaces at the end of a wrapped line hang: they are neither justified nor aligned
		"<div id='pw' style='white-space: pre-wrap; text-align: justify'>XX  XXX <span id='pw1'>X</span>    XXXXXXXX  XXXXXXXXXX XX</div>"
		"<div id='pwr' style='white-space: pre-wrap; text-align: right'><span id='pwr1'>XX</span>  XXX X    XXXXXXXX  XXXXXXXXXX XX</div>"
		// 'pre' keeps them
		"<div id='pre' style='white-space: pre; text-align: right'><span id='p1'>XX</span>   </div>");

	const auto Check = [&](const char* id, float left, float width) {
		INFO(id);
		CHECK(LeftOf(document, id) == doctest::Approx(left).epsilon(0.0005));
		CHECK(WidthOf(document, id) == doctest::Approx(width).epsilon(0.0005));
	};
	Check("tr1", 187.f, 30.f);
	Check("tr2", 0.f, 140.f);
	Check("r1", 187.f, 30.f);
	Check("r2", 47.f, 140.f);
	Check("e1", 192.f, 25.f);
	Check("pw1", 83.f, 10.f);
	Check("pwr1", 7.f, 20.f);
	Check("p1", 167.f, 20.f);

	// the removed space is not drawn (its text ends before it)
	Vector<ElementText*> texts = TextsOf(document, "tr1");
	REQUIRE(texts.size() == 1);
	REQUIRE(texts[0]->GetLines().size() == 1);
	CHECK(texts[0]->GetLines()[0].text == "XXX");

	document->Close();
}

TEST_CASE("Justification.textarea")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("textarea { display: inline-block; width: 217px; height: 40px; white-space: pre-wrap; text-align: justify; }",
		"<textarea id='area'>XX XXX X XXXX XX XXX XXXXX XX\nXX X XX</textarea>");

	// the widget's text elements: the first line is justified (17px over five spaces), the line ended by a line feed and the
	// last line are not
	Vector<const ElementText::Line*> lines;
	std::function<void(Element*)> collect = [&](Element* element) {
		if (auto text = rmlui_dynamic_cast<ElementText*>(element))
		{
			for (const ElementText::Line& line : text->GetLines())
				lines.push_back(&line);
		}
		for (int i = 0; i < element->GetNumChildren(true); i++)
			collect(element->GetChild(i));
	};
	collect(Get(document, "area"));
	REQUIRE(lines.size() >= 3);
	CHECK(lines[0]->text == "XX XXX X XXXX XX XXX ");
	CHECK(AddedSpace(*lines[0]) == doctest::Approx(17.f));
	CHECK(lines[1]->justification.empty());
	CHECK(lines[2]->justification.empty());

	// a selection splits the line: each part is drawn where the justified line has it
	auto area = rmlui_dynamic_cast<ElementFormControlTextArea*>(Get(document, "area"));
	REQUIRE(area);
	area->Focus();
	area->SetSelectionRange(3, 6);
	scope.context->Update();
	scope.context->Render();
	lines.clear();
	collect(area);
	bool found = false;
	for (const ElementText::Line* line : lines)
	{
		if (line->text == "XXX")
		{
			found = true;
			CHECK(line->position.x == doctest::Approx(33.4f));
		}
		if (line->text == " X XXXX XX XXX ")
		{
			CHECK(line->position.x == doctest::Approx(63.4f));
			CHECK(AddedSpace(*line) == doctest::Approx(4.f * 3.4f));
		}
	}
	CHECK(found);

	document->Close();
}
