#include "../../../Source/Core/DataModel.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

// Fork, S17 (docs/html-ui/FORK.md P7-D, P7-L, P7-T): what the engine's virtual views (<list-view> and the like) need from
// the fork. Row aliases a pooled row re-points without making its content again, layout boundaries a change inside a
// scroll box lays out alone, and the inert <template> of an RML document that holds a view's row markup.

namespace {

const String rows_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; }
	div, p, span { display: block; }
</style>
</head>
<body data-model="rows">
<div id="row_a"></div>
<div id="row_b"></div>
</body>
</rml>
)";

DataAddress Address(const String& name, int index)
{
	DataAddress address;
	address.emplace_back(name);
	address.emplace_back(index);
	return address;
}

DataAddress Literal(int value)
{
	DataAddress address;
	address.emplace_back("literal");
	address.emplace_back("int");
	address.emplace_back(value);
	return address;
}

} // namespace

TEST_CASE("VirtualViews.row_aliases")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	DataModelConstructor constructor = context->CreateDataModel("rows");
	REQUIRE(constructor);
	REQUIRE(constructor.RegisterArray<Vector<String>>());
	Vector<String> rows = {"zero", "one", "two", "three"};
	int picked = -1;
	constructor.Bind("rows", &rows);
	constructor.Bind("picked", &picked);
	DataModelHandle handle = constructor.GetModelHandle();

	ElementDocument* document = context->LoadDocumentFromMemory(rows_rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	Element* row_a = document->GetElementById("row_a");
	Element* row_b = document->GetElementById("row_b");
	DataModel* model = row_a->GetDataModel();
	REQUIRE(model);

	const int item_a = model->InsertRowAlias(row_a, "item", Address("rows", 2));
	const int index_a = model->InsertRowAlias(row_a, "i", Literal(2));
	const int item_b = model->InsertRowAlias(row_b, "item", Address("rows", 0));
	const int index_b = model->InsertRowAlias(row_b, "i", Literal(0));
	REQUIRE(item_a >= 0);
	REQUIRE(index_b >= 0);
	CHECK(model->InsertRowAlias(row_a, "bad", DataAddress()) == -1);
	const String content = R"(<p class="text">{{ item }}</p><span class="index" data-event-click="picked = i">{{ i }}</span>)";
	row_a->SetInnerRML(content);
	row_b->SetInnerRML(content);
	TestsShell::RenderLoop();

	Element* text_a = row_a->GetChild(0);
	Element* index_element_a = row_a->GetChild(1);
	CHECK(text_a->GetInnerRML() == "two");
	CHECK(index_element_a->GetInnerRML() == "2");
	CHECK(row_b->GetChild(0)->GetInnerRML() == "zero");

	// Re-pointed: the same elements show the other row; the other pooled row is left alone.
	model->SetRowAlias(item_a, Address("rows", 3));
	model->SetRowAlias(index_a, Literal(3));
	CHECK(model->GetRowAlias(item_a).back().index == 3);
	TestsShell::RenderLoop();
	CHECK(row_a->GetChild(0) == text_a);
	CHECK(row_a->GetChild(1) == index_element_a);
	CHECK(text_a->GetInnerRML() == "three");
	CHECK(index_element_a->GetInnerRML() == "3");
	CHECK(row_b->GetChild(0)->GetInnerRML() == "zero");

	// Events read the current target.
	index_element_a->DispatchEvent(EventId::Click, Dictionary());
	TestsShell::RenderLoop();
	CHECK(picked == 3);

	// The array's variable still updates the rows.
	rows[0] = "nil";
	handle.DirtyVariable("rows");
	TestsShell::RenderLoop();
	CHECK(row_b->GetChild(0)->GetInnerRML() == "nil");

	// Re-pointing to the same target changes nothing; an unknown handle is ignored.
	model->SetRowAlias(item_b, Address("rows", 0));
	model->SetRowAlias(12345, Address("rows", 1));
	model->SetRowAlias(index_b, Literal(1));
	model->SetRowAlias(item_b, Address("rows", 1));
	TestsShell::RenderLoop();
	CHECK(row_b->GetChild(0)->GetInnerRML() == "one");
	CHECK(row_b->GetChild(1)->GetInnerRML() == "1");

	// A removed row frees its aliases.
	row_b->GetParentNode()->RemoveChild(row_b);
	CHECK(model->GetRowAlias(item_b).empty());
	TestsShell::RenderLoop();

	document->Close();
	TestsShell::ShutdownShell();
}

namespace {

const String boundary_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 400px; height: 300px; font-family: LatoLatin; font-size: 10px; }
	div, p { display: block; }
	#view { position: relative; overflow: auto; width: 200px; height: 100px; }
	#sizer { height: 1000px; }
	#row { position: absolute; left: 0; right: 0; top: 40px; }
	#after { height: 20px; }
</style>
</head>
<body>
<div id="view"><div id="sizer"><div id="row"><p id="text">short</p></div></div></div>
<div id="after">after</div>
</body>
</rml>
)";

} // namespace

TEST_CASE("VirtualViews.layout_boundary")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(boundary_rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	Element* view = document->GetElementById("view");
	Element* text = document->GetElementById("text");
	Element* after = document->GetElementById("after");
	view->SetLayoutBoundary(true);
	CHECK(view->IsLayoutBoundary());
	TestsShell::RenderLoop();

	const Vector2f view_offset = view->GetAbsoluteOffset();
	const Vector2f after_offset = after->GetAbsoluteOffset();
	const float one_line = text->GetBox().GetSize().y;
	REQUIRE(one_line > 0.f);

	// A change inside the boundary formats it alone; what is outside it stays.
	const int partial_before = ElementDocument::GetBoundaryLayoutCount();
	text->SetInnerRML("a much longer text that wraps onto several lines inside the two hundred pixel view");
	TestsShell::RenderLoop();
	CHECK(ElementDocument::GetBoundaryLayoutCount() > partial_before);
	CHECK(text->GetBox().GetSize().y > one_line);
	CHECK(view->GetAbsoluteOffset() == view_offset);
	CHECK(after->GetAbsoluteOffset() == after_offset);

	// The partial result is the full layout's.
	const Box partial_text = text->GetBox();
	const Vector2f partial_text_offset = text->GetAbsoluteOffset();
	const float partial_scroll_height = view->GetScrollHeight();
	// an empty element appended at the end of the document: a full layout that moves nothing
	Element* empty = document->AppendChild(document->CreateElement("div"));
	TestsShell::RenderLoop();
	CHECK(text->GetBox() == partial_text);
	CHECK(text->GetAbsoluteOffset() == partial_text_offset);
	CHECK(view->GetScrollHeight() == partial_scroll_height);
	document->RemoveChild(empty);
	TestsShell::RenderLoop();

	// A change of the boundary itself or outside it lays the document out.
	const int partial_count = ElementDocument::GetBoundaryLayoutCount();
	view->SetProperty("height", "150px");
	TestsShell::RenderLoop();
	CHECK(ElementDocument::GetBoundaryLayoutCount() == partial_count);
	CHECK(after->GetAbsoluteOffset().y == doctest::Approx(after_offset.y + 50.f));

	// Scrolled: a change inside keeps the scroll offset.
	view->SetScrollTop(300.f);
	TestsShell::RenderLoop();
	text->SetInnerRML("short again");
	TestsShell::RenderLoop();
	CHECK(view->GetScrollTop() == doctest::Approx(300.f));

	// No boundary any more: changes inside lay the document out.
	view->SetLayoutBoundary(false);
	TestsShell::RenderLoop();
	const int partial_off = ElementDocument::GetBoundaryLayoutCount();
	text->SetInnerRML("x");
	TestsShell::RenderLoop();
	CHECK(ElementDocument::GetBoundaryLayoutCount() == partial_off);

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("VirtualViews.inert_template")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	const String rml = R"(
<rml>
<head></head>
<body>
<div id="view"><template><p class="row">{{ item.name }}<b>!</b></p></template><span id="next">next</span></div>
<div id="empty"><template id="t2" class="c"></template></div>
</body>
</rml>
)";
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	Element* view = document->GetElementById("view");
	REQUIRE(view->GetNumChildren() == 2);
	Element* row_template = view->GetChild(0);
	CHECK(row_template->GetTagName() == "template");
	CHECK(row_template->GetNumChildren() == 0);
	CHECK(row_template->GetAttribute<String>("rmlui-inner-rml", "") == R"(<p class="row">{{ item.name }}<b>!</b></p>)");
	CHECK(view->GetChild(1)->GetId() == "next");

	Element* empty = document->GetElementById("t2");
	REQUIRE(empty);
	CHECK(empty->GetAttribute<String>("class", "") == "c");
	CHECK(empty->GetNumChildren() == 0);

	document->Close();
	TestsShell::ShutdownShell();
}
