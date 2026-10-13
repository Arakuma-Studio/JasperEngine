#include "../../../Source/Core/TransformState.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Fork, phase 4 (docs/html-ui/FORK.md P4-6): Element::UpdateTransformStates. Rendering computes each element's
// transform state; a tool that lays a context out at another size and reads boxes before the next render calls it.

namespace {

const String document_rml = R"(
<rml>
<head>
<style>
	body { display: block; position: absolute; left: 0; top: 0; right: 0; bottom: 0; }
	#centered { display: block; position: absolute; left: 50%; top: 0; width: 50%; height: 100px; transform: translateX(-50%); }
	#child { display: block; width: 10px; height: 10px; transform: translateX(5px); }
</style>
</head>
<body><div id="centered"><div id="child"/></div></body>
</rml>
)";

// x of the element's border box origin through its transform state
float TransformedX(Element* element)
{
	const TransformState* state = element->GetTransformState();
	const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
	if (!state || !state->GetTransform())
		return offset.x;
	const Vector4f mapped = *state->GetTransform() * Vector4f(offset.x, offset.y, 0.f, 1.f);
	return mapped.x / mapped.w;
}

} // namespace

TEST_CASE("TransformStates.update_without_render")
{
	Context* context = TestsShell::GetContext();
	const Vector2i original = context->GetDimensions();
	context->SetDimensions(Vector2i(1000, 400));
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop(false);

	Element* centered = document->GetElementById("centered");
	Element* child = document->GetElementById("child");
	// left 500, width 500, translateX(-50%): 250
	CHECK(TransformedX(centered) == doctest::Approx(250.f));
	CHECK(TransformedX(child) == doctest::Approx(255.f));

	// laid out at another size: the transform states are the last render's until updated
	context->SetDimensions(Vector2i(600, 400));
	context->Update();
	CHECK(centered->GetAbsoluteOffset(BoxArea::Border).x == doctest::Approx(300.f));
	CHECK(TransformedX(centered) != doctest::Approx(150.f));

	context->GetRootElement()->UpdateTransformStates();
	// left 300, width 300: 150
	CHECK(TransformedX(centered) == doctest::Approx(150.f));
	CHECK(TransformedX(child) == doctest::Approx(155.f));

	// and back, before any render
	context->SetDimensions(Vector2i(1000, 400));
	context->Update();
	context->GetRootElement()->UpdateTransformStates();
	CHECK(TransformedX(centered) == doctest::Approx(250.f));
	TestsShell::RenderLoop(false);
	CHECK(TransformedX(centered) == doctest::Approx(250.f));

	document->Close();
	context->SetDimensions(original);
	TestsShell::ShutdownShell();
}
