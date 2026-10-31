#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <doctest.h>

using namespace Rml;

// Fork, engine-gaps (docs/html-ui/FORK.md G5-1..G5-6): DOM Pointer Events. Every pointer has its own hover, active
// chain, capture, drag and click; touch pointers are captured by their pointerdown target, pan their scroll container
// where 'touch-action' allows it, and dispatch the compatibility mouse events too.

namespace {

const String document_rml = R"(
<rml>
<head>
<style>
	body { display: block; width: 600px; height: 400px; }
	div { display: block; position: absolute; width: 100px; height: 100px; }
	#a { left: 0; top: 0; }
	#b { left: 200px; top: 0; }
	#da { left: 0; top: 200px; drag: drag; }
	#db { left: 200px; top: 200px; drag: drag; }
	#scroller { left: 400px; top: 0; width: 100px; height: 100px; overflow: auto; }
	#content { position: static; width: 300px; height: 300px; }
	.none { touch-action: none; }
	.pany { touch-action: pan-y; }
</style>
</head>
<body><div id="a"/><div id="b"/><div id="da"/><div id="db"/><div id="scroller"><div id="content"/></div></body>
</rml>
)";

// "<type>:<pointer_id>" of every event it hears
struct Recorder : EventListener {
	Vector<String> events;
	bool prevent_pointerdown = false;
	PointerId capture_on_down = 0;
	void ProcessEvent(Event& event) override
	{
		events.push_back(event.GetType() + ":" + ToString(event.GetParameter("pointer_id", 0)));
		if (event.GetId() == EventId::Pointerdown && prevent_pointerdown)
			event.PreventDefault();
		if (event.GetId() == EventId::Pointerdown && capture_on_down != 0)
			event.GetCurrentElement()->SetPointerCapture(capture_on_down);
	}
	String Joined() const
	{
		String out;
		for (const String& e : events)
			out += (out.empty() ? "" : " ") + e;
		return out;
	}
};

PointerInput Touch(PointerId id, float x, float y)
{
	PointerInput input;
	input.id = id;
	input.type = PointerType::Touch;
	input.position = {x, y};
	return input;
}

PointerInput Mouse(float x, float y, int button = 0)
{
	PointerInput input;
	input.position = {x, y};
	input.button = button;
	return input;
}

void Listen(Element* element, Recorder& recorder, std::initializer_list<EventId> ids)
{
	for (EventId id : ids)
		element->AddEventListener(id, &recorder);
}

struct Fixture {
	Context* context = TestsShell::GetContext();
	ElementDocument* document = nullptr;
	Fixture()
	{
		document = context->LoadDocumentFromMemory(document_rml);
		REQUIRE(document);
		document->Show();
		context->Update();
		context->Render();
	}
	~Fixture()
	{
		document->Close();
		context->Update();
		TestsShell::ShutdownShell();
	}
	Element* Get(const char* id) { return document->GetElementById(id); }
};

} // namespace

TEST_CASE("PointerEvents.per_pointer_active_and_hover")
{
	Fixture f;
	Element* a = f.Get("a");
	Element* b = f.Get("b");

	f.context->ProcessPointerDown(Touch(2, 50, 50));
	f.context->ProcessPointerDown(Touch(3, 250, 50));
	CHECK(a->IsPseudoClassSet("active"));
	CHECK(b->IsPseudoClassSet("active"));
	CHECK(a->IsPseudoClassSet("hover"));
	CHECK(b->IsPseudoClassSet("hover"));
	CHECK(f.context->GetHoverElement(2) == a);
	CHECK(f.context->GetHoverElement(3) == b);

	// lifting one finger releases only its own element; a finger has no hover after its up
	f.context->ProcessPointerUp(Touch(2, 50, 50));
	CHECK(!a->IsPseudoClassSet("active"));
	CHECK(!a->IsPseudoClassSet("hover"));
	CHECK(b->IsPseudoClassSet("active"));
	CHECK(b->IsPseudoClassSet("hover"));
	CHECK(!f.context->HasPointer(2));

	// the mouse over b keeps its :hover after the finger goes (counted)
	f.context->ProcessMouseMove(260, 60, 0);
	f.context->ProcessPointerUp(Touch(3, 250, 50));
	CHECK(!b->IsPseudoClassSet("active"));
	CHECK(b->IsPseudoClassSet("hover"));
}

TEST_CASE("PointerEvents.event_order_and_compat_mouse_events")
{
	Fixture f;
	Element* a = f.Get("a");
	Recorder rec;
	Listen(a, rec,
		{EventId::Pointerover, EventId::Pointerenter, EventId::Pointerdown, EventId::Pointerup, EventId::Pointerout, EventId::Pointerleave,
			EventId::Mousedown, EventId::Mouseup, EventId::Click, EventId::Mouseover, EventId::Mouseout, EventId::Mouseenter, EventId::Mouseleave});

	f.context->ProcessPointerDown(Touch(4, 50, 50));
	f.context->ProcessPointerUp(Touch(4, 50, 50));
	CHECK(rec.Joined() ==
		"pointerover:4 mouseover:4 pointerenter:4 mouseenter:4 pointerdown:4 mousedown:4 pointerup:4 mouseup:4 click:4 "
		"pointerout:4 mouseout:4 pointerleave:4 mouseleave:4");

	// the mouse: pointer 1
	rec.events.clear();
	f.context->ProcessMouseMove(50, 50, 0);
	f.context->ProcessMouseButtonDown(0, 0);
	f.context->ProcessMouseButtonUp(0, 0);
	CHECK(rec.Joined() == "pointerover:1 mouseover:1 pointerenter:1 mouseenter:1 pointerdown:1 mousedown:1 pointerup:1 mouseup:1 click:1");
	f.context->ProcessMouseLeave();
}

TEST_CASE("PointerEvents.prevent_default_on_pointerdown")
{
	Fixture f;
	Element* a = f.Get("a");
	Recorder rec;
	rec.prevent_pointerdown = true;
	Listen(a, rec, {EventId::Pointerdown, EventId::Mousedown, EventId::Mouseup, EventId::Click});
	f.context->ProcessMouseMove(50, 50, 0);
	f.context->ProcessMouseButtonDown(0, 0);
	f.context->ProcessMouseButtonUp(0, 0);
	CHECK(rec.Joined() == "pointerdown:1 mouseup:1 click:1");
	f.context->ProcessMouseLeave();
}

TEST_CASE("PointerEvents.implicit_capture_of_touches")
{
	Fixture f;
	Element* a = f.Get("a");
	Element* b = f.Get("b");
	Recorder rec_a, rec_b;
	Listen(a, rec_a, {EventId::Gotpointercapture, EventId::Pointermove, EventId::Pointerup, EventId::Lostpointercapture});
	Listen(b, rec_b, {EventId::Pointermove, EventId::Pointerup});

	f.context->ProcessPointerDown(Touch(5, 50, 50));
	CHECK(a->HasPointerCapture(5));
	f.context->ProcessPointerMove(Touch(5, 250, 50));
	// the capture gets the move; :hover follows the real hit
	CHECK(rec_a.Joined() == "gotpointercapture:5 pointermove:5");
	CHECK(rec_b.events.empty());
	CHECK(b->IsPseudoClassSet("hover"));
	CHECK(!a->IsPseudoClassSet("hover"));
	f.context->ProcessPointerUp(Touch(5, 250, 50));
	CHECK(rec_a.Joined() == "gotpointercapture:5 pointermove:5 pointerup:5 lostpointercapture:5");
	CHECK(rec_b.events.empty());
}

TEST_CASE("PointerEvents.explicit_capture_of_the_mouse")
{
	Fixture f;
	Element* a = f.Get("a");
	Element* b = f.Get("b");
	Recorder rec;
	rec.capture_on_down = MousePointerId;
	Listen(a, rec, {EventId::Pointerdown, EventId::Gotpointercapture, EventId::Pointermove, EventId::Pointerup, EventId::Lostpointercapture});

	// without a press the mouse cannot be captured
	CHECK(!a->SetPointerCapture(MousePointerId));
	f.context->ProcessMouseMove(50, 50, 0);
	f.context->ProcessMouseButtonDown(0, 0);
	CHECK(a->HasPointerCapture(MousePointerId));
	f.context->ProcessMouseMove(260, 50, 0);
	CHECK(f.context->GetHoverElement() == b);
	f.context->ProcessMouseButtonUp(0, 0);
	CHECK(rec.Joined() == "pointerdown:1 gotpointercapture:1 pointermove:1 pointerup:1 lostpointercapture:1");
	CHECK(!a->HasPointerCapture(MousePointerId));
	f.context->ProcessMouseLeave();
}

TEST_CASE("PointerEvents.per_pointer_click")
{
	Fixture f;
	Element* a = f.Get("a");
	Element* b = f.Get("b");
	Recorder rec_a, rec_b;
	Listen(a, rec_a, {EventId::Click, EventId::Dblclick});
	Listen(b, rec_b, {EventId::Click});

	// two fingers click two elements; released elsewhere, no click
	f.context->ProcessPointerDown(Touch(2, 50, 50));
	f.context->ProcessPointerDown(Touch(3, 250, 50));
	f.context->ProcessPointerUp(Touch(3, 250, 50));
	f.context->ProcessPointerUp(Touch(2, 50, 50));
	CHECK(rec_a.Joined() == "click:2");
	CHECK(rec_b.Joined() == "click:3");

	f.context->ProcessPointerDown(Touch(2, 50, 50));
	CHECK(rec_a.Joined() == "click:2 dblclick:2");
	f.context->ProcessPointerUp(Touch(2, 260, 50));
	CHECK(rec_a.Joined() == "click:2 dblclick:2");
}

TEST_CASE("PointerEvents.per_pointer_drag")
{
	Fixture f;
	Element* da = f.Get("da");
	Element* db = f.Get("db");
	Recorder rec_a, rec_b;
	Listen(da, rec_a, {EventId::Dragstart, EventId::Dragend});
	Listen(db, rec_b, {EventId::Dragstart, EventId::Dragend});

	f.context->ProcessPointerDown(Touch(2, 50, 250));
	f.context->ProcessPointerDown(Touch(3, 250, 250));
	f.context->ProcessPointerMove(Touch(2, 60, 260));
	f.context->ProcessPointerMove(Touch(3, 260, 260));
	CHECK(rec_a.Joined() == "dragstart:2");
	CHECK(rec_b.Joined() == "dragstart:3");
	f.context->ProcessPointerUp(Touch(2, 60, 260));
	CHECK(rec_a.Joined() == "dragstart:2 dragend:2");
	CHECK(rec_b.Joined() == "dragstart:3");
	f.context->ProcessPointerUp(Touch(3, 260, 260));
	CHECK(rec_b.Joined() == "dragstart:3 dragend:3");
}

TEST_CASE("PointerEvents.touch_action_pan")
{
	Fixture f;
	Element* scroller = f.Get("scroller");
	Element* content = f.Get("content");
	Recorder rec;
	Listen(content, rec, {EventId::Pointercancel, EventId::Pointermove, EventId::Pointerup});
	f.context->Update();
	f.context->Render();

	SUBCASE("auto pans and cancels the pointer")
	{
		f.context->ProcessPointerDown(Touch(2, 450, 80));
		CHECK(content->IsPseudoClassSet("active"));
		f.context->ProcessPointerMove(Touch(2, 450, 40));
		CHECK(rec.Joined() == "pointercancel:2");
		CHECK(!content->IsPseudoClassSet("active"));
		CHECK(scroller->GetScrollTop() > 0.f);
		f.context->ProcessPointerMove(Touch(2, 450, 30));
		f.context->ProcessPointerUp(Touch(2, 450, 30));
		CHECK(rec.Joined() == "pointercancel:2");
	}
	SUBCASE("none does not pan")
	{
		content->SetClass("none", true);
		f.context->Update();
		f.context->ProcessPointerDown(Touch(2, 450, 80));
		f.context->ProcessPointerMove(Touch(2, 450, 40));
		f.context->ProcessPointerUp(Touch(2, 450, 40));
		CHECK(rec.Joined() == "pointermove:2 pointerup:2");
		CHECK(scroller->GetScrollTop() == 0.f);
	}
	SUBCASE("pan-y on the container: a horizontal move does not pan")
	{
		scroller->SetClass("pany", true);
		f.context->Update();
		f.context->ProcessPointerDown(Touch(2, 480, 50));
		f.context->ProcessPointerMove(Touch(2, 420, 50));
		CHECK(rec.Joined() == "pointermove:2");
		f.context->ProcessPointerMove(Touch(2, 420, 10));
		f.context->ProcessPointerUp(Touch(2, 420, 10));
		CHECK_MESSAGE(rec.Joined() == "pointermove:2 pointermove:2 pointerup:2", "a horizontal pan is not allowed: the pointer stays");
	}
}
