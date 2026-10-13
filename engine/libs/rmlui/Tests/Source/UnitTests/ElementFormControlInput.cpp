#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
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
