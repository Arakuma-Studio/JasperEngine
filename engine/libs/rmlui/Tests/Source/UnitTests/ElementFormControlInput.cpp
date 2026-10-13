#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <doctest.h>

using namespace Rml;

TEST_CASE("form.input.text.no_font")
{
	// Missing font style causes intrinsic width of 'input.text' to be set to zero pixels. Ensure we handle this gracefully without assertions.
	const String document_rml = R"(<rml>
	<head>
		<link type="text/rcss" href="/assets/rml.rcss" />
		<style>
			body {
				color: white;
				width: 300dp;
				height: 225dp;
			}
			input.text {
				border: 1px solid #000000;
				padding: 5dp 36dp 5dp 12dp;
				background: #fff;
				border-radius: 8dp;
			}
		</style>
	</head>

	<body>
		<input type="text"/>
	</body>
</rml>)";

	Context* context = TestsShell::GetContext();

	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	document->Show();
	context->Update();
	context->Render();

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("form.input.range.progress_on_track")
{
	// The value fill is placed at the track's border box: a fill as large as the track covers it, border included.
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(R"(
<rml>
<head>
	<style>
		body { width: 400px; height: 300px; }
		input { display: block; width: 129px; height: 16px; }
		slidertrack { margin-top: 4px; margin-left: 1px; margin-right: 1px; height: 6px; border: 1px solid #000; }
		sliderprogress { height: 8px; box-sizing: border-box; }
		sliderbar { width: 16px; height: 16px; }
	</style>
</head>
<body>
	<input type="range" id="r" min="0" max="100" value="70"/>
</body>
</rml>
)");
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();

	Element* input = document->GetElementById("r");
	REQUIRE(input);
	Element* track = nullptr;
	Element* progress = nullptr;
	for (int i = 0; i < input->GetNumChildren(true); i++)
	{
		Element* child = input->GetChild(i);
		if (child->GetTagName() == "slidertrack")
			track = child;
		for (int j = 0; j < child->GetNumChildren(true); j++)
			if (child->GetChild(j)->GetTagName() == "sliderprogress")
				progress = child->GetChild(j);
	}
	REQUIRE(track);
	REQUIRE(progress);
	CHECK(progress->GetAbsoluteOffset(BoxArea::Border) == track->GetAbsoluteOffset(BoxArea::Border));

	document->Close();
	TestsShell::ShutdownShell();
}

// Fork (P4-G2): a button input is laid out as an HTML button: its content, as one block, centered vertically in its content
// box ('safe': content taller than the box starts at its top), whatever 'align-content' says; as a flex container it is laid
// out as any other (Chromium).
TEST_CASE("form.input.button.centered_content")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	const String document_rml = R"(<rml>
	<head>
		<style>
			body { width: 600px; height: 600px; font-family: LatoLatin; font-size: 16px; }
			input { display: inline-block; width: 100px; height: 80px; }
			.child { display: block; height: 20px; }
		</style>
	</head>
	<body>
		<input type="button" id="plain"><div class="child" id="plain-child"/></input>
		<input type="submit" id="padded" style="box-sizing: border-box; padding: 10px 0 30px;"><div class="child" id="padded-child"/></input>
		<input type="button" id="min" style="height: auto; min-height: 80px;"><div class="child" id="min-child"/></input>
		<input type="button" id="start" style="align-content: start;"><div class="child" id="start-child"/></input>
		<input type="button" id="overflow" style="height: 10px;"><div class="child" id="overflow-child"/></input>
		<input type="button" id="block" style="display: block;"><div class="child" id="block-child"/></input>
		<input type="button" id="flex" style="display: flex;"><div class="child" id="flex-child"/></input>
		<div id="div" style="display: inline-block; width: 100px; height: 80px;"><div class="child" id="div-child"/></div>
	</body>
	</rml>)";

	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	// The child's top, from its button's content box.
	const auto child_top = [&](const char* id) {
		Element* button = document->GetElementById(id);
		Element* child = document->GetElementById(String(id) + "-child");
		return child->GetAbsoluteOffset(BoxArea::Border).y - button->GetAbsoluteOffset(BoxArea::Content).y;
	};

	CHECK(child_top("plain") == doctest::Approx(30.f));
	CHECK(child_top("padded") == doctest::Approx(10.f)); // (80 - 40 - 20) / 2
	CHECK(child_top("min") == doctest::Approx(30.f));
	CHECK(child_top("start") == doctest::Approx(30.f));
	CHECK(child_top("overflow") == doctest::Approx(0.f));
	CHECK(child_top("block") == doctest::Approx(30.f));
	CHECK(child_top("flex") == doctest::Approx(0.f));
	CHECK(child_top("div") == doctest::Approx(0.f));

	// A text label: its line is centered, and the button's baseline with it.
	Element* text_button = document->AppendChild(document->CreateElement("input"));
	text_button->SetAttribute("type", "submit");
	text_button->SetAttribute("value", "Go");
	text_button->SetProperty("height", "80px");
	text_button->SetProperty("font-size", "16px");
	context->Update();
	const float line_height = text_button->GetLineHeight();
	const Box& box = text_button->GetBox();
	const float label_top = 0.5f * (80.f - line_height);
	// The baseline is measured from the bottom margin edge: below the centered line's baseline.
	CHECK(text_button->GetBaseline() < 80.f - label_top);
	CHECK(text_button->GetBaseline() > 80.f - label_top - line_height);
	CHECK(box.GetSize().y == doctest::Approx(80.f));

	// Without a line, the baseline is the content box's bottom (Chromium), not the bottom margin edge of an inline-block.
	Element* empty_button = document->GetElementById("plain");
	empty_button->SetProperty("padding-bottom", "6px");
	empty_button->SetProperty("margin-bottom", "3px");
	context->Update();
	CHECK(empty_button->GetBaseline() == doctest::Approx(9.f));
	CHECK(document->GetElementById("div")->GetBaseline() == doctest::Approx(0.f));

	document->Close();
	TestsShell::ShutdownShell();
}

// Fork (P4-G10): a button whose display is inline (or inline-table) is an inline-block, as HTML's button layout and
// Chromium have it: its size and padding apply, it is one box on the line; it follows the input's type.
TEST_CASE("form.input.button.display_inline")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	const String document_rml = R"(<rml>
	<head>
		<style>
			body { width: 600px; height: 600px; font-family: LatoLatin; font-size: 16px; }
			input, span { display: inline; width: 60px; height: 30px; padding: 2px 5px; }
		</style>
	</head>
	<body>
		<p>x <input type="button" id="button" value="Go"/> x <input type="submit" id="table" style="display: inline-table" value="Go"/> x <input type="text" id="text"/> <span id="span">x</span></p>
	</body>
	</rml>)";

	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* button = document->GetElementById("button");
	Element* table = document->GetElementById("table");
	Element* text = document->GetElementById("text");
	Element* span = document->GetElementById("span");
	CHECK(button->GetDisplay() == Style::Display::InlineBlock);
	CHECK(table->GetDisplay() == Style::Display::InlineBlock);
	CHECK(button->GetBox().GetSize() == Vector2f(60.f, 30.f));
	CHECK(button->GetBox().GetEdge(BoxArea::Padding, BoxEdge::Left) == doctest::Approx(5.f));
	// Other elements keep 'inline': a text field (replaced: an inline-level box anyway), a span (its width does not apply).
	CHECK(text->GetDisplay() == Style::Display::Inline);
	CHECK(span->GetDisplay() == Style::Display::Inline);
	CHECK(span->GetBox().GetSize().x < 60.f);

	// The display follows the type.
	text->SetAttribute("type", "button");
	button->SetAttribute("type", "text");
	context->Update();
	CHECK(text->GetDisplay() == Style::Display::InlineBlock);
	CHECK(button->GetDisplay() == Style::Display::Inline);

	document->Close();
	TestsShell::ShutdownShell();
}

namespace {
// An embedder's input with an 'appearance': its native look unless it has the class 'none' (fork, P4-G12).
class ThemedInput : public ElementFormControlInput {
public:
	explicit ThemedInput(const String& tag) : ElementFormControlInput(tag) {}
	bool HasNativeAppearance() const override { return !const_cast<ThemedInput*>(this)->IsClassSet("none"); }
};
} // namespace

// Fork (P4-G12): a themed checkbox or radio has no border, padding, border radius or painted background (Chromium's theme);
// without its look the author's apply. A button's display types as Chromium's theme and HTML's button layout give them.
TEST_CASE("form.input.themed_box")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	static ElementInstancerGeneric<ThemedInput> instancer;
	Factory::RegisterElementInstancer("tinput", &instancer);

	const String document_rml = R"(<rml>
	<head>
		<style>
			body { width: 600px; height: 600px; font-family: LatoLatin; font-size: 16px; }
			tinput { display: inline-block; box-sizing: border-box; width: 30px; height: 20px; border: 3px solid #f00; padding: 4px;
				border-radius: 6px; background-color: #f00; decorator: horizontal-gradient(#f00 #00f); }
			.b { display: table-cell; width: auto; height: auto; }
			.none { cursor: pointer; } /* (the embedder's appearance changes a declaration: its values are computed again) */
			#plain { display: inline-block; width: 30px; height: 20px; padding: 4px; }
		</style>
	</head>
	<body>
		<tinput type="checkbox" id="checkbox"/><tinput type="radio" id="radio"/><tinput type="text" id="text"/>
		<tinput type="button" id="cell" class="b" value="x"/><tinput type="button" id="table" class="b" style="display: table" value="x"/>
		<tinput type="button" id="item" class="b" style="display: list-item" value="x"/><input type="checkbox" id="plain"/>
	</body>
	</rml>)";

	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	const auto themed = [&](const char* id) {
		const ComputedValues& computed = document->GetElementById(id)->GetComputedValues();
		return computed.border_top_width() == 0.f && computed.border_left_width() == 0.f && computed.padding_top().value == 0.f &&
			computed.padding_left().value == 0.f && !computed.has_border_radius() && computed.background_color().alpha == 0 &&
			!computed.has_decorator();
	};
	const auto plain = [&](const char* id) {
		const ComputedValues& computed = document->GetElementById(id)->GetComputedValues();
		return computed.border_top_width() == 3.f && computed.padding_top().value == 4.f && computed.has_border_radius() &&
			computed.background_color().alpha == 255 && computed.has_decorator();
	};
	CHECK(themed("checkbox"));
	CHECK(themed("radio"));
	CHECK(plain("text"));
	CHECK(document->GetElementById("checkbox")->GetBox().GetSize(BoxArea::Border) == Vector2f(30.f, 20.f));
	CHECK(document->GetElementById("checkbox")->GetBox().GetSize(BoxArea::Content) == Vector2f(30.f, 20.f));

	// Without the look (and relaid out).
	Element* checkbox = document->GetElementById("checkbox");
	checkbox->SetClass("none", true);
	context->Update();
	CHECK(plain("checkbox"));
	CHECK(checkbox->GetBox().GetSize(BoxArea::Content) == Vector2f(16.f, 6.f));
	checkbox->SetClass("none", false);
	context->Update();
	CHECK(themed("checkbox"));
	CHECK(checkbox->GetBox().GetSize(BoxArea::Content) == Vector2f(30.f, 20.f));

	// RmlUi's own input has no appearance: unchanged.
	CHECK(document->GetElementById("plain")->GetComputedValues().padding_top().value == 4.f);

	// Buttons: table and list-item are blocks; an internal table type is an inline-block with the look, a block without.
	CHECK(document->GetElementById("table")->GetDisplay() == Style::Display::Block);
	CHECK(document->GetElementById("item")->GetDisplay() == Style::Display::Block);
	Element* cell = document->GetElementById("cell");
	CHECK(cell->GetDisplay() == Style::Display::InlineBlock);
	cell->SetClass("none", true);
	context->Update();
	CHECK(cell->GetDisplay() == Style::Display::Block);

	document->Close();
	TestsShell::ShutdownShell();
}

// Fork (P4-G13): radios without a name are in no group (HTML): checking one leaves the others; radios with one name are.
TEST_CASE("form.input.radio.unnamed")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(R"(<rml><head/><body>
		<input type="radio" id="a" checked=""/><input type="radio" id="b" checked=""/>
		<input type="radio" name="n" id="c" checked=""/><input type="radio" name="n" id="d" checked=""/>
	</body></rml>)");
	REQUIRE(document);
	document->Show();
	context->Update();
	CHECK(document->GetElementById("a")->HasAttribute("checked"));
	CHECK(document->GetElementById("b")->HasAttribute("checked"));
	CHECK(!document->GetElementById("c")->HasAttribute("checked"));
	CHECK(document->GetElementById("d")->HasAttribute("checked"));
	document->GetElementById("a")->RemoveAttribute("checked");
	document->GetElementById("a")->SetAttribute("checked", "");
	context->Update();
	CHECK(document->GetElementById("b")->HasAttribute("checked"));
	document->Close();
	TestsShell::ShutdownShell();
}

// Fork (P4-G3): a single-line text field taller than its line centers the line (Chromium), also when it is shorter.
TEST_CASE("form.input.text.centered_line")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	const String document_rml = R"(<rml>
	<head>
		<style>
			body { width: 600px; height: 600px; font-family: LatoLatin; font-size: 16px; }
			input { display: block; width: 200px; }
		</style>
	</head>
	<body>
		<input type="text" id="tall" value="Text" style="height: 50px;"/>
		<input type="password" id="padded" value="Text" style="height: 50px; padding: 4px 0 16px;"/>
		<input type="text" id="short" value="Text" style="height: 6px;"/>
		<textarea id="area" style="height: 80px;">Text</textarea>
	</body>
	</rml>)";

	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	context->Update();
	context->Render();

	// The top of the field's text, from its content box.
	const auto text_top = [&](const char* id) {
		Element* field = document->GetElementById(id);
		for (int i = 0; i < field->GetNumChildren(true); i++)
		{
			Element* child = field->GetChild(i);
			if (child->GetTagName() == "#text")
				return child->GetAbsoluteOffset(BoxArea::Border).y - field->GetAbsoluteOffset(BoxArea::Content).y;
		}
		FAIL("no text element");
		return 0.f;
	};

	Element* tall = document->GetElementById("tall");
	const float line_height = tall->GetLineHeight();
	CHECK(text_top("tall") == doctest::Approx(0.5f * (50.f - line_height)));
	CHECK(text_top("padded") == doctest::Approx(0.5f * (50.f - line_height))); // (content-box: 50px of content)
	CHECK(text_top("short") == doctest::Approx(0.5f * (6.f - line_height)));
	// A multi-line field keeps its lines at the top.
	CHECK(text_top("area") == doctest::Approx(0.f));

	// A larger font in the same height: the line moves with it.
	tall->SetProperty("font-size", "30px");
	context->Update();
	context->Render();
	CHECK(text_top("tall") == doctest::Approx(0.5f * (50.f - tall->GetLineHeight())));

	document->Close();
	TestsShell::ShutdownShell();
}

// fork P4-3: a text field leaves the key events of a gamepad to the document
TEST_CASE("form.input.text.gamepad_keys")
{
	const String document_rml = R"(<rml>
	<head>
		<link type="text/rcss" href="/assets/rml.rcss" />
		<style>
			body { font-family: LatoLatin; width: 300px; height: 200px; }
			input.text { width: 200px; }
		</style>
	</head>
	<body>
		<input id="field" type="text" value="abc"/>
	</body>
</rml>)";

	struct KeyCounter : EventListener {
		int count = 0;
		void ProcessEvent(Event& /*event*/) override { count++; }
	} counter;

	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	document->Show();
	context->Update();
	document->AddEventListener(EventId::Keydown, &counter);

	auto* field = rmlui_dynamic_cast<ElementFormControlInput*>(document->GetElementById("field"));
	REQUIRE(field);
	field->Focus(true);
	context->Update();

	// a gamepad's key: the field neither edits nor stops it
	for (Input::KeyIdentifier key : {Input::KI_DELETE, Input::KI_RIGHT, Input::KI_ESCAPE})
	{
		Dictionary parameters;
		parameters["key_identifier"] = (int)key;
		parameters["gamepad"] = 0;
		CHECK(field->DispatchEvent(EventId::Keydown, parameters));
	}
	CHECK(counter.count == 3);
	CHECK(field->GetValue() == "abc");

	// the keyboard's: the field takes it (the caret is at the start)
	CHECK_FALSE(context->ProcessKeyDown(Input::KI_DELETE, 0));
	CHECK(counter.count == 3);
	CHECK(field->GetValue() == "bc");

	document->RemoveEventListener(EventId::Keydown, &counter);
	document->Close();
	TestsShell::ShutdownShell();
}
