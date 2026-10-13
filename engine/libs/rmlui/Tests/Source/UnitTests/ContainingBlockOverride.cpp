#include "../../../Source/Core/Layout/ContainerBox.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.9, FORK.md P3-9): an absolutely positioned element can be
// given a containing block that is not its offset parent's padding box (a grid area for p3-grid, the viewport for
// p3-layout's position: fixed). Its size, insets and percentages then resolve against that rectangle.

namespace {

// A box formatting absolutely positioned children of an element, as a grid container will with its grid areas.
class TestContainer final : public ContainerBox {
public:
	TestContainer(Element* element, ContainerBox* parent) : ContainerBox(Type::BlockContainer, element, parent), box(element->GetBox()) {}

	void Close() { ClosePositionedElements(); }

	const Box* GetIfBox() const override { return &box; }
	String DebugDumpTree(int depth) const override { return String(depth * 2, ' ') + "TestContainer"; }

private:
	Box box;
};

const String containing_block_rml = R"(
<rml>
<head>
	<style>
		body { width: 800px; height: 600px; }
		#cb { display: block; position: relative; width: 200px; height: 100px; padding: 10px; border: 5px solid #000; }
		#abs { position: absolute; left: 10%; top: 4px; width: 50%; height: 20%; }
		#abs-far { position: absolute; right: 10px; bottom: 10%; width: 20px; height: 10px; }
	</style>
</head>
<body><div id="cb"><div id="abs"/><div id="abs-far"/></div></body>
</rml>
)";

} // namespace

TEST_CASE("ContainingBlockOverride")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(containing_block_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* cb = document->GetElementById("cb");
	Element* abs = document->GetElementById("abs");
	Element* far = document->GetElementById("abs-far");

	auto check_padding_box_layout = [&]() {
		// The containing block is #cb's padding box: 220 x 120, at (5, 5) in its border box.
		CHECK(abs->GetContainingBlock().x == doctest::Approx(220.f));
		CHECK(abs->GetContainingBlock().y == doctest::Approx(120.f));
		CHECK(abs->GetBox().GetSize(BoxArea::Border).x == doctest::Approx(110.f));
		CHECK(abs->GetBox().GetSize(BoxArea::Border).y == doctest::Approx(24.f));
		CHECK(abs->GetRelativeOffset(BoxArea::Border).x == doctest::Approx(5.f + 22.f));
		CHECK(abs->GetRelativeOffset(BoxArea::Border).y == doctest::Approx(5.f + 4.f));
		CHECK(far->GetRelativeOffset(BoxArea::Border).x == doctest::Approx(5.f + 220.f - 10.f - 20.f));
		CHECK(far->GetRelativeOffset(BoxArea::Border).y == doctest::Approx(5.f + 120.f - 12.f - 10.f));
	};
	check_padding_box_layout();

	// The containing block given by the formatting box: 100 x 50 at (30, 40) in #cb's border box.
	{
		const Rectanglef area = Rectanglef::FromPositionSize({30.f, 40.f}, {100.f, 50.f});
		RootBox root(Vector2f(800.f, 600.f));
		TestContainer container(cb, &root);
		container.AddAbsoluteElement(abs, Vector2f(0.f), cb, area);
		container.AddAbsoluteElement(far, Vector2f(0.f), cb, area);
		container.Close();
	}
	CHECK(abs->GetContainingBlock().x == doctest::Approx(100.f));
	CHECK(abs->GetContainingBlock().y == doctest::Approx(50.f));
	CHECK(abs->GetBox().GetSize(BoxArea::Border).x == doctest::Approx(50.f));
	CHECK(abs->GetBox().GetSize(BoxArea::Border).y == doctest::Approx(10.f));
	CHECK(abs->GetRelativeOffset(BoxArea::Border).x == doctest::Approx(30.f + 10.f));
	CHECK(abs->GetRelativeOffset(BoxArea::Border).y == doctest::Approx(40.f + 4.f));
	CHECK(far->GetContainingBlock().x == doctest::Approx(100.f));
	CHECK(far->GetContainingBlock().y == doctest::Approx(50.f));
	CHECK(far->GetRelativeOffset(BoxArea::Border).x == doctest::Approx(30.f + 100.f - 10.f - 20.f));
	CHECK(far->GetRelativeOffset(BoxArea::Border).y == doctest::Approx(40.f + 50.f - 5.f - 10.f));

	// The next layout without an override: the padding box again.
	cb->GetParentNode()->SetProperty("padding-top", "0px"); // (dirties the layout)
	context->Update();
	check_padding_box_layout();

	document->Close();
	TestsShell::ShutdownShell();
}
