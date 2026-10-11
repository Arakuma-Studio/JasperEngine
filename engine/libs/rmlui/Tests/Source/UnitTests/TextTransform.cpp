#include "../../../Source/Core/TextUnicode.h"
#include "FixedAdvanceFontEngine.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-text (docs/html-ui/phase-3-fork-patches.md 3.10): 'text-transform' with Unicode case mappings
// (Utilities/gen_case_tables.py). The expected strings are what Chromium 153 gives (innerText of the same text).

namespace {

String Upper(const String& text)
{
	String out;
	for (StringIteratorU8 it(text); it; ++it)
		TextUnicode::AppendUppercase(out, *it);
	return out;
}

String Lower(const String& text)
{
	String out;
	for (StringIteratorU8 it(text); it; ++it)
		TextUnicode::AppendLowercase(out, *it);
	return out;
}

String TransformedText(ElementDocument* document, const String& id)
{
	Element* element = document->GetElementById(id);
	REQUIRE(element);
	String result;
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		if (auto text = rmlui_dynamic_cast<ElementText*>(element->GetChild(i)))
		{
			for (const ElementText::Line& line : text->GetLines())
				result += line.text;
		}
	}
	return result;
}

} // namespace

TEST_CASE("TextTransform.case_mapping")
{
	// full mappings: the sharp s, ligatures, a letter with a preceding apostrophe; ASCII and other scripts
	CHECK(Upper("stra\xc3\x9f" "e") == "STRASSE");
	CHECK(Upper("\xef\xac\x81") == "FI");                       // U+FB01 -> "FI"
	CHECK(Upper("\xc5\x89") == "\xca\xbcN");                    // U+0149 -> U+02BC N
	CHECK(Upper("\xd0\xbf\xd1\x80\xd0\xb8") == "\xd0\x9f\xd0\xa0\xd0\x98"); // Cyrillic
	CHECK(Upper("\xf0\x90\x90\xa8") == "\xf0\x90\x90\x80");     // U+10428 -> U+10400 (Deseret, supplementary plane)
	CHECK(Lower("\xc4\xb0") == "i\xcc\x87");                    // U+0130 -> i + U+0307
	CHECK(Lower("ABC\xc3\x84") == "abc\xc3\xa4");
	// Turkish is not special without a language tailoring
	CHECK(Upper("i") == "I");
	CHECK(Lower("I") == "i");
	// simple mappings alone
	CHECK(TextUnicode::ToUpper(Character(0xDF)) == Character(0xDF));
	CHECK(TextUnicode::ToUpper(Character(0x01C6)) == Character(0x01C4));
	CHECK(TextUnicode::ToLower(Character(0x01C4)) == Character(0x01C6));
	String title;
	TextUnicode::AppendTitlecase(title, Character(0x01C6));
	TextUnicode::AppendTitlecase(title, Character(0xDF));
	TextUnicode::AppendTitlecase(title, Character(0x1FB3));
	CHECK(title == "\xc7\x85\xc3\x9f\xe1\xbe\xbc"); // U+01C5, U+00DF, U+1FBC

	// word starts
	auto starts = [](const char32_t before_previous, char32_t previous, char32_t c) {
		return TextUnicode::IsWordStart(Character(before_previous), Character(previous), Character(c));
	};
	CHECK(starts(0, 0, 'a'));
	CHECK(starts('a', ' ', 'b'));
	CHECK_FALSE(starts(' ', 'a', 'b'));
	CHECK_FALSE(starts('n', '\'', 't'));
	CHECK(starts('e', '.', 'g'));
	CHECK(starts('o', '-', 'b'));
	CHECK_FALSE(starts('x', '_', 'y'));
	CHECK_FALSE(starts('1', ',', '5'));
	CHECK_FALSE(starts(' ', '3', 'r'));
	CHECK_FALSE(starts(0, ' ', '-'));
}

TEST_CASE("TextTransform.elements")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("",
		String("<div id='upper' style='text-transform: uppercase'>stra\xc3\x9f" "e \xc7\x86 \xef\xac\x81 \xc5\x89 i</div>") +
			"<div id='lower' style='text-transform: lowercase'>\xce\x9f\xce\x94\xce\x9f\xce\xa3 \xce\x9f\xce\x94\xce\x9f\xce\xa3. \xc4\xb0 "
			"\xce\xa3 \xce\x91\xce\xa3</div>"
			"<div id='capitalize' style='text-transform: capitalize'>foo-bar don't e.g. 3rd x_y a.b \xc7\x86" "emal \xc3\x9f" "a (paren) a/b "
			"1,5a a\xc2\xb7" "b</div>"
			"<div id='none'>stra\xc3\x9f" "e</div>"
			"<span id='width' style='text-transform: uppercase'>\xc3\x9f</span>");

	// Chromium 153: "STRASSE Ǆ FI ʼN I", "οδος οδος. i̇ σ ας", "Foo-Bar Don't E.G. 3rd X_y A.B ǅemal ßa (Paren) A/B 1,5a A·b"
	CHECK(TransformedText(document, "upper") == "STRASSE \xc7\x84 FI \xca\xbcN I");
	CHECK(TransformedText(document, "lower") == "\xce\xbf\xce\xb4\xce\xbf\xcf\x82 \xce\xbf\xce\xb4\xce\xbf\xcf\x82. i\xcc\x87 \xcf\x83 \xce\xb1\xcf\x82");
	CHECK(TransformedText(document, "capitalize") == "Foo-Bar Don't E.G. 3rd X_y A.B \xc7\x85" "emal \xc3\x9f" "a (Paren) A/B 1,5a A\xc2\xb7" "b");
	CHECK(TransformedText(document, "none") == "stra\xc3\x9f" "e");
	// the transformed text is measured: "SS" is two letters wide
	CHECK(document->GetElementById("width")->GetBox().GetSize().x == 20.f);

	document->Close();
}
