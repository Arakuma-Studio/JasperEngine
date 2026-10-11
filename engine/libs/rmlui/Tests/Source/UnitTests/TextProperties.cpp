#include "../../../Source/Core/Layout/LayoutDetails.h"
#include "../../../Source/Core/TextShadow.h"
#include "../Common/TestsInterface.h"
#include "FixedAdvanceFontEngine.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleValue.h>
#include <doctest.h>

using namespace Rml;

// Phase 3, p3-text (docs/html-ui/phase-3-fork-patches.md 3.10): parsing and computed values of the text properties, and
// their effect on text layout, with a font engine of fixed advances (every character 10px wide at 10px).

namespace {

// The declaration as RmlUi parses it: property name -> value as a string; empty when the declaration is rejected.
using Parsed = UnorderedMap<String, String>;
Parsed Parse(const String& name, const String& value)
{
	PropertyDictionary dictionary;
	Parsed result;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value))
		return result;
	for (const auto& id_property : dictionary.GetProperties())
		result[StyleSheetSpecification::GetPropertyName(id_property.first)] = id_property.second.ToString();
	return result;
}

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

const Style::ComputedValues& Computed(ElementDocument* document, const String& id)
{
	Element* element = document->GetElementById(id);
	REQUIRE(element);
	return element->GetComputedValues();
}

} // namespace

TEST_CASE("TextProperties.parsing")
{
	TestsShell::GetContext();

	// spacing
	CHECK(Parse("letter-spacing", "50%") == Parsed{{"letter-spacing", "50%"}});
	CHECK(Parse("word-spacing", "normal") == Parsed{{"word-spacing", "normal"}});
	CHECK(Parse("word-spacing", "0.5em") == Parsed{{"word-spacing", "0.5em"}});
	CHECK(Parse("word-spacing", "red").empty());

	// white-space: the shorthand of white-space-collapse and text-wrap-mode
	CHECK(Parse("white-space", "pre") == Parsed{{"white-space-collapse", "preserve"}, {"text-wrap-mode", "nowrap"}});
	CHECK(Parse("white-space", "pre-wrap") == Parsed{{"white-space-collapse", "preserve"}, {"text-wrap-mode", "wrap"}});
	CHECK(Parse("white-space", "pre-line") == Parsed{{"white-space-collapse", "preserve-breaks"}, {"text-wrap-mode", "wrap"}});
	CHECK(Parse("white-space", "nowrap") == Parsed{{"white-space-collapse", "collapse"}, {"text-wrap-mode", "nowrap"}});
	CHECK(Parse("white-space", "break-spaces") == Parsed{{"white-space-collapse", "break-spaces"}, {"text-wrap-mode", "wrap"}});
	CHECK(Parse("white-space", "normal") == Parsed{{"white-space-collapse", "collapse"}, {"text-wrap-mode", "wrap"}});
	CHECK(Parse("white-space", "nowrap preserve-spaces") == Parsed{{"white-space-collapse", "preserve-spaces"}, {"text-wrap-mode", "nowrap"}});
	CHECK(Parse("white-space", "preserve") == Parsed{{"white-space-collapse", "preserve"}, {"text-wrap-mode", "wrap"}});
	CHECK(Parse("white-space", "pre wrap").empty());
	CHECK(Parse("white-space", "nowrap wrap").empty());
	CHECK(Parse("white-space", "collapse preserve").empty());
	CHECK(Parse("text-wrap", "balance") == Parsed{{"text-wrap-mode", "wrap"}});
	CHECK(Parse("text-wrap", "nowrap") == Parsed{{"text-wrap-mode", "nowrap"}});
	CHECK(Parse("text-wrap", "wrap pretty") == Parsed{{"text-wrap-mode", "wrap"}});
	CHECK(Parse("text-wrap", "pretty balance").empty());
	CHECK(Parse("text-wrap-mode", "balance").empty());

	// breaking
	CHECK(Parse("overflow-wrap", "anywhere") == Parsed{{"overflow-wrap", "anywhere"}});
	CHECK(Parse("word-wrap", "break-word") == Parsed{{"overflow-wrap", "break-word"}});
	CHECK(Parse("word-break", "keep-all") == Parsed{{"word-break", "keep-all"}});

	// alignment
	CHECK(Parse("text-align", "start") == Parsed{{"text-align", "start"}});
	CHECK(Parse("text-align", "match-parent") == Parsed{{"text-align", "match-parent"}});
	CHECK(Parse("text-align", "-webkit-center") == Parsed{{"text-align", "-webkit-center"}});

	// decoration
	CHECK(Parse("text-decoration-line", "underline line-through") == Parsed{{"text-decoration-line", "5"}});
	CHECK(Parse("text-decoration-line", "none") == Parsed{{"text-decoration-line", "0"}});
	CHECK(Parse("text-decoration-line", "underline underline").empty());
	CHECK(Parse("text-decoration-line", "none underline").empty());
	CHECK(Parse("text-decoration-line", "blink") == Parsed{{"text-decoration-line", "0"}});
	{
		const Parsed parsed = Parse("text-decoration", "underline dotted #ff0000 2px");
		REQUIRE(parsed.size() == 4);
		CHECK(parsed.at("text-decoration-line") == "1");
		CHECK(parsed.at("text-decoration-style") == "dotted");
		CHECK(parsed.at("text-decoration-thickness") == "2px");
		CHECK(parsed.at("text-decoration-color") != "currentcolor");
	}
	{
		// omitted longhands are reset
		const Parsed parsed = Parse("text-decoration", "overline underline");
		REQUIRE(parsed.size() == 4);
		CHECK(parsed.at("text-decoration-line") == "3");
		CHECK(parsed.at("text-decoration-style") == "solid");
		CHECK(parsed.at("text-decoration-color") == "currentcolor");
		CHECK(parsed.at("text-decoration-thickness") == "auto");
	}
	CHECK(Parse("text-decoration", "none").at("text-decoration-line") == "0");
	CHECK(Parse("text-decoration", "underline solid dotted").empty());
	CHECK(Parse("text-decoration", "underline 2px 3px").empty());
	CHECK(Parse("text-decoration", "underline nonsense").empty());
	CHECK(Parse("text-decoration-style", "wavy") == Parsed{{"text-decoration-style", "wavy"}});
	CHECK(Parse("text-decoration-thickness", "from-font") == Parsed{{"text-decoration-thickness", "from-font"}});
	CHECK(Parse("text-decoration-thickness", "10%") == Parsed{{"text-decoration-thickness", "10%"}});
	CHECK(Parse("text-underline-offset", "0.2em") == Parsed{{"text-underline-offset", "0.2em"}});

	// shadows
	CHECK(Parse("text-shadow", "none") == Parsed{{"text-shadow", "none"}});
	CHECK(Parse("text-shadow", "1px 2px") == Parsed{{"text-shadow", "1px 2px 0px"}});
	CHECK(Parse("text-shadow", "1px 2px 3px red").at("text-shadow").find("1px 2px 3px") == 0);
	CHECK(Parse("text-shadow", "red 1px 2px").size() == 1);
	CHECK(Parse("text-shadow", "1px 2px currentcolor") == Parsed{{"text-shadow", "1px 2px 0px"}});
	CHECK(Parse("text-shadow", "1px 2px 3px #000, 0 0 0.5em blue").size() == 1);
	CHECK(Parse("text-shadow", "1px").empty());
	CHECK(Parse("text-shadow", "1px 2px -3px").empty());
	CHECK(Parse("text-shadow", "1px 2px 3px 4px").empty());
	CHECK(Parse("text-shadow", "1px red 2px").empty());
	CHECK(Parse("text-shadow", "10% 2px").empty());
	CHECK(Parse("text-shadow", "1px 2px,").empty());

	// interaction
	CHECK(Parse("user-select", "none") == Parsed{{"user-select", "none"}});
	CHECK(Parse("user-select", "all") == Parsed{{"user-select", "all"}});
	CHECK(Parse("accent-color", "auto") == Parsed{{"accent-color", "auto"}});
	CHECK(Parse("accent-color", "#00ff00").size() == 1);

	TestsShell::ShutdownShell();
}

TEST_CASE("TextProperties.computed_values")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load(".parent { font-size: 20px; letter-spacing: 0.5em; word-spacing: 10%; }",
		"<div class='parent' id='parent'><div id='child' style='font-size: 10px'>x</div></div>"
		"<div id='percent' style='letter-spacing: 50%; word-spacing: calc(1px + 10%)'>x</div>"
		"<div id='rtl' style='-rmlui-direction: rtl; text-align: start'><div id='rtl-child' style='-rmlui-direction: ltr'>x</div>"
		"<div id='rtl-end' style='text-align: end'>x</div><div id='match' style='-rmlui-direction: ltr; text-align: match-parent'>x</div></div>"
		"<div id='initial'>x</div><div id='center' style='text-align: -webkit-center'>x</div>"
		"<div id='ws' style='white-space: pre-line'>x</div><div id='ws-pre' style='white-space: pre'>x</div>"
		"<div id='wrap' style='overflow-wrap: break-word; word-break: keep-all'>x</div>"
		"<div id='decoration' style='text-decoration: underline overline'>x</div>"
		"<div id='shadow' style='text-shadow: 1px 2px'><span id='shadow-child'>x</span></div>");

	// letter-spacing and word-spacing compute to lengths where declared and inherit as such
	CHECK(Computed(document, "parent").letter_spacing() == 10.f);
	CHECK(Computed(document, "parent").word_spacing() == doctest::Approx(2.f));
	CHECK(Computed(document, "child").letter_spacing() == 10.f);
	CHECK(Computed(document, "child").word_spacing() == doctest::Approx(2.f));
	CHECK(Computed(document, "percent").letter_spacing() == 5.f);
	CHECK(Computed(document, "percent").word_spacing() == doctest::Approx(2.f));
	CHECK(Computed(document, "initial").letter_spacing() == 0.f);

	// text-align: 'start' is the initial value; 'start' and 'end' follow the direction where they are used
	CHECK(Computed(document, "initial").text_align() == Style::TextAlign::Left);
	CHECK(Computed(document, "initial").specified_text_align() == Style::TextAlign::Start);
	CHECK(Computed(document, "rtl").text_align() == Style::TextAlign::Right);
	CHECK(Computed(document, "rtl-child").text_align() == Style::TextAlign::Left);
	CHECK(Computed(document, "rtl-end").text_align() == Style::TextAlign::Left);
	// (Chromium: the parent's 'start' resolved against this element's direction)
	CHECK(Computed(document, "match").text_align() == Style::TextAlign::Left);
	CHECK(Computed(document, "center").text_align() == Style::TextAlign::Center);

	// white-space longhands, and the CSS 2.1 keyword they amount to
	CHECK(Computed(document, "ws").white_space_collapse() == Style::WhiteSpaceCollapse::PreserveBreaks);
	CHECK(Computed(document, "ws").text_wrap_mode() == Style::TextWrapMode::Wrap);
	CHECK(Computed(document, "ws").white_space() == Style::WhiteSpace::Preline);
	CHECK(Computed(document, "ws-pre").white_space() == Style::WhiteSpace::Pre);
	CHECK(Computed(document, "initial").white_space() == Style::WhiteSpace::Normal);

	CHECK(Computed(document, "wrap").overflow_wrap() == Style::OverflowWrap::BreakWord);
	CHECK(Computed(document, "wrap").word_break() == Style::WordBreak::KeepAll);
	CHECK(Computed(document, "decoration").text_decoration() == (Style::TextDecoration::Underline | Style::TextDecoration::Overline));

	// text-shadow is inherited
	CHECK(Computed(document, "shadow").has_text_shadow());
	CHECK(Computed(document, "shadow-child").has_text_shadow());
	CHECK_FALSE(Computed(document, "initial").has_text_shadow());

	document->Close();
}

TEST_CASE("TextProperties.spacing_layout")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load(".measure { display: inline-block; white-space: nowrap; }",
		"<div><span class='measure' id='letters' style='letter-spacing: 2px'>abc</span></div>"
		"<div><span class='measure' id='words' style='word-spacing: 5px'>a b&nbsp;c</span></div>"
		"<div><span class='measure' id='both' style='word-spacing: 1em; letter-spacing: 1px'>a b</span></div>");

	auto width = [&](const char* id) { return document->GetElementById(id)->GetBox().GetSize(BoxArea::Border).x; };
	// a letter spacing after each character, a word spacing after each space and no-break space
	CHECK(width("letters") == 36.f);
	CHECK(width("words") == 60.f);
	CHECK(width("both") == 43.f);
	// the width of a string measured for an element (text inputs place their cursor with it) has the same spacing
	CHECK(ElementUtilities::GetStringWidth(document->GetElementById("words"), "a b") == 35);

	document->Close();
}

TEST_CASE("TextProperties.white_space_layout")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("div { width: 40px; }",
		"<div id='pre-wrap' style='white-space: pre-wrap'>ab      cd</div>"
		"<div id='break-spaces' style='white-space: break-spaces'>ab      cd</div>"
		"<div id='pre-line' style='white-space: pre-line'>a   b\nc</div>"
		"<div id='preserve-spaces' style='white-space-collapse: preserve-spaces; text-wrap-mode: nowrap'>a  b\nc</div>"
		"<div id='nowrap' style='white-space: nowrap'>aa bb cc</div>"
		"<div id='pre' style='white-space: pre'>aa bb\ncc</div>");

	// pre-wrap: the spaces at the end of the line hang (no wrap, no width), the next word starts the next line
	CHECK(LinesOf(document, "pre-wrap") == StringList{"ab", "cd"});
	// break-spaces: the spaces take room and wrap, a line may break after each of them
	CHECK(LinesOf(document, "break-spaces") == StringList{"ab  ", "    ", "cd"});
	// pre-line: spaces collapse, line feeds break
	CHECK(LinesOf(document, "pre-line") == StringList{"a b\n", "c"});
	// preserve-spaces: spaces kept, a line feed is a space
	CHECK(LinesOf(document, "preserve-spaces") == StringList{"a  b c"});
	CHECK(LinesOf(document, "nowrap") == StringList{"aa bb cc"});
	CHECK(LinesOf(document, "pre") == StringList{"aa bb\n", "cc"});

	document->Close();
}

TEST_CASE("TextProperties.overflow_wrap_layout")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load(".measure { display: inline-block; } div { width: 30px; }",
		"<div id='normal'>abcdefg</div>"
		"<div id='anywhere' style='overflow-wrap: anywhere'>abcdefg</div>"
		"<div id='break-word' style='overflow-wrap: break-word'>abcdefg</div>"
		"<div id='word-wrap' style='word-wrap: break-word'>abcdefg</div>"
		"<div id='after-word' style='overflow-wrap: anywhere; width: 50px'>ab abcdefg</div>"
		"<span class='measure' id='min-anywhere' style='overflow-wrap: anywhere'>abcdefg hi</span>"
		"<span class='measure' id='min-break-word' style='overflow-wrap: break-word'>abcdefg hi</span>"
		"<span class='measure' id='min-legacy' style='word-break: break-word'>abcdefg hi</span>");

	CHECK(LinesOf(document, "normal") == StringList{"abcdefg"});
	CHECK(LinesOf(document, "anywhere") == StringList{"abc", "def", "g"});
	CHECK(LinesOf(document, "break-word") == StringList{"abc", "def", "g"});
	CHECK(LinesOf(document, "word-wrap") == StringList{"abc", "def", "g"});
	// the word goes to its own line before it is broken
	CHECK(LinesOf(document, "after-word") == StringList{"ab", "abcde", "fg"});

	// min-content: 'anywhere' (and the legacy 'word-break: break-word') count the breaks inside words, 'break-word' does not
	const Vector2f containing_block(800.f, 600.f);
	CHECK(LayoutDetails::GetMinContentWidth(document->GetElementById("min-anywhere"), containing_block) == 10.f);
	CHECK(LayoutDetails::GetMinContentWidth(document->GetElementById("min-legacy"), containing_block) == 10.f);
	CHECK(LayoutDetails::GetMinContentWidth(document->GetElementById("min-break-word"), containing_block) == 70.f);

	document->Close();
}

TEST_CASE("TextProperties.text_shadow_value")
{
	TestsShell::GetContext();
	PropertyDictionary dictionary;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(dictionary, "text-shadow", "1px 2px 4px #ff0000, 0 0 6px"));
	const Property* property = dictionary.GetProperty(PropertyId::TextShadow);
	REQUIRE(property);
	REQUIRE(property->unit == Unit::STYLE_VALUE);
	const auto* list = static_cast<const TextShadowListValue*>(property->value.Get<StyleValuePtr>().get());
	REQUIRE(list);
	REQUIRE(list->shadows.size() == 2);
	CHECK(list->shadows[0].offset_x.value.number == 1.f);
	CHECK(list->shadows[0].offset_y.value.number == 2.f);
	CHECK(list->shadows[0].blur.value.number == 4.f);
	CHECK_FALSE(list->shadows[0].current_colour);
	CHECK(list->shadows[0].colour == Colourb(255, 0, 0));
	CHECK(list->shadows[1].current_colour);
	CHECK(list->shadows[1].blur.value.number == 6.f);

	// interpolation: the shorter list is padded with transparent shadows
	PropertyDictionary other;
	REQUIRE(StyleSheetSpecification::ParsePropertyDeclaration(other, "text-shadow", "3px 4px 8px #0000ff"));
	const auto* to = static_cast<const TextShadowListValue*>(other.GetProperty(PropertyId::TextShadow)->value.Get<StyleValuePtr>().get());
	ElementDocument* document = TestsShell::GetContext()->LoadDocumentFromMemory("<rml><body style='color: #00ff00'/></rml>");
	REQUIRE(document);
	TestsShell::GetContext()->Update();
	const StyleValuePtr mid = list->Interpolate(*to, 0.5f, *document);
	REQUIRE(bool(mid));
	const auto* half = static_cast<const TextShadowListValue*>(mid.get());
	REQUIRE(half->shadows.size() == 2);
	CHECK(half->shadows[0].offset_x.value.number == 2.f);
	CHECK(half->shadows[0].blur.value.number == 6.f);
	CHECK(half->shadows[0].colour.alpha == 255);
	CHECK(half->shadows[0].colour.red == 128);
	CHECK(half->shadows[0].colour.blue == 128);
	// currentcolor (green) towards transparent: half of green
	CHECK_FALSE(half->shadows[1].current_colour);
	CHECK(half->shadows[1].colour.alpha == 128);
	CHECK(half->shadows[1].colour.green == 255);
	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("TextProperties.text_shadow_render")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("", "<div id='hard' style='text-shadow: 1px 1px #000'>ab</div><div id='none'>ab</div>");
	scope.render_interface.ResetCounters();
	scope.context->Render();
	const auto hard = scope.render_interface.GetCounters();
	// two strings for the shadowed text: the shadow, then the text; no layer or filter
	CHECK(hard.compile_filter == 0);

	document->GetElementById("hard")->SetProperty("text-shadow", "1px 1px 4px #000");
	scope.context->Update();
	scope.render_interface.ResetCounters();
	scope.context->Render();
	// a blurred shadow goes through the blur filter
	CHECK(scope.render_interface.GetCounters().compile_filter == 1);

	scope.font_engine.generated.clear();
	document->GetElementById("hard")->SetProperty("text-shadow", "1px 1px red, 2px 2px blue");
	scope.context->Update();
	scope.context->Render();
	// the text and two shadows of it
	CHECK(std::count(scope.font_engine.generated.begin(), scope.font_engine.generated.end(), "ab") == 3);
	document->Close();
}

TEST_CASE("TextProperties.user_select_in_text_input")
{
	TestsShell::GetContext();
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(R"(<rml><head><style>body { font-family: LatoLatin; font-size: 16px; }
		input { display: inline-block; width: 200px; }</style></head>
		<body><input id='free' type='text' value='hello'/><input id='none' type='text' value='hello' style='user-select: none'/></body></rml>)");
	REQUIRE(document);
	document->Show();
	context->Update();

	auto* free_input = rmlui_dynamic_cast<ElementFormControlInput*>(document->GetElementById("free"));
	auto* none_input = rmlui_dynamic_cast<ElementFormControlInput*>(document->GetElementById("none"));
	REQUIRE(free_input);
	REQUIRE(none_input);
	int begin = -1, end = -1;
	// (a selection is made in the focused input)
	free_input->Focus();
	free_input->SetSelectionRange(1, 4);
	free_input->GetSelection(&begin, &end, nullptr);
	CHECK(begin == 1);
	CHECK(end == 4);
	none_input->Focus();
	none_input->SetSelectionRange(1, 4);
	none_input->GetSelection(&begin, &end, nullptr);
	CHECK(begin == end);

	document->Close();
	TestsShell::ShutdownShell();
}
