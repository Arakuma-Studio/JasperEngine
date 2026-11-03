#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/ContextInstancer.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/DataModelHandle.h"
#include "../../Include/RmlUi/Core/Debug.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/RenderBox.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/StreamMemory.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "DataModel.h"
#include "ElementBackgroundBorder.h"
#include "ElementCascade.h"
#include "EventDispatcher.h"
#include "PluginRegistry.h"
#include "PseudoElements.h"
#include "ScrollController.h"
#include "StreamFile.h"
#include <algorithm>
#include <clocale>
#include <iterator>
#include <limits>

namespace Rml {

static constexpr float DOUBLE_CLICK_TIME = 0.5f;    // [s]
static constexpr float DOUBLE_CLICK_MAX_DIST = 3.f; // [dp]
static constexpr float UNIT_SCROLL_LENGTH = 80.f;   // [dp]

// If the user stops scrolling for this amount of time in seconds before touch/click release, don't apply inertia.
static constexpr float SCROLL_INERTIA_DELAY = 0.1f;
static constexpr float TOUCH_MOVEMENT_DECAY_RATE = 5.0f;
static constexpr float TOUCH_CLICK_MAX_DISTANCE = DOUBLE_CLICK_MAX_DIST; // [dp]
// Fork (P7-H): on touch, a draggable element inside a scroll container starts its drag after a press this long
// that moved at most this far; a touch that moves further first scrolls the container instead.
static constexpr float TOUCH_LONG_PRESS_TIME = 0.4f;    // [s]
static constexpr float TOUCH_LONG_PRESS_DISTANCE = 8.f; // [dp]

static void DebugVerifyLocaleSetting()
{
#ifdef RMLUI_DEBUG
	constexpr float expected_value = 1000.5f;
	const Rml::String expected_string = "1000.5";
	const Rml::String formatted_string = Rml::ToString(expected_value);
	const float parsed_value = Rml::FromString<float>(expected_string);

	const char* description = "RmlUi expects the global locale to be set to the default minimal \"C\" locale, please see `std::setlocale`.";
	if (formatted_string != expected_string)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR,
			"Incompatible locale setting detected while formatting %f. Formatted: \"%s\". Expected: \"%s\". Current locale: %s. %s", expected_value,
			formatted_string.c_str(), expected_string.c_str(), std::setlocale(LC_ALL, nullptr), description);
	}
	if (parsed_value != expected_value)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR,
			"Incompatible locale setting detected while parsing \"%s\". Parsed: %f. Expected: %f. Current locale: %s. %s", expected_string.c_str(),
			parsed_value, expected_value, std::setlocale(LC_ALL, nullptr), description);
	}
#endif
}

Context::Context(const String& name, RenderManager* render_manager, TextInputHandler* text_input_handler) :
	name(name), render_manager(render_manager), text_input_handler(text_input_handler)
{
	instancer = nullptr;

	root = Factory::InstanceElement(nullptr, "*", "#root", XMLAttributes());
	root->SetId(name);
	root->SetOffset(Vector2f(0, 0), nullptr);
	root->SetProperty(PropertyId::ZIndex, Property(0, Unit::NUMBER));

	cursor_proxy = Factory::InstanceElement(nullptr, documents_base_tag, documents_base_tag, XMLAttributes());
	ElementDocument* cursor_proxy_document = rmlui_dynamic_cast<ElementDocument*>(cursor_proxy.get());
	RMLUI_ASSERT(cursor_proxy_document);
	cursor_proxy_document->context = this;

	// The cursor proxy takes the style from its cloned element's document. The latter may define style rules for `<body>` which we don't want on the
	// proxy. Thus, we override some properties here that we in particular don't want to inherit from the client document, especially those that
	// result in decoration of the body element.
	cursor_proxy_document->SetProperty(PropertyId::BackgroundColor, Property(Colourb(255, 255, 255, 0), Unit::COLOUR));
	cursor_proxy_document->SetProperty(PropertyId::BorderTopWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderRightWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderBottomWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderLeftWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::Decorator, Property());
	cursor_proxy_document->SetProperty(PropertyId::OverflowX, Property(Style::Overflow::Visible));
	cursor_proxy_document->SetProperty(PropertyId::OverflowY, Property(Style::Overflow::Visible));

	document_focus_history.push_back(root.get());
	focus = root.get();
	hover = nullptr;
	active = nullptr;
	drag = nullptr;

	drag_started = false;
	drag_verbose = false;
	drag_clone = nullptr;
	drag_hover = nullptr;
	last_click_element = nullptr;
	last_click_time = 0;

	mouse_active = false;
	enable_cursor = true;

	scroll_controller = MakeUnique<ScrollController>();
}

Context::~Context()
{
	PluginRegistry::NotifyContextDestroy(this);

	UnloadAllDocuments();

	ReleaseUnloadedDocuments();

	for (const auto& pair : data_models)
		PluginRegistry::NotifyDataModelDestroy(this, pair.first);
	data_models.clear();

	root.reset();

	cursor_proxy.reset();

	instancer = nullptr;
}

const String& Context::GetName() const
{
	return name;
}

void Context::SetDimensions(const Vector2i _dimensions)
{
	if (dimensions != _dimensions)
	{
		dimensions = _dimensions;
		render_manager->SetViewport(dimensions);
		root->SetBox(Box(Vector2f(dimensions)));
		root->DirtyLayout();

		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
			if (document != nullptr)
			{
				document->DirtyMediaQueries();
				document->DirtyVwAndVhProperties();
				document->DirtyLayout();
				document->DirtyPosition();
				document->DispatchEvent(EventId::Resize, Dictionary());
			}
		}
	}
}

Vector2i Context::GetDimensions() const
{
	return dimensions;
}

void Context::SetDensityIndependentPixelRatio(float dp_ratio)
{
	if (density_independent_pixel_ratio != dp_ratio)
	{
		density_independent_pixel_ratio = dp_ratio;

		for (int i = 0; i < root->GetNumChildren(true); ++i)
		{
			ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
			if (document)
			{
				document->DirtyMediaQueries();
				document->OnDpRatioChangeRecursive();
			}
		}
	}
}

float Context::GetDensityIndependentPixelRatio() const
{
	return density_independent_pixel_ratio;
}

bool Context::Update()
{
	RMLUI_ZoneScoped;
	DebugVerifyLocaleSetting();

	next_update_timeout = std::numeric_limits<double>::infinity();

	if (scroll_controller->Update(mouse_position, density_independent_pixel_ratio))
		RequestNextUpdate(0);

	// Update the hover chain to detect any new or moved elements under the mouse.
	if (mouse_active)
		UpdateHoverChain(mouse_position);

	// Update all the data models before updating properties and layout.
	for (auto& data_model : data_models)
		data_model.second->Update(true);

	// The style definition of each document should be independent of each other. By manually resetting these flags we avoid unnecessary definition
	// lookups in unrelated documents, such as when adding a new document. Adding an element dirties the parent definition, which in this case is the
	// root. By extension the definition of all the other documents are also dirtied, unnecessarily.
	root->dirty_definition = false;
	root->dirty_child_definitions = false;

	root->Update(density_independent_pixel_ratio, Vector2f(dimensions));

	for (int i = 0; i < root->GetNumChildren(); ++i)
	{
		if (auto doc = root->GetChild(i)->GetOwnerDocument())
		{
			doc->UpdateLayout();
			doc->UpdatePosition();
		}
	}

	// Release any documents that were unloaded during the update.
	ReleaseUnloadedDocuments();

	return true;
}

void Context::DirtyBackgrounds()
{
	Function<void(Element*)> dirty = [&dirty](Element* element) {
		ElementBackgroundBorder* background = element->GetElementBackgroundBorder();
		background->DirtyBackground();
		background->DirtyBorder();
		background->DirtyOutline();
		// the decorators' data too: gradients are primitives or meshes (S16-F5)
		element->DirtyBorderTopCut();
		for (int i = 0; i < element->GetNumChildren(true); i++)
			dirty(element->GetChild(i));
	};
	dirty(root.get());
}

bool Context::Render()
{
	RMLUI_ZoneScoped;

	render_manager->PrepareRender(dimensions);

	root->Render();

	// Render the cursor proxy so that any attached drag clone will be rendered below the cursor.
	bool pointer_clones = false;
	for (const auto& pair : pointers)
		pointer_clones = pointer_clones || pair.second.drag_clone;
	if (drag_clone || pointer_clones)
	{
		// (fork, S16-F6: the proxy moves)
		render_manager->ResetClipMemo();
		PositionPointerDragClones();
		static_cast<ElementDocument&>(*cursor_proxy).UpdateDocument();
		cursor_proxy->SetOffset(
			Vector2f((float)Math::Clamp(mouse_position.x, 0, dimensions.x), (float)Math::Clamp(mouse_position.y, 0, dimensions.y)), nullptr);
		cursor_proxy->Render();
	}

	render_manager->ResetState();

	return true;
}

ElementDocument* Context::CreateDocument(const String& instancer_name)
{
	ElementPtr element = Factory::InstanceElement(nullptr, instancer_name, documents_base_tag, XMLAttributes());
	if (!element)
	{
		Log::Message(Log::LT_ERROR, "Failed to instance document on instancer_name '%s', instancer returned nullptr.", instancer_name.c_str());
		return nullptr;
	}

	ElementDocument* document = rmlui_dynamic_cast<ElementDocument*>(element.get());
	if (!document)
	{
		Log::Message(Log::LT_ERROR,
			"Failed to instance document on instancer_name '%s', Found type '%s', was expecting derivative of ElementDocument.",
			instancer_name.c_str(), rmlui_type_name(*element));
		return nullptr;
	}

	document->context = this;
	root->AppendChild(std::move(element));

	PluginRegistry::NotifyDocumentLoad(document);

	return document;
}

ElementDocument* Context::LoadDocument(const String& document_path)
{
	auto stream = MakeUnique<StreamFile>();

	if (!stream->Open(document_path))
		return nullptr;

	ElementDocument* document = LoadDocument(stream.get());

	return document;
}

ElementDocument* Context::LoadDocument(Stream* stream)
{
	DebugVerifyLocaleSetting();
	PluginRegistry::NotifyDocumentOpen(this, stream->GetSourceURL().GetURL());

	ElementPtr element = Factory::InstanceDocumentStream(this, stream, GetDocumentsBaseTag());
	if (!element)
		return nullptr;

	ElementDocument* document = rmlui_static_cast<ElementDocument*>(element.get());

	root->AppendChild(std::move(element));

	// The 'load' event is fired before updating the document, because the user might
	// need to initalize things before running an update. The drawback is that computed
	// values and layouting are not performed yet, resulting in default values when
	// querying such information in the event handler.
	PluginRegistry::NotifyDocumentLoad(document);
	document->DispatchEvent(EventId::Load, Dictionary());

	// Data models are updated after the 'load' event so that the user has a chance to change
	// any data variables first. We do not clear dirty variables here, since users may need to
	// retrieve whether or not eg. a data variable has changed in a controller.
	for (auto& data_model : data_models)
		data_model.second->Update(false);

	document->UpdateDocument();

	return document;
}

ElementDocument* Context::LoadDocumentFromMemory(const String& string, const String& source_url)
{
	// Open the stream based on the string contents.
	auto stream = MakeUnique<StreamMemory>(reinterpret_cast<const byte*>(string.c_str()), string.size());

	stream->SetSourceURL(source_url);

	// Load the document from the stream.
	ElementDocument* document = LoadDocument(stream.get());

	return document;
}

void Context::UnloadDocument(ElementDocument* _document)
{
	// Has this document already been unloaded?
	for (size_t i = 0; i < unloaded_documents.size(); ++i)
	{
		if (unloaded_documents[i].get() == _document)
			return;
	}

	ElementDocument* document = _document;

	if (document->GetParentNode() == root.get())
	{
		// Dispatch the unload notifications.
		document->DispatchEvent(EventId::Unload, Dictionary());
		PluginRegistry::NotifyDocumentUnload(document);

		// Move document to a temporary location to be released later.
		unloaded_documents.push_back(root->RemoveChild(document));
	}

	// Remove the item from the focus history.
	ElementList::iterator itr = std::find(document_focus_history.begin(), document_focus_history.end(), document);
	if (itr != document_focus_history.end())
		document_focus_history.erase(itr);

	// Focus to the previous document if the old document is the current focus.
	if (focus && focus->GetOwnerDocument() == document)
	{
		focus = nullptr;
		document_focus_history.back()->GetFocusLeafNode()->Focus();
	}

	// Clear the active element if the old document is the active element.
	if (active && active->GetOwnerDocument() == document)
	{
		active = nullptr;
	}

	// Clear other pointers to elements whose owner was deleted
	if (drag && drag->GetOwnerDocument() == document)
	{
		drag = nullptr;
		ReleaseDragClone();
	}

	if (drag_hover && drag_hover->GetOwnerDocument() == document)
	{
		drag_hover = nullptr;
	}

	// the touch and pen pointers drop the document's elements, without events (fork, engine-gaps G5)
	auto in_document = [document](Element* element) { return element && element->GetOwnerDocument() == document; };
	if (in_document(mouse_capture))
		mouse_capture = nullptr;
	if (in_document(mouse_pending_capture))
		mouse_pending_capture = nullptr;
	for (auto it = pointers.begin(); it != pointers.end();)
	{
		PointerState& state = it->second;
		if (in_document(state.hover) || in_document(state.active) || in_document(state.capture) || in_document(state.drag))
		{
			for (Element* element : state.active_chain)
				RemoveActiveRef(element);
			for (Element* element : state.hover_chain)
				RemoveHoverRef(element);
			if (state.drag_clone)
				cursor_proxy->RemoveChild(state.drag_clone);
			it = pointers.erase(it);
		}
		else
			++it;
	}
	for (auto it = hover_refs.begin(); it != hover_refs.end();)
		it = in_document(it->first) ? hover_refs.erase(it) : std::next(it);
	for (auto it = active_refs.begin(); it != active_refs.end();)
		it = in_document(it->first) ? active_refs.erase(it) : std::next(it);
	active_chain.erase(std::remove_if(active_chain.begin(), active_chain.end(), in_document), active_chain.end());

	// Rebuild the hover state.
	UpdateHoverChain(mouse_position);
}

void Context::UnloadAllDocuments()
{
	// Unload all children.
	while (root->GetNumChildren(true) > 0)
		UnloadDocument(root->GetChild(0)->GetOwnerDocument());

	// The element lists may point to elements that are getting removed.
	active_chain.clear();
	hover_chain.clear();
	drag_hover_chain.clear();
	pointers.clear();
	hover_refs.clear();
	active_refs.clear();
	mouse_capture = nullptr;
	mouse_pending_capture = nullptr;
	mouse_pending_capture_set = false;
}

void Context::EnableMouseCursor(bool enable)
{
	// The cursor is set to an invalid name so that it is forced to update in the next update loop.
	cursor_name = ":reset:";
	enable_cursor = enable;
}

void Context::ActivateTheme(const String& theme_name, bool activate)
{
	bool theme_changed = false;

	if (activate)
		theme_changed = active_themes.insert(theme_name).second;
	else
		theme_changed = (active_themes.erase(theme_name) > 0);

	if (theme_changed)
	{
		for (int i = 0; i < root->GetNumChildren(true); ++i)
		{
			if (ElementDocument* document = root->GetChild(i)->GetOwnerDocument())
				document->DirtyMediaQueries();
		}
	}
}

bool Context::IsThemeActive(const String& theme_name) const
{
	return active_themes.count(theme_name);
}

ElementDocument* Context::GetDocument(const String& id)
{
	for (int i = 0; i < root->GetNumChildren(); i++)
	{
		ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
		if (document == nullptr)
			continue;

		if (document->GetId() == id)
			return document;
	}

	return nullptr;
}

ElementDocument* Context::GetDocument(int index)
{
	Element* element = root->GetChild(index);
	if (element == nullptr)
		return nullptr;

	return element->GetOwnerDocument();
}

int Context::GetNumDocuments() const
{
	return root->GetNumChildren();
}

Element* Context::GetHoverElement()
{
	return hover;
}

Element* Context::GetFocusElement()
{
	return focus;
}

Element* Context::GetRootElement()
{
	return root.get();
}

void Context::PullDocumentToFront(ElementDocument* document)
{
	if (document != root->GetLastChild())
	{
		// Calling RemoveChild() / AppendChild() would be cleaner, but that dirties the document's layout
		// unnecessarily, so we'll go under the hood here.
		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			if (root->GetChild(i) == document)
			{
				ElementPtr element = std::move(root->children[i]);
				root->children.erase(root->children.begin() + i);
				root->children.insert(root->children.begin() + root->GetNumChildren(), std::move(element));

				root->DirtyStackingContext();
			}
		}
	}
}

void Context::PushDocumentToBack(ElementDocument* document)
{
	if (document != root->GetFirstChild())
	{
		// See PullDocumentToFront().
		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			if (root->GetChild(i) == document)
			{
				ElementPtr element = std::move(root->children[i]);
				root->children.erase(root->children.begin() + i);
				root->children.insert(root->children.begin(), std::move(element));

				root->DirtyStackingContext();
			}
		}
	}
}

void Context::UnfocusDocument(ElementDocument* document)
{
	auto it = std::find(document_focus_history.begin(), document_focus_history.end(), document);
	if (it != document_focus_history.end())
		document_focus_history.erase(it);

	if (!document_focus_history.empty())
		document_focus_history.back()->GetFocusLeafNode()->Focus();
}

void Context::AddEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	root->AddEventListener(event, listener, in_capture_phase);
}

void Context::RemoveEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	root->RemoveEventListener(event, listener, in_capture_phase);
}

bool Context::ProcessKeyDown(Input::KeyIdentifier key_identifier, int key_modifier_state)
{
	// Generate the parameters for the key event.
	Dictionary parameters;
	GenerateKeyEventParameters(parameters, key_identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	// DispatchEvent is false when the event was stopped or its default action prevented; only the latter drops
	// the text input of the key (a stopped key down still types, e.g. in a text field that stops propagation)
	suppress_text_input = false;
	Element* target = (focus ? focus : root.get());
	bool default_prevented = false;
	const bool propagating = target->DispatchEvent(EventId::Keydown, parameters, &default_prevented);
	suppress_text_input = default_prevented;
	return propagating;
}

bool Context::ProcessKeyUp(Input::KeyIdentifier key_identifier, int key_modifier_state)
{
	// Generate the parameters for the key event.
	Dictionary parameters;
	GenerateKeyEventParameters(parameters, key_identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	suppress_text_input = false;
	if (focus)
		return focus->DispatchEvent(EventId::Keyup, parameters);
	else
		return root->DispatchEvent(EventId::Keyup, parameters);
}

bool Context::ProcessTextInput(char character)
{
	// Only the standard ASCII character set is a valid subset of UTF-8.
	if (static_cast<unsigned char>(character) > 127)
		return false;
	return ProcessTextInput(static_cast<Character>(character));
}

bool Context::ProcessTextInput(Character character)
{
	// Generate the parameters for the key event.
	String text = StringUtilities::ToUTF8(character);
	return ProcessTextInput(text);
}

bool Context::ProcessTextInput(const String& string)
{
	// the key down that produced this text had its default action prevented
	if (suppress_text_input)
		return false;

	Element* target = (focus ? focus : root.get());

	Dictionary parameters;
	parameters["text"] = string;

	bool consumed = target->DispatchEvent(EventId::Textinput, parameters);

	return consumed;
}

bool Context::ProcessMouseMove(int x, int y, int key_modifier_state)
{
	// Check whether the mouse moved since the last event came through.
	Vector2i old_mouse_position = mouse_position;
	mouse_position = {x, y};
	const bool mouse_moved = (mouse_position != old_mouse_position || !mouse_active);
	mouse_active = true;

	// Update the current hover chain. This will send all necessary 'onmouseout', 'onmouseover', 'ondragout' and 'ondragover' messages.
	ProcessPendingCapture(MousePointerId);
	Dictionary parameters, drag_parameters;
	UpdateHoverChain(old_mouse_position, key_modifier_state, &parameters, &drag_parameters);

	// Dispatch any 'onmousemove' events: 'pointermove' first, to the capture or the hover (fork, engine-gaps G5-2).
	if (mouse_moved)
	{
		if (Element* target = GetPointerTarget(MousePointerId))
		{
			ObserverPtr<Element> target_obs = target->GetObserverPtr();
			target->DispatchEvent(EventId::Pointermove, parameters);
			if (target_obs)
				target_obs->DispatchEvent(EventId::Mousemove, parameters);
		}
		if (hover && drag_hover && drag_verbose)
			drag_hover->DispatchEvent(EventId::Dragmove, drag_parameters);
	}

	return !IsMouseInteracting();
}

static Element* FindFocusElement(Element* element)
{
	ElementDocument* owner_document = element->GetOwnerDocument();
	if (!owner_document || owner_document->GetComputedValues().focus() == Style::Focus::None)
		return nullptr;

	while (element && ElementCascade::IsUnfocusable(element))
	{
		element = element->GetParentNode();
	}

	return element;
}

bool Context::ProcessMouseButtonDown(int button_index, int key_modifier_state)
{
	// fork (engine-gaps G5-2): 'pointerdown' for the first button pressed (a later one: 'pointermove', DOM chorded
	// buttons), to the capture or the hover; prevented, it suppresses the compatibility 'mousedown' and the focus change
	const bool first_button = (mouse_buttons == 0);
	if (button_index >= 0 && button_index < 3)
		mouse_buttons |= (button_index == 0 ? 1 : button_index == 1 ? 2 : 4);
	ProcessPendingCapture(MousePointerId);
	Dictionary parameters;
	GenerateMousePointerParameters(parameters, button_index, key_modifier_state);
	bool pointer_prevented = false;
	if (Element* target = GetPointerTarget(MousePointerId))
		target->DispatchEvent(first_button ? EventId::Pointerdown : EventId::Pointermove, parameters, &pointer_prevented);

	bool propagate = true;

	if (button_index == 0)
	{
		Element* new_focus = hover;

		// Set the currently hovered element to focus if it isn't already the focus.
		if (hover)
		{
			new_focus = FindFocusElement(hover);
			if (!pointer_prevented && new_focus && new_focus != focus)
			{
				if (!new_focus->Focus())
					return !IsMouseInteracting();
			}
		}

		// Save the just-pressed-on element as the pressed element.
		active = new_focus;

		// Call 'onmousedown' on every item in the hover chain, and copy the hover chain to the active chain.
		if (hover && !pointer_prevented)
			propagate = hover->DispatchEvent(EventId::Mousedown, parameters);

		if (propagate)
		{
			// Check for a double-click on an element; if one has occured, we send the 'dblclick' event to the hover
			// element. If not, we'll start a timer to catch the next one.
			float mouse_distance_squared = float((mouse_position - last_click_mouse_position).SquaredMagnitude());
			float max_mouse_distance = DOUBLE_CLICK_MAX_DIST * density_independent_pixel_ratio;

			double click_time = GetSystemInterface()->GetElapsedTime();

			if (active == last_click_element && float(click_time - last_click_time) < DOUBLE_CLICK_TIME &&
				mouse_distance_squared < max_mouse_distance * max_mouse_distance)
			{
				if (hover)
					propagate = hover->DispatchEvent(EventId::Dblclick, parameters);

				last_click_element = nullptr;
				last_click_time = 0;
			}
			else
			{
				last_click_element = active;
				last_click_time = click_time;
			}
		}

		last_click_mouse_position = mouse_position;

		// ':active' where the point is within the element, counted with the other pointers' (G5-1)
		for (Element* element : hover_chain)
		{
			if (element->IsPointWithinElement(Vector2f(mouse_position)))
			{
				active_chain.push_back(element);
				AddActiveRef(element);
			}
		}

		if (propagate)
		{
			// Traverse down the hierarchy of the newly focused element (if any), and see if we can begin dragging it.
			drag_started = false;
			drag = hover;
			while (drag)
			{
				Style::Drag drag_style = drag->GetComputedValues().drag();
				switch (drag_style)
				{
				case Style::Drag::None: drag = drag->GetParentNode(); continue;
				case Style::Drag::Block: drag = nullptr; continue;
				default: drag_verbose = (drag_style == Style::Drag::DragDrop || drag_style == Style::Drag::Clone);
				}

				break;
			}
		}
	}
	else
	{
		// Not the primary mouse button, so we're not doing any special processing.
		if (hover && !pointer_prevented)
			propagate = hover->DispatchEvent(EventId::Mousedown, parameters);
	}

	if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
	{
		scroll_controller->Reset();
	}
	else if (button_index == 2 && hover && propagate)
	{
		Dictionary scroll_parameters;
		GenerateMouseEventParameters(scroll_parameters);
		GenerateKeyModifierEventParameters(scroll_parameters, key_modifier_state);
		scroll_parameters["autoscroll"] = true;

		// Dispatch a mouse scroll event, this gives elements an opportunity to block autoscroll from being initialized.
		if (hover->DispatchEvent(EventId::Mousescroll, scroll_parameters))
			scroll_controller->ActivateAutoscroll(hover->GetClosestScrollableContainer(), mouse_position);
	}

	return !IsMouseInteracting();
}

bool Context::ProcessMouseButtonUp(int button_index, int key_modifier_state)
{
	// We want to return the interaction state before handling the mouse up events, so that any active element that is released is considered to
	// capture the event.
	const bool result = !IsMouseInteracting();

	// fork (engine-gaps G5-2/3): 'pointerup' when the last button goes (else 'pointermove'), then the compatibility
	// 'mouseup', to the capture or the hover; the capture goes with the last button
	ProcessPendingCapture(MousePointerId);
	if (button_index >= 0 && button_index < 3)
		mouse_buttons &= ~(button_index == 0 ? 1 : button_index == 1 ? 2 : 4);
	const bool last_button = (mouse_buttons == 0);
	Dictionary parameters;
	GenerateMousePointerParameters(parameters, button_index, key_modifier_state);
	Element* up_target = GetPointerTarget(MousePointerId);
	ObserverPtr<Element> up_target_obs = up_target ? up_target->GetObserverPtr() : ObserverPtr<Element>();
	if (up_target)
		up_target->DispatchEvent(last_button ? EventId::Pointerup : EventId::Pointermove, parameters);

	// Process primary click.
	if (button_index == 0)
	{
		// The elements in the new hover chain have the 'onmouseup' event called on them (the capture's when captured).
		if (mouse_capture ? up_target_obs.get() : hover)
			(mouse_capture ? up_target_obs.get() : hover)->DispatchEvent(EventId::Mouseup, parameters);

		// If the active element (the one that was being hovered over when the mouse button was pressed) is still being
		// hovered over, we click it.
		if (hover && active && active == FindFocusElement(hover))
		{
			active->DispatchEvent(EventId::Click, parameters);
		}

		ResetActiveChain();

		if (drag)
		{
			if (drag_started)
			{
				Dictionary drag_parameters;
				GenerateMouseEventParameters(drag_parameters);
				GenerateDragEventParameters(drag_parameters);
				GenerateKeyModifierEventParameters(drag_parameters, key_modifier_state);

				if (drag_hover)
				{
					if (drag_verbose)
					{
						drag_hover->DispatchEvent(EventId::Dragdrop, drag_parameters);
						// User may have removed the element, do an extra check.
						if (drag_hover)
							drag_hover->DispatchEvent(EventId::Dragout, drag_parameters);
					}
				}

				if (drag)
					drag->DispatchEvent(EventId::Dragend, drag_parameters);

				ReleaseDragClone();
			}

			drag = nullptr;
			drag_hover = nullptr;
			drag_hover_chain.clear();

			// We may have changes under our mouse, this ensures that the hover chain is properly updated
			ProcessMouseMove(mouse_position.x, mouse_position.y, key_modifier_state);
		}
	}
	else
	{
		// Not the left mouse button, so we're not doing any special processing.
		if (hover)
			hover->DispatchEvent(EventId::Mouseup, parameters);
	}

	// If we have autoscrolled while holding the middle mouse button, release the autoscroll mode now.
	if (scroll_controller->HasAutoscrollMoved())
		scroll_controller->Reset();

	if (last_button && (mouse_capture || mouse_pending_capture_set))
	{
		mouse_pending_capture = nullptr;
		mouse_pending_capture_set = true;
		ProcessPendingCapture(MousePointerId);
	}

	return result;
}

bool Context::ProcessMouseWheel(float wheel_delta, int key_modifier_state)
{
	return ProcessMouseWheel(Vector2f{0.f, wheel_delta}, key_modifier_state);
}

bool Context::ProcessMouseWheel(Vector2f wheel_delta, int key_modifier_state)
{
	if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
	{
		scroll_controller->Reset();
		return false;
	}
	else if (!hover)
	{
		scroll_controller->Reset();
		return true;
	}

	Dictionary scroll_parameters;
	GenerateMouseEventParameters(scroll_parameters);
	GenerateKeyModifierEventParameters(scroll_parameters, key_modifier_state);
	scroll_parameters["wheel_delta_x"] = wheel_delta.x;
	scroll_parameters["wheel_delta_y"] = wheel_delta.y;

	// Dispatch a mouse scroll event, this gives elements an opportunity to block scrolling from being performed.
	if (!hover->DispatchEvent(EventId::Mousescroll, scroll_parameters))
		return false;

	const float unit_scroll_length = UNIT_SCROLL_LENGTH * density_independent_pixel_ratio;
	const Vector2f scroll_length = wheel_delta * unit_scroll_length;
	Element* target = hover->GetClosestScrollableContainer();

	if (scroll_controller->GetMode() == ScrollController::Mode::Smoothscroll && scroll_controller->GetTarget() == target)
		scroll_controller->IncrementSmoothscrollTarget(scroll_length);
	else
		scroll_controller->ActivateSmoothscroll(target, scroll_length, ScrollBehavior::Auto);

	return target == nullptr;
}

bool Context::ProcessMouseLeave()
{
	mouse_active = false;

	// Update the hover chain. Now that 'mouse_active' is disabled this will remove the hover state from all elements.
	UpdateHoverChain(mouse_position);

	return !IsMouseInteracting();
}

bool Context::IsMouseInteracting() const
{
	return (hover && hover != root.get()) || (active && active != root.get()) || scroll_controller->GetMode() == ScrollController::Mode::Autoscroll;
}

void Context::SetDefaultScrollBehavior(ScrollBehavior scroll_behavior, float speed_factor)
{
	scroll_controller->SetDefaultScrollBehavior(scroll_behavior, speed_factor);
}

RenderManager& Context::GetRenderManager()
{
	return *render_manager;
}

TextInputHandler* Context::GetTextInputHandler() const
{
	return text_input_handler;
}

void Context::SetInstancer(ContextInstancer* _instancer)
{
	RMLUI_ASSERT(instancer == nullptr);
	instancer = _instancer;
}

DataModelConstructor Context::CreateDataModel(const String& name, DataTypeRegister* data_type_register, bool allow_missing_variables)
{
	if (!data_type_register)
	{
		if (!default_data_type_register)
			default_data_type_register = MakeUnique<DataTypeRegister>();
		data_type_register = default_data_type_register.get();
	}

	auto result = data_models.emplace(name, MakeUnique<DataModel>(data_type_register, allow_missing_variables));
	bool inserted = result.second;
	if (inserted)
	{
		DataModel* model = result.first->second.get();
		PluginRegistry::NotifyDataModelCreate(this, name);
		return DataModelConstructor(model);
	}

	Log::Message(Log::LT_ERROR, "Data model name '%s' already exists.", name.c_str());
	return DataModelConstructor();
}

DataModelConstructor Context::GetDataModel(const String& name)
{
	if (DataModel* model = GetDataModelPtr(name))
		return DataModelConstructor(model);

	Log::Message(Log::LT_ERROR, "Data model name '%s' could not be found.", name.c_str());
	return DataModelConstructor();
}

UnorderedMap<String, DataModelConstructor> Context::GetDataModels() const
{
	UnorderedMap<String, DataModelConstructor> result;
	result.reserve(data_models.size());
	for (const auto& pair : data_models)
		result.emplace(pair.first, DataModelConstructor(pair.second.get()));
	return result;
}

bool Context::RemoveDataModel(const String& name)
{
	auto it = data_models.find(name);
	if (it == data_models.end())
		return false;

	DataModel* model = it->second.get();
	ElementList elements = model->GetAttachedModelRootElements();

	for (Element* element : elements)
		element->SetDataModel(nullptr);

	PluginRegistry::NotifyDataModelDestroy(this, it->first);

	data_models.erase(it);

	return true;
}

void Context::OnElementDetach(Element* element)
{
	auto it_hover = hover_chain.find(element);
	if (it_hover != hover_chain.end())
	{
		Dictionary parameters;
		GenerateMouseEventParameters(parameters, -1);
		element->DispatchEvent(EventId::Mouseout, parameters);

		hover_chain.erase(it_hover);

		if (hover == element)
			hover = nullptr;
	}

	auto it_active = std::find(active_chain.begin(), active_chain.end(), element);
	if (it_active != active_chain.end())
	{
		active_chain.erase(it_active);

		if (active == element)
			active = nullptr;
	}

	if (drag)
	{
		auto it = drag_hover_chain.find(element);
		if (it != drag_hover_chain.end())
		{
			drag_hover_chain.erase(it);

			if (drag_hover == element)
				drag_hover = nullptr;
		}

		if (drag == element)
		{
			// The dragged element is being removed, silently cancel the drag operation
			if (drag_started)
				ReleaseDragClone();

			drag = nullptr;
			drag_hover = nullptr;
			drag_hover_chain.clear();
		}
	}

	// Focus normally cleared and set by parent during Element::RemoveChild.
	// However, there are some exceptions, such as when an there are multiple
	// ElementDocuments in the hierarchy above the current element.
	if (element == focus)
		focus = nullptr;

	// If the element is a document lower down in the hierarchy, we may need to remove it from the focus history.
	if (element->GetOwnerDocument() == element)
	{
		auto it = std::find(document_focus_history.begin(), document_focus_history.end(), element);
		if (it != document_focus_history.end())
			document_focus_history.erase(it);
	}

	if (scroll_controller->GetTarget() == element)
		scroll_controller->Reset();

	// the touch and pen pointers, the captures and the counted pseudo-classes (fork, engine-gaps G5)
	DetachFromPointers(element);
}

bool Context::OnFocusChange(Element* new_focus, bool focus_visible)
{
	RMLUI_ASSERT(new_focus);

	ElementSet old_chain;
	ElementSet new_chain;

	Element* old_focus = focus;
	ElementDocument* old_document = old_focus ? old_focus->GetOwnerDocument() : nullptr;
	ElementDocument* new_document = new_focus->GetOwnerDocument();

	// If the current focus is modal and the new focus is cannot receive focus from modal, deny the request.
	if (old_document && old_document->IsModal() && (!new_document || !(new_document->IsModal() || new_document->IsFocusableFromModal())))
		return false;

	// If the document of the new focus has been closed, deny the request.
	if (std::find_if(unloaded_documents.begin(), unloaded_documents.end(),
			[&](const auto& unloaded_document) { return unloaded_document.get() == new_document; }) != unloaded_documents.end())
	{
		return false;
	}

	// Build the old chains
	Element* element = old_focus;
	while (element)
	{
		old_chain.insert(element);
		element = element->GetParentNode();
	}

	// Build the new chain
	element = new_focus;
	while (element)
	{
		new_chain.insert(element);
		element = element->GetParentNode();
	}

	// The focus pseudo-classes (CSS Selectors 4, phase 3): ':focus' on the focused element alone, ':focus-within' on it
	// and its ancestors, ':focus-visible' on the focused element when the focus came from the keyboard or navigation
	// (text fields set it themselves when focused). The old element loses them before the blur events, the new one gets
	// them before the focus events. The events still go to the whole chains.
	if (old_focus)
	{
		old_focus->SetPseudoClass("focus", false);
		old_focus->SetPseudoClass("focus-visible", false);
	}
	for (Element* old_element : old_chain)
	{
		if (new_chain.count(old_element) == 0)
			old_element->SetPseudoClass("focus-within", false);
	}

	// Send out blur/focus events.
	Dictionary parameters;
	SendEvents(old_chain, new_chain, EventId::Blur, parameters);

	for (Element* new_element : new_chain)
		new_element->SetPseudoClass("focus-within", true);
	new_focus->SetPseudoClass("focus", true);
	new_focus->SetPseudoClass("focus-visible", focus_visible);

	if (focus_visible)
		parameters["focus_visible"] = true;

	SendEvents(new_chain, old_chain, EventId::Focus, parameters);

	focus = new_focus;

	// Raise the element's document to the front, if desired.
	ElementDocument* document = focus->GetOwnerDocument();
	if (document != nullptr)
	{
		Style::ZIndex z_index_property = document->GetComputedValues().z_index();
		if (z_index_property.type == Style::ZIndex::Auto)
			document->PullToFront();
	}

	// Update the focus history
	if (old_document != new_document)
	{
		// If documents have changed, add the new document to the end of the history
		ElementList::iterator itr = std::find(document_focus_history.begin(), document_focus_history.end(), new_document);
		if (itr != document_focus_history.end())
			document_focus_history.erase(itr);

		if (new_document != nullptr)
			document_focus_history.push_back(new_document);
	}

	return true;
}

void Context::GenerateClickEvent(Element* element)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, 0);

	element->DispatchEvent(EventId::Click, parameters);
}

void Context::UpdateHoverChain(Vector2i old_mouse_position, int key_modifier_state, Dictionary* out_parameters, Dictionary* out_drag_parameters)
{
	const Vector2f position(mouse_position);

	Dictionary local_parameters, local_drag_parameters;
	Dictionary& parameters = out_parameters ? *out_parameters : local_parameters;
	Dictionary& drag_parameters = out_drag_parameters ? *out_drag_parameters : local_drag_parameters;

	// Generate the parameters for the mouse events (there could be a few!), with the pointer's (fork, engine-gaps G5-2).
	GenerateMousePointerParameters(parameters, -1, key_modifier_state);

	GenerateMouseEventParameters(drag_parameters);
	GenerateDragEventParameters(drag_parameters);
	GenerateKeyModifierEventParameters(drag_parameters, key_modifier_state);

	// Send out drag events.
	if (drag)
	{
		if (mouse_position != old_mouse_position)
		{
			if (!drag_started)
			{
				Dictionary drag_start_parameters = drag_parameters;
				drag_start_parameters["mouse_x"] = old_mouse_position.x;
				drag_start_parameters["mouse_y"] = old_mouse_position.y;
				// Fork (P7-C): a dragstart whose default action a listener prevented starts no drag, as in HTML5.
				bool prevented = false;
				drag->DispatchEvent(EventId::Dragstart, drag_start_parameters, &prevented);
				if (prevented || !drag)
				{
					drag = nullptr;
					drag_hover = nullptr;
					drag_hover_chain.clear();
				}
				else
				{
					drag_started = true;

					if (drag->GetComputedValues().drag() == Style::Drag::Clone)
					{
						// Clone the element and attach it to the mouse cursor.
						CreateDragClone(drag);
					}
				}
			}

			if (drag)
				drag->DispatchEvent(EventId::Drag, drag_parameters);
		}
	}

	Element* old_hover = hover;
	hover = mouse_active ? GetElementAtPoint(position) : nullptr;

	if (enable_cursor)
	{
		String new_cursor_name;

		if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
			new_cursor_name = scroll_controller->GetAutoscrollCursor(mouse_position, density_independent_pixel_ratio);
		else if (drag)
			new_cursor_name = drag->GetComputedValues().cursor();
		else if (hover)
			new_cursor_name = hover->GetComputedValues().cursor();

		if (new_cursor_name != cursor_name)
		{
			GetSystemInterface()->SetMouseCursor(new_cursor_name);
			cursor_name = new_cursor_name;
		}
	}

	// Build the new hover chain.
	ElementSet new_hover_chain;
	Element* element = hover;
	while (element != nullptr)
	{
		new_hover_chain.insert(element);
		element = element->GetParentNode();
	}

	// Send pointerout / mouseout, pointerleave / mouseleave, pointerover / mouseover, pointerenter / mouseenter events, with
	// the counted ':hover' (fork, engine-gaps G5-1/2). The new chain is swapped in.
	SwapHoverChain(old_hover, hover, hover_chain, new_hover_chain, parameters, true);

	// Send out drag events.
	if (drag && mouse_active)
	{
		drag_hover = GetElementAtPoint(position, drag);

		ElementSet new_drag_hover_chain;
		element = drag_hover;
		while (element != nullptr)
		{
			new_drag_hover_chain.insert(element);
			element = element->GetParentNode();
		}

		if (drag_started && drag_verbose)
		{
			// Send out ondragover and ondragout events as appropriate.
			SendEvents(drag_hover_chain, new_drag_hover_chain, EventId::Dragout, drag_parameters);
			SendEvents(new_drag_hover_chain, drag_hover_chain, EventId::Dragover, drag_parameters);
		}

		drag_hover_chain.swap(new_drag_hover_chain);
	}
}

void Context::ResetActiveChain()
{
	// Unset the 'active' pseudo-class on all the elements in the active chain; because they may not necessarily
	// have had 'onmouseup' called on them, we can't guarantee this has happened already. Counted with the other
	// pointers' (fork, engine-gaps G5-1).
	for (Element* element : active_chain)
		RemoveActiveRef(element);

	active_chain.clear();
	active = nullptr;
}

// CSS hit testing follows the curves of the border box (phase 3, p3-paint): a point in the rectangle of one of the
// element's boxes but outside of the ellipse of a rounded corner is not within the element.
static bool IsPointWithinBorderRadius(Element* element, Vector2f point)
{
	if (!element->GetComputedValues().has_border_radius())
		return true;

	const Vector2f origin = element->GetAbsoluteOffset(BoxArea::Border);
	for (int i = 0; i < element->GetNumBoxes(); i++)
	{
		const RenderBox box = element->GetRenderBox(BoxArea::Border, i);
		const Vector2f size = box.GetFillSize();
		const Vector2f p = point - origin - box.GetBorderOffset();
		if (p.x < 0.f || p.y < 0.f || p.x > size.x || p.y > size.y)
			continue;

		// The distance into the box from the corner's two edges, for top-left, top-right, bottom-right, bottom-left.
		const Vector2f from_corner[4] = {p, {size.x - p.x, p.y}, size - p, {p.x, size.y - p.y}};
		const CornerSizes2 radii = box.GetBorderRadius();
		bool inside = true;
		for (int corner = 0; corner < 4 && inside; corner++)
		{
			const Vector2f r = radii[corner];
			const Vector2f d = from_corner[corner];
			if (r.x > 0.f && r.y > 0.f && d.x < r.x && d.y < r.y)
			{
				const Vector2f q = (r - d) / r;
				inside = (q.x * q.x + q.y * q.y <= 1.f);
			}
		}
		if (inside)
			return true;
	}
	return false;
}

Element* Context::GetElementAtPoint(Vector2f point, const Element* ignore_element, Element* element) const
{
	if (!element)
	{
		if (ignore_element == root.get())
			return nullptr;

		element = root.get();
	}

	bool is_modal = false;
	ElementDocument* focus_document = nullptr;

	// If we have modal focus, only check down documents that can receive focus from modals.
	if (element == root.get() && focus)
	{
		focus_document = focus->GetOwnerDocument();
		if (focus_document && focus_document->IsModal())
			is_modal = true;
	}

	// Check any elements within our stacking context. We want to return the lowest-down element
	// that is under the cursor.
	if (element->local_stacking_context)
	{
		if (element->stacking_context_dirty)
			element->BuildLocalStackingContext();

		for (int i = (int)element->stacking_context.size() - 1; i >= 0; --i)
		{
			Element* stacking_child = element->stacking_context[i];
			if (ignore_element)
			{
				// Check if the element is a descendant of the element we're ignoring.
				Element* element_hierarchy = stacking_child;
				while (element_hierarchy)
				{
					if (element_hierarchy == ignore_element)
						break;

					element_hierarchy = element_hierarchy->GetParentNode();
				}

				if (element_hierarchy)
					continue;
			}

			if (is_modal)
			{
				ElementDocument* child_document = stacking_child->GetOwnerDocument();
				if (!child_document || !(child_document == focus_document || child_document->IsFocusableFromModal()))
					continue;
			}

			Element* child_element = GetElementAtPoint(point, ignore_element, stacking_child);
			if (child_element)
				return child_element;
		}
	}

	// Ignore elements whose pointer events are disabled, and hidden elements (their visible descendants were tested above,
	// in the stacking context they are part of).
	if (element->GetComputedValues().pointer_events() == Style::PointerEvents::None ||
		element->GetComputedValues().visibility() != Style::Visibility::Visible)
		return nullptr;

	// Projection may fail if we have a singular transformation matrix.
	bool projection_result = element->Project(point);

	// Check if the point is actually within this element.
	bool within_element = (projection_result && element->IsPointWithinElement(point) && IsPointWithinBorderRadius(element, point));
	if (within_element)
	{
		// The element may have been clipped out of view if it overflows an ancestor, so check its clipping region.
		Rectanglei clip_region;
		if (ElementUtilities::GetClippingRegion(element, clip_region))
			within_element = clip_region.Contains(Vector2i(point));
	}

	// A hit on a pseudo-element (or inside one) is a hit on its originating element (phase 3): events target it and its
	// :hover applies, so 'a:hover::after' rules follow the pointer.
	if (within_element)
	{
		// An element the pointer passes through (SetPointerPassThrough, phase 4) is not hit itself; its descendants were
		// tested above, and the caller goes on to what is below it.
		if (pointer_pass_through && pointer_pass_through(element))
			return nullptr;
		return PseudoElements::GetOriginatingElement(element);
	}

	return nullptr;
}

void Context::SetPointerPassThrough(Function<bool(Element*)> filter)
{
	pointer_pass_through = std::move(filter);
	// The hover chain follows at the next update or pointer event.
}

void Context::CreateDragClone(Element* element)
{
	RMLUI_ASSERTMSG(cursor_proxy, "Unable to create drag clone, no cursor proxy document.");

	ReleaseDragClone();

	// Instance the drag clone.
	ElementPtr element_drag_clone = element->Clone();
	if (!element_drag_clone)
	{
		Log::Message(Log::LT_ERROR, "Unable to duplicate drag clone.");
		return;
	}

	// Set the style sheet on the cursor proxy.
	if (ElementDocument* document = element->GetOwnerDocument())
	{
		// Borrow the target document's style sheet. Sharing style sheet containers should be used with care, and
		// only within the same context.
		static_cast<ElementDocument&>(*cursor_proxy).SetStyleSheetContainer(document->style_sheet_container);
	}

	drag_clone = element_drag_clone.get();

	// Append the clone to the cursor proxy element.
	cursor_proxy->AppendChild(std::move(element_drag_clone));

	// Position the clone. Use projected mouse coordinates to handle any ancestor transforms.
	const Vector2f absolute_pos = element->GetAbsoluteOffset(BoxArea::Border);
	Vector2f projected_mouse_position = Vector2f(mouse_position);
	if (Element* parent = element->GetParentNode())
		parent->Project(projected_mouse_position);

	drag_clone->SetProperty(PropertyId::Position, Property(Style::Position::Absolute));
	drag_clone->SetProperty(PropertyId::Left, Property(absolute_pos.x - projected_mouse_position.x, Unit::PX));
	drag_clone->SetProperty(PropertyId::Top, Property(absolute_pos.y - projected_mouse_position.y, Unit::PX));
	// We remove margins so that percentage- and auto-margins are evaluated correctly.
	drag_clone->SetProperty(PropertyId::MarginLeft, Property(0.f, Unit::PX));
	drag_clone->SetProperty(PropertyId::MarginTop, Property(0.f, Unit::PX));
	drag_clone->SetPseudoClass("drag", true);
}

void Context::ReleaseDragClone()
{
	if (drag_clone)
	{
		cursor_proxy->RemoveChild(drag_clone);
		drag_clone = nullptr;
		static_cast<ElementDocument&>(*cursor_proxy).SetStyleSheetContainer(nullptr);
	}
}

void Context::PerformSmoothscrollOnTarget(Element* target, Vector2f delta_offset, ScrollBehavior scroll_behavior)
{
	scroll_controller->ActivateSmoothscroll(target, delta_offset, scroll_behavior);
}

DataModel* Context::GetDataModelPtr(const String& name) const
{
	auto it = data_models.find(name);
	if (it != data_models.end())
		return it->second.get();
	return nullptr;
}

void Context::GenerateKeyEventParameters(Dictionary& parameters, Input::KeyIdentifier key_identifier)
{
	parameters["key_identifier"] = (int)key_identifier;
}

void Context::GenerateMouseEventParameters(Dictionary& parameters, int button_index)
{
	parameters.reserve(3);
	parameters["mouse_x"] = mouse_position.x;
	parameters["mouse_y"] = mouse_position.y;
	if (button_index >= 0)
		parameters["button"] = button_index;
}

void Context::GenerateKeyModifierEventParameters(Dictionary& parameters, int key_modifier_state)
{
	static const String property_names[] = {"ctrl_key", "shift_key", "alt_key", "meta_key", "caps_lock_key", "num_lock_key", "scroll_lock_key"};

	for (int i = 0; i < 7; i++)
		parameters[property_names[i]] = (int)((key_modifier_state & (1 << i)) > 0);
}

void Context::GenerateDragEventParameters(Dictionary& parameters)
{
	parameters["drag_element"] = (void*)drag;
}

void Context::ReleaseUnloadedDocuments()
{
	if (!unloaded_documents.empty())
	{
		OwnedElementList documents = std::move(unloaded_documents);
		unloaded_documents.clear();

		// Clear the deleted list.
		for (size_t i = 0; i < documents.size(); ++i)
			documents[i]->GetEventDispatcher()->DetachAllEvents();
		documents.clear();
	}
}

using ElementObserverList = Vector<ObserverPtr<Element>>;

class ElementObserverListBackInserter {
public:
	using iterator_category = std::output_iterator_tag;
	using value_type = void;
	using difference_type = void;
	using pointer = void;
	using reference = void;
	using container_type = ElementObserverList;

	ElementObserverListBackInserter(ElementObserverList& elements) : elements(&elements) {}
	ElementObserverListBackInserter& operator=(Element* element)
	{
		elements->push_back(element->GetObserverPtr());
		return *this;
	}
	ElementObserverListBackInserter& operator*() { return *this; }
	ElementObserverListBackInserter& operator++() { return *this; }
	ElementObserverListBackInserter& operator++(int) { return *this; }

private:
	ElementObserverList* elements;
};

void Context::CancelDrag()
{
	if (!drag)
		return;
	if (drag_started)
	{
		Dictionary drag_parameters;
		GenerateMouseEventParameters(drag_parameters);
		GenerateDragEventParameters(drag_parameters);
		drag_parameters["cancelled"] = 1;
		if (drag_hover && drag_verbose)
			drag_hover->DispatchEvent(EventId::Dragout, drag_parameters);
		if (drag)
			drag->DispatchEvent(EventId::Dragend, drag_parameters);
		ReleaseDragClone();
	}
	drag = nullptr;
	drag_started = false;
	drag_hover = nullptr;
	drag_hover_chain.clear();
	// the button still held clicks nothing when it goes up
	ResetActiveChain();
}

bool Context::IsDragging() const
{
	return drag && drag_started;
}

void Context::SendEvents(const ElementSet& old_items, const ElementSet& new_items, EventId id, const Dictionary& parameters)
{
	// We put our elements in observer pointers in case some of them are deleted during dispatch.
	ElementObserverList elements;
	std::set_difference(old_items.begin(), old_items.end(), new_items.begin(), new_items.end(), ElementObserverListBackInserter(elements));
	for (auto& element : elements)
	{
		if (element)
			element->DispatchEvent(id, parameters);
	}
}

void Context::Release()
{
	if (instancer)
	{
		instancer->ReleaseContext(this);
	}
}

void Context::SetDocumentsBaseTag(const String& tag)
{
	documents_base_tag = tag;
}

const String& Context::GetDocumentsBaseTag()
{
	return documents_base_tag;
}

void Context::RequestNextUpdate(double delay)
{
	RMLUI_ASSERT(delay >= 0.0);
	next_update_timeout = Math::Min(next_update_timeout, delay);
}

double Context::GetNextUpdateDelay() const
{
	return next_update_timeout;
}

} // namespace Rml
