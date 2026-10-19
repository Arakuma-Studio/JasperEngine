#include "FixedAdvanceFontEngine.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>
#include <functional>

using namespace Rml;

// p6-justify, P6-J2: 'text-indent' (CSS Text 3 §8.1) as Chromium 153 indents. The expected boxes are Chromium's for the same
// document in Ahem at 10px (the fixed-advance font engine's advances), read with getBoundingClientRect.

namespace {

Element* Get(ElementDocument* document, const String& id)
{
	Element* element = document->GetElementById(id);
	REQUIRE(element);
	return element;
}

} // namespace

TEST_CASE("TextIndent.properties")
{
	FixedAdvanceFontScope scope;
	PropertyDictionary properties;
	for (const char* value : {"10px", "10%", "-5px", "calc(10% + 1px)", "10px hanging", "each-line 10px", "hanging 1em each-line"})
	{
		INFO(value);
		CHECK(StyleSheetSpecification::ParsePropertyDeclaration(properties, "text-indent", value));
	}
	for (const char* value : {"hanging", "10px 20px", "10px hanging hanging", "auto", "each-line each-line 1px"})
	{
		INFO(value);
		CHECK_FALSE(StyleSheetSpecification::ParsePropertyDeclaration(properties, "text-indent", value));
	}

	ElementDocument* document = scope.Load("#parent { text-indent: 2em each-line; font-size: 20px; } #pct { text-indent: 10% hanging; }",
		"<div id='init'>x</div><div id='parent'><span id='child'>x</span></div><div id='pct'>x</div>");
	const ComputedValues& init = Get(document, "init")->GetComputedValues();
	CHECK(init.text_indent().type == Style::LengthPercentage::Length);
	CHECK(init.text_indent().value == 0.f);
	CHECK(init.text_indent_mode() == Style::TextIndentMode::None);
	// inherited: the length computed where it is declared, the keywords with it
	const ComputedValues& child = Get(document, "child")->GetComputedValues();
	CHECK(child.text_indent().value == doctest::Approx(40.f));
	CHECK(child.text_indent_mode() == Style::TextIndentMode::EachLine);
	const ComputedValues& pct = Get(document, "pct")->GetComputedValues();
	CHECK(pct.text_indent().type == Style::LengthPercentage::Percentage);
	CHECK(pct.text_indent().value == doctest::Approx(10.f));
	CHECK(pct.text_indent_mode() == Style::TextIndentMode::Hanging);
	document->Close();
}

TEST_CASE("TextIndent.lines")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("div { width: 200px; } span { display: inline; } .ib { display: inline-block; width: auto; }",
		// a length; a percentage of the block's width; negative (into the padding)
		"<div id='len' style='text-indent: 25px'><span id='len1'>XX</span> XXX XXXX XX XXX XXXXX XX <span id='len2'>X</span> XXX XX XX</div>"
		"<div id='pct' style='text-indent: 10%'><span id='pct1'>XX</span> XXX</div>"
		"<div id='neg' style='text-indent: -15px; padding-left: 20px'><span id='neg1'>XX</span> XXX XXXX XX XXX XXXXX XX <span id='neg2'>X</span></div>"
		// alignment in the width the indent leaves; justified to the end
		"<div id='cen' style='text-indent: 30px; text-align: center'><span id='cen1'>XX</span> XXX XXXX XX XXX XXXXX XX X XXX XX <span id='cen2'>XX</span></div>"
		"<div id='rig' style='text-indent: 30px; text-align: right'><span id='rig1'>XX</span> XXX XXXX XX XXX XXXXX XX</div>"
		"<div id='jus' style='text-indent: 30px; text-align: justify'><span id='jus1'>XX</span> XXX XXXX <span id='jus2'>XX</span> XXX XXXXX XX</div>"
		// right to left: the indent at the end of the line (the right)
		"<div id='rtl' style='text-indent: 30px; -rmlui-direction: rtl'>XX XXX XXXX <span id='rtl1'>XX</span> XXX XXXXX XX</div>"
		// the first line only; 'each-line' also after a forced break; 'hanging' every other line
		"<div id='br' style='text-indent: 30px'><span id='br1'>XX</span><br/><span id='br2'>XX</span></div>"
		"<div id='hang' style='text-indent: 30px hanging'><span id='hang1'>XX</span> XXX XXXX XX XXX XXXXX XX <span id='hang2'>X</span></div>"
		"<div id='each' style='text-indent: 30px each-line'><span id='each1'>XX</span><br/><span id='each2'>XX</span></div>"
		"<div id='pre' style='text-indent: 30px; white-space: pre'><span id='pre1'>XX</span>\n<span id='pre2'>XX</span></div>"
		"<div id='pree' style='text-indent: 30px each-line; white-space: pre'><span id='pree1'>XX</span>\n<span id='pree2'>XX</span></div>"
		// the first formatted line: in a block child (inherited), not after one, not after an empty one
		"<div id='blk' style='text-indent: 30px'><span id='blk1'>XX</span><div><span id='blk2'>XX</span></div><span id='blk3'>XX</span></div>"
		"<div id='emp' style='text-indent: 30px'><div></div><span id='emp1'>XX</span></div>"
		// after a float; calc() (the float above reaches the next line); em computed where declared
		"<div id='flt' style='text-indent: 30px'><span style='float: left; width: 40px; height: 15px'></span><span id='flt1'>XX</span></div>"
		"<div id='calc' style='text-indent: calc(10% + 5px)'><span id='calc1'>XX</span></div>"
		"<div id='em' style='text-indent: 2em; font-size: 20px; line-height: 20px'><span id='em1' style='font-size: 10px'>XX</span></div>"
		// intrinsic widths count the indent; a percentage counts as 0 there, then resolves against the width
		"<div style='width: auto'><span id='ibw' class='ib' style='text-indent: 30px'>XX XX</span></div>"
		"<div style='width: auto'><span id='ibp' class='ib' style='text-indent: 10%'><span id='ibp1'>XX</span> XX</span></div>"
		"<div style='width: auto'><span id='ibm' class='ib' style='width: min-content; text-indent: 20px'>XX XX</span></div>"
		"<div style='width: auto'><span id='ibn' class='ib' style='width: min-content; text-indent: -20px'>XXXX XX</span></div>");

	const auto Check = [&](const char* id, float left, float top, float width) {
		INFO(id);
		Element* element = Get(document, id);
		CHECK(element->GetAbsoluteOffset(BoxArea::Border).x == doctest::Approx(left));
		CHECK(element->GetAbsoluteOffset(BoxArea::Border).y == doctest::Approx(top));
		CHECK(element->GetBox().GetSize(BoxArea::Border).x == doctest::Approx(width));
	};
	Check("len1", 25.f, 0.f, 20.f);
	Check("len2", 130.f, 10.f, 10.f);
	Check("pct1", 20.f, 30.f, 20.f);
	Check("neg1", 5.f, 40.f, 20.f);
	Check("neg2", 110.f, 50.f, 10.f);
	Check("cen1", 45.f, 60.f, 20.f);
	Check("cen2", 105.f, 80.f, 20.f);
	Check("rig1", 60.f, 90.f, 20.f);
	Check("jus1", 30.f, 110.f, 20.f);
	Check("jus2", 180.f, 110.f, 20.f);
	Check("rtl1", 150.f, 130.f, 20.f);
	Check("br1", 30.f, 150.f, 20.f);
	Check("br2", 0.f, 160.f, 20.f);
	Check("hang1", 0.f, 170.f, 20.f);
	Check("hang2", 120.f, 180.f, 10.f);
	Check("each1", 30.f, 190.f, 20.f);
	Check("each2", 30.f, 200.f, 20.f);
	Check("pre1", 30.f, 210.f, 20.f);
	Check("pre2", 0.f, 220.f, 20.f);
	Check("pree1", 30.f, 230.f, 20.f);
	Check("pree2", 30.f, 240.f, 20.f);
	Check("blk1", 30.f, 250.f, 20.f);
	Check("blk2", 30.f, 260.f, 20.f);
	Check("blk3", 0.f, 270.f, 20.f);
	Check("emp1", 0.f, 280.f, 20.f);
	Check("flt1", 70.f, 290.f, 20.f);
	Check("calc1", 65.f, 300.f, 20.f);
	Check("em1", 40.f, 318.f, 20.f);
	Check("ibw", 0.f, 333.f, 80.f);
	Check("ibp", 0.f, 343.f, 50.f);
	Check("ibp1", 5.f, 343.f, 20.f);
	Check("ibm", 0.f, 363.f, 40.f);
	Check("ibn", 0.f, 383.f, 20.f);
	document->Close();
}

TEST_CASE("TextIndent.textarea")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load(
		"textarea, input { display: inline-block; width: 200px; height: 40px; white-space: pre-wrap; text-indent: 30px; }",
		"<textarea id='area'>XX XXX XXXX XX XXX XXXXX XX X\nXX XX</textarea>");

	// the first line starts after the indent and wraps in the width it leaves; the line after the line feed is not indented
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
	CHECK(lines[0]->text == "XX XXX XXXX XX XXX ");
	CHECK(lines[0]->position.x == doctest::Approx(30.f));
	CHECK(lines[1]->position.x == doctest::Approx(0.f));
	CHECK(lines[2]->text == "XX XX");
	CHECK(lines[2]->position.x == doctest::Approx(0.f));
	document->Close();
}

TEST_CASE("TextIndent.transition")
{
	FixedAdvanceFontScope scope;
	ElementDocument* document = scope.Load("#t { text-indent: 0px; transition: text-indent 1s linear; } #t.to { text-indent: 40px; }",
		"<div id='t' style='width: 200px'><span id='t1'>XX</span></div>");
	Element* element = Get(document, "t");
	scope.system_interface.SetManualTime(0.0);
	scope.context->Update();
	element->SetClass("to", true);
	scope.context->Update();
	scope.system_interface.SetManualTime(0.5);
	scope.context->Update();
	CHECK(element->GetComputedValues().text_indent().value == doctest::Approx(20.f).epsilon(0.01));
	CHECK(Get(document, "t1")->GetAbsoluteOffset(BoxArea::Border).x == doctest::Approx(20.f).epsilon(0.01));
	document->Close();
}
