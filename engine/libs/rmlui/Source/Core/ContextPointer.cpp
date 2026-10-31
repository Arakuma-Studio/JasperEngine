// Fork (engine-gaps G5-1..G5-6): DOM Pointer Events in the context. Every pointer has its own hover chain, active chain,
// capture, drag and click; ':hover' and ':active' are counted over the pointers. The mouse is pointer 1 and keeps the
// context's original members (Context.cpp); this file has the touch and pen pointers, the capture API, the counted
// pseudo-classes and the event parameters.

#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "ElementCascade.h"
#include "ScrollController.h"
#include <algorithm>

namespace Rml {

static constexpr float POINTER_DOUBLE_CLICK_TIME = 0.5f;   // [s] as the mouse's
static constexpr float POINTER_DOUBLE_CLICK_MAX_DIST = 3.f; // [dp]
static constexpr float POINTER_TOUCH_SLOP = 3.f;            // [dp] TOUCH_CLICK_MAX_DISTANCE
static constexpr float POINTER_SCROLL_INERTIA_DELAY = 0.1f;
static constexpr float POINTER_TOUCH_MOVEMENT_DECAY_RATE = 5.0f;

namespace {
	const char* PointerTypeName(PointerType type)
	{
		switch (type)
		{
		case PointerType::Mouse: return "mouse";
		case PointerType::Pen: return "pen";
		default: return "touch";
		}
	}

	// the element and its ancestors, innermost first
	Vector<Element*> AncestorsOf(Element* element)
	{
		Vector<Element*> list;
		for (; element; element = element->GetParentNode())
			list.push_back(element);
		return list;
	}

	Element* FindPointerFocusElement(Element* element)
	{
		ElementDocument* owner_document = element ? element->GetOwnerDocument() : nullptr;
		if (!owner_document || owner_document->GetComputedValues().focus() == Style::Focus::None)
			return nullptr;
		while (element && ElementCascade::IsUnfocusable(element))
			element = element->GetParentNode();
		return element;
	}

	// 'touch-action' as a mask: bit 0 pan-x, bit 1 pan-y (auto and manipulation: both)
	int TouchActionMask(Element* element)
	{
		const Property* property = element->GetLocalProperty(PropertyId::TouchAction);
		if (!property)
			return 3;
		const String value = StringUtilities::ToLower(property->Get<String>());
		if (value == "none")
			return 0;
		if (value == "auto" || value == "manipulation" || value.empty())
			return 3;
		int mask = 0;
		StringList parts;
		StringUtilities::ExpandString(parts, value, ' ');
		for (const String& part : parts)
		{
			if (part == "pan-x" || part == "pan-left" || part == "pan-right")
				mask |= 1;
			else if (part == "pan-y" || part == "pan-up" || part == "pan-down")
				mask |= 2;
			else if (part == "pinch-zoom")
				continue;
			else
				return 3; // an unknown value: auto
		}
		return mask;
	}
} // namespace

// ---- ':hover' / ':active' counted over the pointers

void Context::AddHoverRef(Element* element)
{
	if (++hover_refs[element] == 1)
		element->SetPseudoClass("hover", true);
}

void Context::RemoveHoverRef(Element* element)
{
	auto it = hover_refs.find(element);
	if (it == hover_refs.end())
		return;
	if (--it->second <= 0)
	{
		hover_refs.erase(it);
		element->SetPseudoClass("hover", false);
	}
}

void Context::AddActiveRef(Element* element)
{
	if (++active_refs[element] == 1)
		element->SetPseudoClass("active", true);
}

void Context::RemoveActiveRef(Element* element)
{
	auto it = active_refs.find(element);
	if (it == active_refs.end())
		return;
	if (--it->second <= 0)
	{
		active_refs.erase(it);
		element->SetPseudoClass("active", false);
	}
}

// ---- event parameters

void Context::GeneratePointerEventParameters(Dictionary& parameters, PointerId id, PointerType type, bool primary, Vector2f position, int button,
	int buttons, float pressure, float width, float height, int key_modifier_state)
{
	parameters["mouse_x"] = (int)position.x;
	parameters["mouse_y"] = (int)position.y;
	parameters["pointer_id"] = id;
	parameters["pointer_type"] = String(PointerTypeName(type));
	parameters["is_primary"] = primary ? 1 : 0;
	parameters["button"] = button;
	parameters["buttons"] = buttons;
	parameters["pressure"] = (pressure >= 0.f ? pressure : (buttons != 0 ? 0.5f : 0.f));
	parameters["width"] = width;
	parameters["height"] = height;
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);
}

void Context::GenerateMousePointerParameters(Dictionary& parameters, int button_index, int key_modifier_state)
{
	GeneratePointerEventParameters(parameters, MousePointerId, PointerType::Mouse, true, Vector2f(mouse_position), button_index, mouse_buttons, -1.f,
		1.f, 1.f, key_modifier_state);
}

// ---- hover chains

void Context::SwapHoverChain(Element* old_hover, Element* new_hover, ElementSet& chain, ElementSet& new_chain, Dictionary& parameters,
	bool compat_events)
{
	using ObserverList = Vector<ObserverPtr<Element>>;
	auto observe = [](const Vector<Element*>& elements) {
		ObserverList list;
		for (Element* element : elements)
			list.push_back(element->GetObserverPtr());
		return list;
	};

	// the leaving elements innermost first, the entering ones outermost first
	Vector<Element*> leaving, entering;
	for (Element* element : AncestorsOf(old_hover))
		if (chain.count(element) && !new_chain.count(element))
			leaving.push_back(element);
	for (Element* element : chain)
		if (!new_chain.count(element) && std::find(leaving.begin(), leaving.end(), element) == leaving.end())
			leaving.push_back(element); // detached in between: no longer an ancestor of the old hover
	Vector<Element*> new_ancestors = AncestorsOf(new_hover);
	for (auto it = new_ancestors.rbegin(); it != new_ancestors.rend(); ++it)
		if (new_chain.count(*it) && !chain.count(*it))
			entering.push_back(*it);

	ObserverList leaving_obs = observe(leaving);
	ObserverList entering_obs = observe(entering);
	ObserverPtr<Element> old_obs = old_hover ? old_hover->GetObserverPtr() : ObserverPtr<Element>();
	ObserverPtr<Element> new_obs = new_hover ? new_hover->GetObserverPtr() : ObserverPtr<Element>();
	const bool target_changed = old_hover != new_hover;

	ElementSet old_chain;
	old_chain.swap(chain);
	chain = new_chain;

	// the DOM's order: the pointer events, then the compatibility mouse events
	// (pointerout, pointerleave, mouseout, mouseleave; pointerover, pointerenter, mouseover, mouseenter)
	if (target_changed && old_obs)
		old_obs->DispatchEvent(EventId::Pointerout, parameters);
	for (auto& element : leaving_obs)
		if (element)
			element->DispatchEvent(EventId::Pointerleave, parameters);
	if (compat_events)
	{
		SendEvents(old_chain, new_chain, EventId::Mouseout, parameters);
		for (auto& element : leaving_obs)
			if (element)
				element->DispatchEvent(EventId::Mouseleave, parameters);
	}
	for (auto& element : leaving_obs)
		if (element)
			RemoveHoverRef(element.get());
	for (Element* element : old_chain)
		if (!new_chain.count(element) && std::find(leaving.begin(), leaving.end(), element) == leaving.end())
			RemoveHoverRef(element);

	for (auto& element : entering_obs)
		if (element)
			AddHoverRef(element.get());
	if (target_changed && new_obs)
		new_obs->DispatchEvent(EventId::Pointerover, parameters);
	for (auto& element : entering_obs)
		if (element)
			element->DispatchEvent(EventId::Pointerenter, parameters);
	if (compat_events)
	{
		SendEvents(new_chain, old_chain, EventId::Mouseover, parameters);
		for (auto& element : entering_obs)
			if (element)
				element->DispatchEvent(EventId::Mouseenter, parameters);
	}
}

// ---- capture

Context::PointerState* Context::LookupPointer(PointerId id)
{
	auto it = pointers.find(id);
	return it != pointers.end() ? &it->second : nullptr;
}

bool Context::HasPointer(PointerId id) const
{
	return id == MousePointerId || pointers.find(id) != pointers.end();
}

bool Context::SetPointerCapture(Element* element, PointerId id)
{
	if (!element || element->GetContext() != this)
		return false;
	if (id == MousePointerId)
	{
		if (mouse_buttons == 0)
			return false; // a pointer that is not pressed cannot be captured (DOM: active buttons)
		mouse_pending_capture = element;
		mouse_pending_capture_set = true;
		return true;
	}
	PointerState* state = LookupPointer(id);
	if (!state || state->panning)
		return false;
	state->pending_capture = element;
	state->pending_capture_set = true;
	return true;
}

bool Context::ReleasePointerCapture(Element* element, PointerId id)
{
	if (!HasPointerCapture(element, id))
		return false;
	if (id == MousePointerId)
	{
		mouse_pending_capture = nullptr;
		mouse_pending_capture_set = true;
		return true;
	}
	PointerState* state = LookupPointer(id);
	state->pending_capture = nullptr;
	state->pending_capture_set = true;
	return true;
}

bool Context::HasPointerCapture(const Element* element, PointerId id) const
{
	if (!element)
		return false;
	// a capture set but not yet processed counts (DOM: hasPointerCapture after setPointerCapture is true)
	if (id == MousePointerId)
		return mouse_pending_capture_set ? mouse_pending_capture == element : mouse_capture == element;
	auto it = pointers.find(id);
	if (it == pointers.end())
		return false;
	const PointerState& state = it->second;
	return state.pending_capture_set ? state.pending_capture == element : state.capture == element;
}

void Context::ProcessPendingCapture(PointerId id)
{
	Element** capture = nullptr;
	Element** pending = nullptr;
	bool* pending_set = nullptr;
	Dictionary parameters;
	if (id == MousePointerId)
	{
		capture = &mouse_capture;
		pending = &mouse_pending_capture;
		pending_set = &mouse_pending_capture_set;
		GenerateMousePointerParameters(parameters, -1, 0);
	}
	else
	{
		PointerState* state = LookupPointer(id);
		if (!state)
			return;
		capture = &state->capture;
		pending = &state->pending_capture;
		pending_set = &state->pending_capture_set;
		GeneratePointerEventParameters(parameters, id, state->type, state->primary, state->position, -1, state->buttons, -1.f, 1.f, 1.f, 0);
	}
	if (!*pending_set)
		return;
	*pending_set = false;
	Element* old_capture = *capture;
	Element* new_capture = *pending;
	*pending = nullptr;
	if (old_capture == new_capture)
		return;
	*capture = new_capture;
	ObserverPtr<Element> new_obs = new_capture ? new_capture->GetObserverPtr() : ObserverPtr<Element>();
	if (old_capture)
		old_capture->DispatchEvent(EventId::Lostpointercapture, parameters);
	if (new_obs)
		new_obs->DispatchEvent(EventId::Gotpointercapture, parameters);
}

Element* Context::GetPointerTarget(PointerId id)
{
	if (id == MousePointerId)
		return mouse_capture ? mouse_capture : hover;
	PointerState* state = LookupPointer(id);
	if (!state)
		return nullptr;
	return state->capture ? state->capture : state->hover;
}

Element* Context::GetHoverElement(PointerId id)
{
	if (id == MousePointerId)
		return hover;
	PointerState* state = LookupPointer(id);
	return state ? state->hover : nullptr;
}

bool Context::IsPointerInteracting(PointerId id) const
{
	if (id == MousePointerId)
		return IsMouseInteracting();
	auto it = pointers.find(id);
	if (it == pointers.end())
		return false;
	const PointerState& state = it->second;
	return (state.hover && state.hover != root.get()) || (state.active && state.active != root.get()) || state.panning || state.capture;
}

// ---- touch and pen pointers

void Context::ComputeTouchAction(PointerState& state, Element* target)
{
	// the intersection from the target up to (including) its nearest scroll container (CSS touch-action)
	int mask = 3;
	for (Element* element = target; element; element = element->GetParentNode())
	{
		mask &= TouchActionMask(element);
		if (element == state.scroll.scroll_container)
			break;
	}
	state.pan_x = (mask & 1) != 0;
	state.pan_y = (mask & 2) != 0;
}

void Context::UpdatePointerHover(PointerId id, PointerState& state, int key_modifier_state, bool moved)
{
	Dictionary parameters;
	GeneratePointerEventParameters(parameters, id, state.type, state.primary, state.position, -1, state.buttons, -1.f, 1.f, 1.f, key_modifier_state);

	Element* new_hover = GetElementAtPoint(state.position);
	ElementSet new_chain;
	for (Element* element = new_hover; element; element = element->GetParentNode())
		new_chain.insert(element);
	Element* old_hover = state.hover;
	state.hover = new_hover;
	ObserverPtr<Element> hover_obs = new_hover ? new_hover->GetObserverPtr() : ObserverPtr<Element>();
	SwapHoverChain(old_hover, new_hover, state.hover_chain, new_chain, parameters, true);

	PointerState* current = LookupPointer(id);
	if (!current || !moved)
		return;

	// drag (G5-5): start on the first move, then drag / dragover / dragout with this pointer's hover
	if (current->drag)
	{
		Dictionary drag_parameters = parameters;
		drag_parameters["drag_element"] = (void*)current->drag;
		if (!current->drag_started)
		{
			current->drag->DispatchEvent(EventId::Dragstart, drag_parameters);
			current = LookupPointer(id);
			if (!current || !current->drag)
				return;
			current->drag_started = true;
			if (current->drag->GetComputedValues().drag() == Style::Drag::Clone && cursor_proxy)
			{
				ElementPtr clone = current->drag->Clone();
				if (clone)
				{
					if (ElementDocument* document = current->drag->GetOwnerDocument())
						static_cast<ElementDocument&>(*cursor_proxy).SetStyleSheetContainer(document->style_sheet_container);
					current->drag_clone = clone.get();
					cursor_proxy->AppendChild(std::move(clone));
					Vector2f projected = current->position;
					if (Element* parent = current->drag->GetParentNode())
						parent->Project(projected);
					current->drag_clone_offset = current->drag->GetAbsoluteOffset(BoxArea::Border) - projected;
					current->drag_clone->SetProperty(PropertyId::Position, Property(Style::Position::Absolute));
					current->drag_clone->SetProperty(PropertyId::MarginLeft, Property(0.f, Unit::PX));
					current->drag_clone->SetProperty(PropertyId::MarginTop, Property(0.f, Unit::PX));
					current->drag_clone->SetPseudoClass("drag", true);
					PositionPointerDragClones();
				}
			}
		}
		current->drag->DispatchEvent(EventId::Drag, drag_parameters);
		current = LookupPointer(id);
		if (!current || !current->drag)
			return;

		current->drag_hover = GetElementAtPoint(current->position, current->drag);
		ElementSet new_drag_chain;
		for (Element* element = current->drag_hover; element; element = element->GetParentNode())
			new_drag_chain.insert(element);
		if (current->drag_started && current->drag_verbose)
		{
			ElementSet old_drag_chain = current->drag_hover_chain;
			SendEvents(old_drag_chain, new_drag_chain, EventId::Dragout, drag_parameters);
			SendEvents(new_drag_chain, old_drag_chain, EventId::Dragover, drag_parameters);
			current = LookupPointer(id);
			if (!current)
				return;
		}
		current->drag_hover_chain.swap(new_drag_chain);
		if (current->drag_hover && current->drag_verbose)
			current->drag_hover->DispatchEvent(EventId::Dragmove, drag_parameters);
	}
	(void)hover_obs;
}

void Context::PositionPointerDragClones()
{
	// the clones live in the cursor proxy, which follows the mouse: offset by the pointer's distance from it
	const Vector2f proxy_offset((float)Math::Clamp(mouse_position.x, 0, dimensions.x), (float)Math::Clamp(mouse_position.y, 0, dimensions.y));
	for (auto& pair : pointers)
	{
		PointerState& state = pair.second;
		if (!state.drag_clone)
			continue;
		const Vector2f at = state.position + state.drag_clone_offset - proxy_offset;
		state.drag_clone->SetProperty(PropertyId::Left, Property(at.x, Unit::PX));
		state.drag_clone->SetProperty(PropertyId::Top, Property(at.y, Unit::PX));
	}
}

bool Context::ProcessPointerDown(const PointerInput& input)
{
	if (input.type == PointerType::Mouse || input.id == MousePointerId)
	{
		ProcessMouseMove((int)input.position.x, (int)input.position.y, input.key_modifier_state);
		return ProcessMouseButtonDown(input.button, input.key_modifier_state);
	}

	// a down for a pointer still known (a lost up): that one ends first
	if (LookupPointer(input.id))
		ProcessPointerCancel(input.id);

	bool any_down = false;
	for (const auto& pair : pointers)
		any_down = any_down || pair.second.buttons != 0;

	PointerState& created = pointers[input.id];
	created.type = input.type;
	created.primary = !any_down; // the first of a multi-touch interaction (DOM)
	created.position = input.position;
	created.buttons = 1;
	const PointerId id = input.id;

	// panning: the scroll container under the press (a touch that pans scrolls it)
	Element* touch_element = GetElementAtPoint(input.position);
	created.scroll = TouchState();
	created.scroll.start_position = input.position;
	created.scroll.inertia_position = input.position;
	created.scroll.last_position = input.position;
	created.scroll.scrolling_last_time = GetSystemInterface()->GetElapsedTime();
	created.scroll.scroll_container = touch_element ? touch_element->GetClosestScrollableContainer() : nullptr;
	if (created.scroll.scroll_container && scroll_controller->GetTarget() == created.scroll.scroll_container)
		scroll_controller->Reset();
	ComputeTouchAction(created, touch_element);

	// a finger has no hover until it is down: it enters now
	UpdatePointerHover(id, created, input.key_modifier_state, false);
	PointerState* state = LookupPointer(id);
	if (!state)
		return true;

	Dictionary parameters;
	GeneratePointerEventParameters(parameters, id, state->type, state->primary, state->position, input.button, state->buttons, input.pressure,
		input.width, input.height, input.key_modifier_state);

	Element* target = state->hover;
	bool prevented = false;
	if (target)
	{
		// implicit capture of touch pointers (DOM): set before the down's listeners, so one can release it
		if (state->type == PointerType::Touch)
		{
			state->pending_capture = target;
			state->pending_capture_set = true;
		}
		target->DispatchEvent(EventId::Pointerdown, parameters, &prevented);
		state = LookupPointer(id);
		if (!state)
			return true;
	}

	// focus and the compatibility mousedown, unless the pointerdown was prevented
	Element* new_focus = state->hover ? FindPointerFocusElement(state->hover) : nullptr;
	if (!prevented && new_focus && new_focus != focus)
	{
		new_focus->Focus();
		state = LookupPointer(id);
		if (!state)
			return true;
	}
	state->active = new_focus;

	bool propagate = true;
	if (!prevented && state->hover)
	{
		Dictionary mouse_parameters = parameters;
		mouse_parameters["button"] = 0;
		propagate = state->hover->DispatchEvent(EventId::Mousedown, mouse_parameters);
		state = LookupPointer(id);
		if (!state)
			return true;

		if (propagate)
		{
			// double click per pointer (G5-6)
			PointerClick& click = pointer_clicks[id];
			const double click_time = GetSystemInterface()->GetElapsedTime();
			const float max_distance = POINTER_DOUBLE_CLICK_MAX_DIST * density_independent_pixel_ratio;
			if (state->active == click.element && float(click_time - click.time) < POINTER_DOUBLE_CLICK_TIME &&
				(state->position - click.position).SquaredMagnitude() < max_distance * max_distance)
			{
				if (state->hover)
					propagate = state->hover->DispatchEvent(EventId::Dblclick, mouse_parameters);
				state = LookupPointer(id);
				if (!state)
					return true;
				click = PointerClick();
			}
			else
			{
				click.element = state->active;
				click.time = click_time;
				click.position = state->position;
			}
		}
	}

	// ':active' on the pressed chain (where the point is within the element)
	for (Element* element : state->hover_chain)
	{
		if (element->IsPointWithinElement(state->position))
		{
			state->active_chain.push_back(element);
			AddActiveRef(element);
		}
	}

	// a drag="..." ancestor this pointer may drag (G5-5)
	if (propagate)
	{
		state->drag_started = false;
		Element* drag = state->hover;
		while (drag)
		{
			Style::Drag drag_style = drag->GetComputedValues().drag();
			if (drag_style == Style::Drag::None)
			{
				drag = drag->GetParentNode();
				continue;
			}
			if (drag_style == Style::Drag::Block)
				drag = nullptr;
			else
				state->drag_verbose = (drag_style == Style::Drag::DragDrop || drag_style == Style::Drag::Clone);
			break;
		}
		// a document or element another pointer drags is not dragged twice
		if (drag == this->drag)
			drag = nullptr;
		for (const auto& pair : pointers)
			if (pair.first != id && pair.second.drag == drag)
				drag = nullptr;
		state->drag = drag;
	}

	return !IsPointerInteracting(id);
}

bool Context::ProcessPointerMove(const PointerInput& input)
{
	if (input.type == PointerType::Mouse || input.id == MousePointerId)
		return ProcessMouseMove((int)input.position.x, (int)input.position.y, input.key_modifier_state);

	PointerState* state = LookupPointer(input.id);
	if (!state)
		return true; // a pointer that went down elsewhere does not hover
	const PointerId id = input.id;
	const Vector2f old_position = state->position;
	state->position = input.position;
	const double now = GetSystemInterface()->GetElapsedTime();

	// panning (G5-4)
	TouchState& scroll = state->scroll;
	if (scroll.scroll_container && !state->drag)
	{
		const Vector2f delta = input.position - scroll.last_position;
		const Vector2f from_start = input.position - scroll.start_position;
		const float slop = POINTER_TOUCH_SLOP * density_independent_pixel_ratio;
		if (!state->panning && from_start.SquaredMagnitude() >= slop * slop)
		{
			const bool horizontal = Math::Absolute(from_start.x) >= Math::Absolute(from_start.y);
			if ((horizontal && state->pan_x) || (!horizontal && state->pan_y))
			{
				CancelPointerPan(id, *state, input.key_modifier_state);
				state = LookupPointer(id);
				if (!state)
					return true;
			}
		}
		if (state->panning)
		{
			TouchState& pan = state->scroll;
			Vector2f scroll_delta = delta;
			if (!state->pan_x)
				scroll_delta.x = 0;
			if (!state->pan_y)
				scroll_delta.y = 0;
			if (scroll_delta.x != 0 || scroll_delta.y != 0)
				scroll_controller->InstantScrollOnTarget(pan.scroll_container, -scroll_delta);

			enum { Horizontal = 0, Vertical = 1 };
			for (int axis : {Horizontal, Vertical})
			{
				bool& positive = (axis == Horizontal ? pan.scrolling_right : pan.scrolling_down);
				double& start_time = (axis == Horizontal ? pan.scrolling_start_time_x : pan.scrolling_start_time_y);
				const bool scroll_start = (start_time == 0);
				const bool going_positive = (delta[axis] > 0);
				if (delta[axis] != 0 && (going_positive != positive || scroll_start))
				{
					pan.inertia_position[axis] = input.position[axis];
					positive = going_positive;
					start_time = now;
				}
				else
				{
					const float elapsed = static_cast<float>(now - start_time);
					const float weight = Math::Exp(-elapsed * POINTER_TOUCH_MOVEMENT_DECAY_RATE);
					pan.inertia_position[axis] = input.position[axis] - (input.position[axis] - pan.inertia_position[axis]) * weight;
					start_time = now - (now - start_time) * weight;
				}
			}
			pan.scrolling_last_time = now;
			pan.last_position = input.position;
			return false; // a pan is not passed on
		}
	}
	scroll.scrolling_last_time = now;
	scroll.last_position = input.position;

	ProcessPendingCapture(id);
	state = LookupPointer(id);
	if (!state)
		return true;
	const bool moved = old_position != input.position;
	UpdatePointerHover(id, *state, input.key_modifier_state, moved);
	state = LookupPointer(id);
	if (!state)
		return true;

	if (moved)
	{
		if (Element* target = GetPointerTarget(id))
		{
			Dictionary parameters;
			GeneratePointerEventParameters(parameters, id, state->type, state->primary, state->position, -1, state->buttons, input.pressure,
				input.width, input.height, input.key_modifier_state);
			ObserverPtr<Element> target_obs = target->GetObserverPtr();
			target->DispatchEvent(EventId::Pointermove, parameters);
			if (target_obs)
				target_obs->DispatchEvent(EventId::Mousemove, parameters);
		}
	}
	return !IsPointerInteracting(id);
}

void Context::CancelPointerPan(PointerId id, PointerState& state, int key_modifier_state)
{
	// a pan starts: the pointer is cancelled (DOM), loses ':active' and its capture, and only scrolls from here
	Dictionary parameters;
	GeneratePointerEventParameters(parameters, id, state.type, state.primary, state.position, -1, state.buttons, -1.f, 1.f, 1.f, key_modifier_state);
	Element* target = GetPointerTarget(id);
	ObserverPtr<Element> target_obs = target ? target->GetObserverPtr() : ObserverPtr<Element>();
	state.panning = true;
	for (Element* element : state.active_chain)
		RemoveActiveRef(element);
	state.active_chain.clear();
	state.active = nullptr;
	if (target_obs)
		target_obs->DispatchEvent(EventId::Pointercancel, parameters);
	PointerState* current = LookupPointer(id);
	if (!current)
		return;
	if (current->capture || current->pending_capture_set)
	{
		current->pending_capture = nullptr;
		current->pending_capture_set = true;
		ProcessPendingCapture(id);
		current = LookupPointer(id);
		if (!current)
			return;
	}
	// it no longer hovers
	ElementSet empty;
	Element* old_hover = current->hover;
	current->hover = nullptr;
	SwapHoverChain(old_hover, nullptr, current->hover_chain, empty, parameters, true);
	if (PointerState* again = LookupPointer(id))
	{
		again->scroll.inertia_position = again->position;
		again->scroll.scrolling_start_time_x = 0;
		again->scroll.scrolling_start_time_y = 0;
	}
}

void Context::EndPointer(PointerId id, bool released, Vector2f position, int key_modifier_state)
{
	PointerState* state = LookupPointer(id);
	if (!state)
		return;
	state->position = position;

	if (state->panning)
	{
		// the pan ends with inertia; no events (it was cancelled when it started)
		if (released)
		{
			TouchState& pan = state->scroll;
			const double now = GetSystemInterface()->GetElapsedTime();
			if (now - pan.scrolling_last_time < POINTER_SCROLL_INERTIA_DELAY && pan.scroll_container)
			{
				Vector2f velocity = -(position - pan.inertia_position);
				const float elapsed_x = static_cast<float>(now - pan.scrolling_start_time_x);
				const float elapsed_y = static_cast<float>(now - pan.scrolling_start_time_y);
				if (elapsed_x > 0)
					velocity.x /= elapsed_x;
				if (elapsed_y > 0)
					velocity.y /= elapsed_y;
				if (!state->pan_x)
					velocity.x = 0;
				if (!state->pan_y)
					velocity.y = 0;
				scroll_controller->ActivateInertia(pan.scroll_container, velocity);
			}
		}
		pointers.erase(id);
		return;
	}

	ProcessPendingCapture(id);
	state = LookupPointer(id);
	if (!state)
		return;
	if (released)
	{
		UpdatePointerHover(id, *state, key_modifier_state, false);
		state = LookupPointer(id);
		if (!state)
			return;
	}
	state->buttons = 0;

	Dictionary parameters;
	GeneratePointerEventParameters(parameters, id, state->type, state->primary, state->position, released ? 0 : -1, 0, 0.f, 1.f, 1.f,
		key_modifier_state);

	Element* target = GetPointerTarget(id);
	ObserverPtr<Element> target_obs = target ? target->GetObserverPtr() : ObserverPtr<Element>();
	if (target_obs)
		target_obs->DispatchEvent(released ? EventId::Pointerup : EventId::Pointercancel, parameters);
	state = LookupPointer(id);
	if (!state)
		return;

	if (released)
	{
		// the compatibility mouseup, then the click when released over the press's focus element (G5-6)
		Element* compat_target = GetPointerTarget(id);
		if (compat_target)
			compat_target->DispatchEvent(EventId::Mouseup, parameters);
		state = LookupPointer(id);
		if (!state)
			return;
		if (state->hover && state->active && state->active == FindPointerFocusElement(state->hover))
		{
			state->active->DispatchEvent(EventId::Click, parameters);
			state = LookupPointer(id);
			if (!state)
				return;
		}
	}

	for (Element* element : state->active_chain)
		RemoveActiveRef(element);
	state->active_chain.clear();
	state->active = nullptr;

	if (state->drag)
	{
		if (state->drag_started)
		{
			Dictionary drag_parameters = parameters;
			drag_parameters["drag_element"] = (void*)state->drag;
			if (released && state->drag_hover && state->drag_verbose)
			{
				ObserverPtr<Element> drag_hover_obs = state->drag_hover->GetObserverPtr();
				state->drag_hover->DispatchEvent(EventId::Dragdrop, drag_parameters);
				if (drag_hover_obs)
					drag_hover_obs->DispatchEvent(EventId::Dragout, drag_parameters);
				state = LookupPointer(id);
				if (!state)
					return;
			}
			if (state->drag)
				state->drag->DispatchEvent(EventId::Dragend, drag_parameters);
			state = LookupPointer(id);
			if (!state)
				return;
			if (state->drag_clone)
			{
				cursor_proxy->RemoveChild(state->drag_clone);
				state->drag_clone = nullptr;
			}
		}
		state->drag = nullptr;
		state->drag_hover = nullptr;
		state->drag_hover_chain.clear();
	}

	// the capture goes with the release (lostpointercapture), then the pointer leaves (touch: no sticky hover)
	if (state->capture)
	{
		state->pending_capture = nullptr;
		state->pending_capture_set = true;
		ProcessPendingCapture(id);
		state = LookupPointer(id);
		if (!state)
			return;
	}
	ElementSet empty;
	Element* old_hover = state->hover;
	state->hover = nullptr;
	SwapHoverChain(old_hover, nullptr, state->hover_chain, empty, parameters, true);
	pointers.erase(id);
}

bool Context::ProcessPointerUp(const PointerInput& input)
{
	if (input.type == PointerType::Mouse || input.id == MousePointerId)
	{
		ProcessMouseMove((int)input.position.x, (int)input.position.y, input.key_modifier_state);
		return ProcessMouseButtonUp(input.button, input.key_modifier_state);
	}
	if (!LookupPointer(input.id))
		return true;
	// the interaction state before the release, so a released pointer is considered to have taken it
	const bool result = !IsPointerInteracting(input.id);
	EndPointer(input.id, true, input.position, input.key_modifier_state);
	return result;
}

bool Context::ProcessPointerCancel(PointerId id)
{
	if (id == MousePointerId)
	{
		// the mouse: its buttons let go without a click (no compatibility event)
		Dictionary parameters;
		GenerateMousePointerParameters(parameters, -1, 0);
		Element* target = GetPointerTarget(MousePointerId);
		mouse_buttons = 0;
		if (target)
			target->DispatchEvent(EventId::Pointercancel, parameters);
		mouse_pending_capture = nullptr;
		mouse_pending_capture_set = true;
		ProcessPendingCapture(MousePointerId);
		ResetActiveChain();
		if (drag)
		{
			if (drag_started)
				ReleaseDragClone();
			drag = nullptr;
			drag_hover = nullptr;
			drag_hover_chain.clear();
		}
		return !IsMouseInteracting();
	}
	PointerState* state = LookupPointer(id);
	if (!state)
		return true;
	EndPointer(id, false, state->position, 0);
	return true;
}

bool Context::ProcessPointerLeave(PointerId id)
{
	if (id == MousePointerId)
		return ProcessMouseLeave();
	PointerState* state = LookupPointer(id);
	if (!state)
		return true;
	Dictionary parameters;
	GeneratePointerEventParameters(parameters, id, state->type, state->primary, state->position, -1, state->buttons, -1.f, 1.f, 1.f, 0);
	ElementSet empty;
	Element* old_hover = state->hover;
	state->hover = nullptr;
	SwapHoverChain(old_hover, nullptr, state->hover_chain, empty, parameters, true);
	return !IsPointerInteracting(id);
}

// ---- the touch API on pointers (each touch its own pointer)

bool Context::ProcessTouchStart(const TouchList& touches, int key_modifier_state)
{
	bool result = true;
	for (const Touch& touch : touches)
	{
		PointerInput input;
		input.id = (PointerId)(intptr_t)touch.identifier;
		input.type = PointerType::Touch;
		input.position = touch.position;
		input.key_modifier_state = key_modifier_state;
		result &= ProcessPointerDown(input);
	}
	return result;
}

bool Context::ProcessTouchMove(const TouchList& touches, int key_modifier_state)
{
	bool result = true;
	for (const Touch& touch : touches)
	{
		PointerInput input;
		input.id = (PointerId)(intptr_t)touch.identifier;
		input.type = PointerType::Touch;
		input.position = touch.position;
		input.key_modifier_state = key_modifier_state;
		result &= ProcessPointerMove(input);
	}
	return result;
}

bool Context::ProcessTouchEnd(const TouchList& touches, int key_modifier_state)
{
	bool result = true;
	for (const Touch& touch : touches)
	{
		PointerInput input;
		input.id = (PointerId)(intptr_t)touch.identifier;
		input.type = PointerType::Touch;
		input.position = touch.position;
		input.key_modifier_state = key_modifier_state;
		result &= ProcessPointerUp(input);
	}
	return result;
}

bool Context::ProcessTouchCancel(const TouchList& touches)
{
	bool result = true;
	for (const Touch& touch : touches)
		result &= ProcessPointerCancel((PointerId)(intptr_t)touch.identifier);
	return result;
}

// ---- elements that go

void Context::DetachFromPointers(Element* element)
{
	Dictionary parameters;
	auto lose_capture = [&](Element*& capture, Element*& pending, bool& pending_set, PointerId id, PointerType type) {
		if (pending_set && pending == element)
			pending = nullptr;
		if (capture == element)
		{
			capture = nullptr;
			if (!pending_set)
				pending = nullptr;
			GeneratePointerEventParameters(parameters, id, type, true, Vector2f(mouse_position), -1, 0, -1.f, 1.f, 1.f, 0);
			element->DispatchEvent(EventId::Lostpointercapture, parameters);
		}
	};
	lose_capture(mouse_capture, mouse_pending_capture, mouse_pending_capture_set, MousePointerId, PointerType::Mouse);

	for (auto& pair : pointers)
	{
		PointerState& state = pair.second;
		lose_capture(state.capture, state.pending_capture, state.pending_capture_set, pair.first, state.type);
		state.hover_chain.erase(element);
		if (state.hover == element)
			state.hover = nullptr;
		state.active_chain.erase(std::remove(state.active_chain.begin(), state.active_chain.end(), element), state.active_chain.end());
		if (state.active == element)
			state.active = nullptr;
		state.drag_hover_chain.erase(element);
		if (state.drag_hover == element)
			state.drag_hover = nullptr;
		if (state.drag == element)
		{
			if (state.drag_clone)
			{
				cursor_proxy->RemoveChild(state.drag_clone);
				state.drag_clone = nullptr;
			}
			state.drag = nullptr;
			state.drag_hover = nullptr;
			state.drag_hover_chain.clear();
		}
		if (state.scroll.scroll_container == element)
			state.scroll.scroll_container = nullptr;
	}
	for (auto& pair : pointer_clicks)
		if (pair.second.element == element)
			pair.second = PointerClick();
	hover_refs.erase(element);
	active_refs.erase(element);
}

} // namespace Rml
