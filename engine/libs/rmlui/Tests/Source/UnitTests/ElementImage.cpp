#include "../Common/TestsInterface.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <doctest.h>

using namespace Rml;

static const String document_wrapped_image_rml = R"(
<rml>
<head>
	<title>Test</title>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			left: 0;
			top: 0;
			right: 0;
			bottom: 0;
		}
	</style>
</head>

<body>
<div>
	<img src="/assets/high_scores_alien_1.tga"/>
</div>
</body>
</rml>
)";

TEST_CASE("elementimage.dp_ratio")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_wrapped_image_rml, "assets/");

	Element* div = TestsShell::GetElementChild(document, 0);
	Element* img = TestsShell::GetElementChild(div, 0);

	document->Show();
	const float img_width = img->GetClientWidth();

	SUBCASE("basic")
	{
		context->SetDensityIndependentPixelRatio(2.0f);
		context->Update();
		CHECK(img->GetClientWidth() == 2.f * img_width);
	}

	SUBCASE("clone")
	{
		context->SetDensityIndependentPixelRatio(2.0f);
		auto clone_ptr = div->Clone();

		context->SetDensityIndependentPixelRatio(4.f);
		Element* clone_div = document->AppendChild(std::move(clone_ptr));
		Element* clone_img = TestsShell::GetElementChild(clone_div, 0);
		context->Update();

		CHECK(clone_img->GetClientWidth() == 4.f * img_width);
	}

	SUBCASE("create_image")
	{
		context->SetDensityIndependentPixelRatio(2.0f);
		ElementPtr new_img_ptr = document->CreateElement("img");
		new_img_ptr->SetAttribute("src", "high_scores_alien_1.tga");

		context->SetDensityIndependentPixelRatio(4.0f);
		Element* new_img = document->AppendChild(std::move(new_img_ptr));
		context->Update();

		CHECK(new_img->GetClientWidth() == 4.f * img_width);
	}

	TestsShell::RenderLoop();

	document->Close();
	TestsShell::ShutdownShell();
}

static const String document_wrapped_image_rml_ratio_test = R"(
<rml>
<head>
	<title>Test</title>
	<style>
		.width-50 { width: 50px; }
		.height-50 { height: 50px; }
		.height-100 { height: 100px; }
	</style>
</head>
<body>
	<img src="/test_not_loaded/test_512_256.tga" id="test1" alt="Original size" />
	<img src="/test_not_loaded/test_512_256.tga" id="test2" height="50" alt="Set height to 50, width auto, attribute set" />
	<img src="/test_not_loaded/test_512_256.tga" id="test3" class="height-50" alt="Set height to 50, width auto, css set" />
	<img src="/test_not_loaded/test_512_256.tga" id="test4" width="50" alt="Set width to 50, height auto, attribute set" />
	<img src="/test_not_loaded/test_512_256.tga" id="test5" class="width-50" alt="Set width to 50, height auto, css set" />
	<img src="/test_not_loaded/test_512_256.tga" id="test6" width="50" height="100" alt="Set width to 50 and height to 100, attribute set" />
	<img src="/test_not_loaded/test_512_256.tga" id="test7" class="height-100 width-50" alt="Set width to 50 and height to 100, css set" />
</body>
</rml>
)";

TEST_CASE("elementimage.preserve_ratio")
{
	Context* context = TestsShell::GetContext();
	ElementDocument* document = context->LoadDocumentFromMemory(document_wrapped_image_rml_ratio_test, "assets/");
	document->Show();

	// Texture size is hardcoded in test render interface to 512x256
	{
		INFO("original image size");
		Element* img_test1 = document->GetElementById("test1");
		CHECK(img_test1->GetClientWidth() == 512);
		CHECK(img_test1->GetClientHeight() == 256);
	}

	{
		INFO("height only - attribute set");
		Element* img_test2 = document->GetElementById("test2");
		CHECK(img_test2->GetClientWidth() == 100);
		CHECK(img_test2->GetClientHeight() == 50);
	}

	{
		INFO("height only - css set");
		Element* img_test3 = document->GetElementById("test3");
		CHECK(img_test3->GetClientWidth() == 100);
		CHECK(img_test3->GetClientHeight() == 50);
	}

	{
		INFO("width only - attribute set");
		Element* img_test4 = document->GetElementById("test4");
		CHECK(img_test4->GetClientWidth() == 50);
		CHECK(img_test4->GetClientHeight() == 25);
	}

	{
		INFO("width only - css set");
		Element* img_test5 = document->GetElementById("test5");
		CHECK(img_test5->GetClientWidth() == 50);
		CHECK(img_test5->GetClientHeight() == 25);
	}

	{
		INFO("height and width - attribute set");
		Element* img_test6 = document->GetElementById("test6");
		CHECK(img_test6->GetClientWidth() == 50);
		CHECK(img_test6->GetClientHeight() == 100);
	}

	{
		INFO("height and width - css set");
		Element* img_test7 = document->GetElementById("test7");
		CHECK(img_test7->GetClientWidth() == 50);
		CHECK(img_test7->GetClientHeight() == 100);
	}

	TestsShell::RenderLoop();

	document->Close();
	TestsShell::ShutdownShell();
}

// Fork (P4-G1): the picture of a replaced element is trimmed to the content edge curve of a rounded box (CSS Backgrounds 3 §5.3).
TEST_CASE("elementimage.replaced_content_mesh")
{
	// A 40 x 40 content box at (14, 14): a 4px border, 10px of padding, 20px radii (6px at the content edge).
	const RenderBox content_box(Vector2f(40.f), Vector2f(0.f), EdgeSizes{14.f, 14.f, 14.f, 14.f}, CornerSizes{20.f, 20.f, 20.f, 20.f});
	const ColourbPremultiplied white(255, 255, 255, 255);

	SUBCASE("covering")
	{
		// The content box filled like a background, the texture coordinates of the quad (here larger: object-fit cover).
		Mesh mesh;
		const Rectanglef quad = Rectanglef::FromPositionSize({4.f, 14.f}, {60.f, 40.f});
		CHECK(MeshUtilities::GenerateReplacedContent(mesh, content_box, quad, white, {0.f, 0.f}, {1.f, 1.f}, 1.f));
		CHECK(mesh.vertices.size() > 16);
		for (const Vertex& vertex : mesh.vertices)
		{
			CHECK(vertex.position.x >= 14.f - 1.f);
			CHECK(vertex.position.x <= 54.f + 1.f);
			CHECK(vertex.tex_coord.x == doctest::Approx((vertex.position.x - 4.f) / 60.f));
			CHECK(vertex.tex_coord.y == doctest::Approx((vertex.position.y - 14.f) / 40.f));
		}
	}

	SUBCASE("inside")
	{
		// Clear of the curves: the quad as it is.
		Mesh mesh;
		CHECK(MeshUtilities::GenerateReplacedContent(mesh, content_box, Rectanglef::FromPositionSize({16.f, 20.f}, {36.f, 20.f}), white, {0.f, 0.f},
			{1.f, 1.f}, 1.f));
		CHECK(mesh.vertices.size() == 4);
	}

	SUBCASE("crossing")
	{
		// A corner of the quad beyond the curve: generated untrimmed, for a clip mask.
		Mesh mesh;
		CHECK(!MeshUtilities::GenerateReplacedContent(mesh, content_box, Rectanglef::FromPositionSize({14.f, 16.f}, {40.f, 20.f}), white,
			{0.f, 0.f}, {1.f, 1.f}, 1.f));
		CHECK(mesh.vertices.size() == 4);
	}

	SUBCASE("square")
	{
		const RenderBox square(Vector2f(40.f), Vector2f(0.f), EdgeSizes{}, CornerSizes{});
		Mesh mesh;
		CHECK(MeshUtilities::GenerateReplacedContent(mesh, square, Rectanglef::FromPositionSize({0.f, 10.f}, {40.f, 20.f}), white, {0.f, 0.f},
			{1.f, 1.f}, 1.f));
		CHECK(mesh.vertices.size() == 4);
	}
}

// Fork (P4-G1): a rounded img whose picture crosses its curve (object-fit: contain) draws it under a clip mask; a square img,
// or a rounded one whose picture covers its content box (trimmed by its mesh), draws without one.
TEST_CASE("elementimage.rounded_content_clip")
{
	Context* context = TestsShell::GetContext();
	TestsRenderInterface* render = TestsShell::GetTestsRenderInterface();
	REQUIRE(render);

	static const String rml = R"(
<rml>
<head>
	<style>
		body { left: 0; top: 0; right: 0; bottom: 0; }
		img { display: block; width: 40px; height: 80px; }
		#round { border-radius: 50%; padding: 4px; border: 2px #000; }
	</style>
</head>
<body>
	<img id="square" src="/assets/high_scores_alien_1.tga"/>
	<img id="round" src="/assets/high_scores_alien_1.tga"/>
</body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml, "assets/");
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* round = document->GetElementById("round");
	const auto render_frame = [&]() {
		render->ResetCounters();
		context->Render();
		render->ResetCounters();
		return render->GetCountersFromPreviousReset();
	};

	// Filled: the mesh trims the picture.
	CHECK(render_frame().render_to_clip_mask == 0);

	// Contained (a square picture in a tall box, across the elliptical curves): one mask.
	round->SetProperty("object-fit", "contain");
	context->Update();
	CHECK(render_frame().render_to_clip_mask == 1);

	round->SetProperty("border-radius", "0px");
	context->Update();
	CHECK(render_frame().render_to_clip_mask == 0);

	// Inside an 'overflow: hidden' rounded box, the picture's own mask intersects the box's.
	round->RemoveProperty("border-radius");
	document->GetElementById("square")->SetProperty("display", "none");
	document->SetProperty("overflow", "hidden");
	document->SetProperty("border-radius", "5px");
	context->Update();
	// The img is clipped by the document's mask, then by the document's and the picture's, then by the document's again.
	CHECK(render_frame().render_to_clip_mask == 4);

	document->Close();
	TestsShell::ShutdownShell();
}
