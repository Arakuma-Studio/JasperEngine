#include "../../../Source/Core/IndividualTransforms.h"
#include "../../../Source/Core/TransformState.h"
#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 (docs/html-ui/phase-3-fork-patches.md 3.13.7, FORK.md P3-A5): 'translate', 'rotate' and 'scale'.

namespace {

String Parsed(const String& name, const String& value)
{
	PropertyDictionary dictionary;
	if (!StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value))
		return "<invalid>";
	const Property* property = dictionary.GetProperty(StyleSheetSpecification::GetPropertyId(name));
	return property ? property->ToString() : "<unset>";
}

String TransformFunction(PropertyParserIndividualTransform::Kind kind, const String& value)
{
	String function;
	if (!PropertyParserIndividualTransform::ToTransformFunction(kind, value, function))
		return "<invalid>";
	return function;
}

const String document_rml = R"(
<rml>
<head>
	<style>
		body { inset: 0; }
		div { display: block; position: absolute; left: 0; top: 0; width: 100px; height: 100px; }
		#composed { translate: 10px 0px; rotate: 90deg; scale: 2; transform: translateX(5px); }
		#plain { transform: translate(10px, 0px) rotate(90deg) scale(2) translateX(5px); }
		#hover { scale: 1; translate: none; transition: scale 1s linear, translate 1s linear; }
		#hover.on { scale: 2; translate: 10px 20px; }
		#origin { rotate: 90deg; transform-origin: 0 0; }
	</style>
</head>
<body><div id="composed"/><div id="plain"/><div id="hover"/><div id="origin"/></body>
</rml>
)";

} // namespace

TEST_CASE("IndividualTransforms.parse")
{
	TestsShell::GetContext();
	using Kind = PropertyParserIndividualTransform::Kind;

	CHECK(TransformFunction(Kind::Translate, "10px") == "translate3d(10px, 0px, 0px)");
	CHECK(TransformFunction(Kind::Translate, "10px 20%") == "translate3d(10px, 20%, 0px)");
	CHECK(TransformFunction(Kind::Translate, "10px 20px 5px") == "translate3d(10px, 20px, 5px)");
	CHECK(TransformFunction(Kind::Translate, "none") == "none");
	CHECK(TransformFunction(Kind::Rotate, "45deg") == "rotate(45deg)");
	CHECK(TransformFunction(Kind::Rotate, "x 45deg") == "rotateX(45deg)");
	CHECK(TransformFunction(Kind::Rotate, "45deg Y") == "rotateY(45deg)");
	CHECK(TransformFunction(Kind::Rotate, "z 1rad") == "rotateZ(1rad)");
	CHECK(TransformFunction(Kind::Rotate, "1 1 0 90deg") == "rotate3d(1, 1, 0, 90deg)");
	CHECK(TransformFunction(Kind::Scale, "1.5") == "scale3d(1.5, 1.5, 1)");
	CHECK(TransformFunction(Kind::Scale, "150% 50%") == "scale3d(1.5, 0.5, 1)");
	CHECK(TransformFunction(Kind::Scale, "1 2 3") == "scale3d(1, 2, 3)");

	CHECK(Parsed("translate", "10px 20px") != "<invalid>");
	CHECK(Parsed("rotate", "0") != "<invalid>");
	CHECK(Parsed("scale", "none") == "none");
	for (const char* invalid : {"10px 20px 5%", "1 2 3 4", "10deg", "x"})
		CHECK(Parsed("translate", invalid) == "<invalid>");
	for (const char* invalid : {"x", "1 1 45deg", "45deg 45deg", "w 45deg", "10px"})
		CHECK(Parsed("rotate", invalid) == "<invalid>");
	for (const char* invalid : {"1 2 3 4", "1px", "x"})
		CHECK(Parsed("scale", invalid) == "<invalid>");

	TestsShell::ShutdownShell();
}

TEST_CASE("IndividualTransforms.composition_and_transitions")
{
	Context* context = TestsShell::GetContext();
	TestsSystemInterface* system = TestsShell::GetTestsSystemInterface();
	system->SetManualTime(0.0);
	ElementDocument* document = context->LoadDocumentFromMemory(document_rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop(false);

	// translate * rotate * scale * transform, around the transform origin: as one 'transform' list in that order.
	Element* composed = document->GetElementById("composed");
	Element* plain = document->GetElementById("plain");
	REQUIRE(composed->GetTransformState());
	REQUIRE(plain->GetTransformState());
	REQUIRE(composed->GetTransformState()->GetTransform());
	const Matrix4f& a = *composed->GetTransformState()->GetTransform();
	const Matrix4f& b = *plain->GetTransformState()->GetTransform();
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			CHECK(a[i][j] == doctest::Approx(b[i][j]).epsilon(1e-4));
	CHECK(composed->GetComputedValues().has_local_transform());

	Element* origin = document->GetElementById("origin");
	Vector2f point(50.f, 0.f);
	REQUIRE(origin->GetTransformState());
	const Vector4f mapped = *origin->GetTransformState()->GetTransform() * Vector4f(point.x, point.y, 0.f, 1.f);
	CHECK(mapped.x == doctest::Approx(0.f).epsilon(1e-3));
	CHECK(mapped.y == doctest::Approx(50.f).epsilon(1e-3));

	// They transition independently (the common 'transition: scale .2s' with ':hover { scale: 1.05 }').
	Element* hover = document->GetElementById("hover");
	CHECK(hover->GetComputedValues().has_local_transform());
	hover->SetClass("on", true);
	context->Update();
	double t = 0.0;
	while (t < 0.5)
	{
		t = Math::Min(t + 0.05, 0.5);
		system->SetManualTime(t);
		context->Update();
	}
	CHECK(hover->GetProperty<String>("scale") == "scale3d(1.5, 1.5, 1)");
	CHECK(hover->GetProperty<String>("translate") == "translate3d(5px, 10px, 0px)");

	document->Close();
	system->SetManualTime(0.0);
	TestsShell::ShutdownShell();
}
