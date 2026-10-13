#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
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
