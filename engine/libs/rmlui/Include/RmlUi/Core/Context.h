#pragma once

#include "Header.h"
#include "Input.h"
#include "ScriptInterface.h"
#include "ScrollTypes.h"
#include "Traits.h"
#include "Types.h"

namespace Rml {

class Stream;
class ContextInstancer;
class ElementDocument;
class EventListener;
class DataModel;
class DataModelConstructor;
class DataTypeRegister;
class ScrollController;
class RenderManager;
class TextInputHandler;
enum class EventId : uint16_t;

/**
    A context for storing, rendering, and processing RML documents. Multiple contexts can exist simultaneously.
 */

class RMLUICORE_API Context : public ScriptInterface {
public:
	/// Constructs a new, uninitialised context. This should not be called directly, use CreateContext() instead.
	/// @param[in] name The name of the context.
	/// @param[in] render_manager The render manager used for this context.
	/// @param[in] text_input_handler The text input handler used for this context.
	Context(const String& name, RenderManager* render_manager, TextInputHandler* text_input_handler);
	/// Destroys a context.
	virtual ~Context();

	/// Returns the name of the context.
	/// @return The context's name.
	const String& GetName() const;

	/// Changes the dimensions of the context.
	/// @param[in] dimensions The new dimensions of the context.
	void SetDimensions(Vector2i dimensions);
	/// Returns the dimensions of the context.
	/// @return The current dimensions of the context.
	Vector2i GetDimensions() const;

	/// Changes the ratio of the 'dp' unit to the 'px' unit.
	/// @param[in] dp_ratio The new density-independent pixel ratio of the context.
	void SetDensityIndependentPixelRatio(float dp_ratio);
	/// Returns the ratio of the 'dp' unit to the 'px' unit.
	/// @return The current density-independent pixel ratio of the context.
	float GetDensityIndependentPixelRatio() const;

	/// Updates all elements in the context's documents.
	/// This must be called before Context::Render, but after any elements have been changed, added, or removed.
	bool Update();
	/// Renders all visible elements in the context's documents.
	bool Render();
	/// Fork (S16-F1): every element's background, border, outline and box shadow is generated again (the render interface
	/// started or stopped drawing primitives).
	void DirtyBackgrounds();

	/// Creates a new, empty document and places it into this context.
	/// @param[in] instancer_name The name of the instancer used to create the document.
	/// @return The new document, or nullptr if no document could be created.
	ElementDocument* CreateDocument(const String& instancer_name = "body");
	/// Load a document into the context.
	/// @param[in] document_path The path to the document to load. The path is passed directly to the file interface which is used to load the file.
	/// The default file interface accepts both absolute paths and paths relative to the working directory.
	/// @return The loaded document, or nullptr if no document was loaded.
	ElementDocument* LoadDocument(const String& document_path);
	/// Load a document into the context.
	/// @param[in] document_stream The opened stream, ready to read.
	/// @return The loaded document, or nullptr if no document was loaded.
	ElementDocument* LoadDocument(Stream* document_stream);
	/// Load a document into the context.
	/// @param[in] document_rml The string containing the document RML.
	/// @param[in] source_url Optional string used to set the document's source URL, or naming the document for log messages.
	/// @return The loaded document, or nullptr if no document was loaded.
	ElementDocument* LoadDocumentFromMemory(const String& document_rml, const String& source_url = "[document from memory]");
	/// Unload the given document.
	/// @param[in] document The document to unload.
	/// @note The destruction of the document is deferred until the next call to Context::Update().
	void UnloadDocument(ElementDocument* document);
	/// Unloads all loaded documents.
	/// @note The destruction of the documents is deferred until the next call to Context::Update().
	void UnloadAllDocuments();

	/// Enable or disable handling of the mouse cursor from this context.
	/// When enabled, changes to the cursor name are passed to the system interface.
	/// @param[in] enable True to enable mouse cursor handling, false to disable.
	void EnableMouseCursor(bool enable);

	/// Activate or deactivate a media theme. Themes can be used in RCSS media queries.
	/// @param theme_name[in] The name of the theme to (de)activate.
	/// @param activate True to activate the given theme, false to deactivate.
	void ActivateTheme(const String& theme_name, bool activate);
	/// Check if a given media theme has been activated.
	/// @param theme_name The name of the theme.
	/// @return True if the theme is activated.
	bool IsThemeActive(const String& theme_name) const;

	/// Returns the first document in the context with the given id.
	/// @param[in] id The id of the desired document.
	/// @return The document (if it was found), or nullptr if no document exists with the ID.
	ElementDocument* GetDocument(const String& id);
	/// Returns a document in the context by index.
	/// @param[in] index The index of the desired document.
	/// @return The document (if one exists with this index), or nullptr if the index was invalid.
	ElementDocument* GetDocument(int index);
	/// Returns the number of documents in the context.
	int GetNumDocuments() const;

	/// Returns the hover element.
	/// @return The element the mouse cursor is hovering over.
	Element* GetHoverElement();
	/// Returns the focus element.
	/// @return The element with input focus.
	Element* GetFocusElement();
	/// Returns the root element that holds all the documents
	/// @return The root element.
	Element* GetRootElement();

	/// Returns the youngest descendent of the given element which is under the given point in screen coordinates.
	/// @param[in] point The point to test.
	/// @param[in] ignore_element If set, this element and its descendents will be ignored.
	/// @param[in] element Used internally.
	/// @return The element under the point, or nullptr if nothing is.
	Element* GetElementAtPoint(Vector2f point, const Element* ignore_element = nullptr, Element* element = nullptr) const;
	/// Fork (phase 4): elements the pointer passes through. An element under the point for which the filter returns true
	/// is not hit itself, as with 'pointer-events: none' on that element alone: its descendants are still hit, and the
	/// point goes on to what is below it (a lower document, or nothing). Every hit test of the context (hover, presses,
	/// touches, drag hover) uses it. An empty filter (the default) passes through nothing.
	/// @param[in] filter The test, called with an element the point is within.
	void SetPointerPassThrough(Function<bool(Element*)> filter);

	/// Brings the document to the front of the document stack.
	/// @param[in] document The document to pull to the front of the stack.
	void PullDocumentToFront(ElementDocument* document);
	/// Sends the document to the back of the document stack.
	/// @param[in] document The document to push to the bottom of the stack.
	void PushDocumentToBack(ElementDocument* document);
	/// Remove the document from the focus history and focus the previous document.
	/// @param[in] document The document to unfocus.
	void UnfocusDocument(ElementDocument* document);

	/// Adds an event listener to the context's root element.
	/// @param[in] event The name of the event to attach to.
	/// @param[in] listener Listener object to be attached.
	/// @param[in] in_capture_phase True if the listener is to be attached to the capture phase, false for the bubble phase.
	void AddEventListener(const String& event, EventListener* listener, bool in_capture_phase = false);
	/// Removes an event listener from the context's root element.
	/// @param[in] event The name of the event to detach from.
	/// @param[in] listener Listener object to be detached.
	/// @param[in] in_capture_phase True to detach from the capture phase, false from the bubble phase.
	void RemoveEventListener(const String& event, EventListener* listener, bool in_capture_phase = false);

	/// Sends a key down event into this context.
	/// @param[in] key_identifier The key pressed.
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if the event was not consumed (ie, was prevented from propagating by an element), false if it was.
	bool ProcessKeyDown(Input::KeyIdentifier key_identifier, int key_modifier_state);
	/// Sends a key up event into this context.
	/// @param[in] key_identifier The key released.
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if the event was not consumed (ie, was prevented from propagating by an element), false if it was.
	bool ProcessKeyUp(Input::KeyIdentifier key_identifier, int key_modifier_state);

	/// Sends a single Unicode character as text input into this context.
	/// @param[in] character The Unicode code point to send into this context.
	/// @return True if the event was not consumed (ie, was prevented from propagating by an element), false if it was.
	bool ProcessTextInput(Character character);
	/// Sends a single ascii character as text input into this context.
	bool ProcessTextInput(char character);
	/// Sends a string of text as text input into this context.
	/// @param[in] string The UTF-8 string to send into this context.
	/// @return True if the event was not consumed (ie, was prevented from propagating by an element), false if it was.
	bool ProcessTextInput(const String& string);

	/// Sends a mouse movement event into this context.
	/// @param[in] x The x-coordinate of the mouse cursor, in window-coordinates (ie, 0 should be the left of the client area).
	/// @param[in] y The y-coordinate of the mouse cursor, in window-coordinates (ie, 0 should be the top of the client area).
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if the mouse is not interacting with any elements in the context (see 'IsMouseInteracting'), otherwise false.
	bool ProcessMouseMove(int x, int y, int key_modifier_state);
	/// Sends a mouse-button down event into this context.
	/// @param[in] button_index The index of the button that was pressed. Left: 0, Right: 1, Middle: 2.
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if the mouse is not interacting with any elements in the context (see 'IsMouseInteracting'), otherwise false.
	bool ProcessMouseButtonDown(int button_index, int key_modifier_state);
	/// Sends a mouse-button up event into this context.
	/// @param[in] button_index The index of the button that was release. Left: 0, Right: 1, Middle: 2.
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if the mouse is not interacting with any elements in the context (see 'IsMouseInteracting'), otherwise false.
	bool ProcessMouseButtonUp(int button_index, int key_modifier_state);
	/// Sends a mousescroll event into this context.
	/// @deprecated Please use the Vector2f version of this function.
	bool ProcessMouseWheel(float wheel_delta, int key_modifier_state);
	/// Sends a mousescroll event into this context, and scrolls the document unless the event was stopped from propagating.
	/// @param[in] wheel_delta The mouse-wheel movement this frame, with positive values being directed right and down.
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if the event was not consumed (ie, was prevented from propagating by an element), false if it was.
	bool ProcessMouseWheel(Vector2f wheel_delta, int key_modifier_state);
	/// Tells the context the mouse has left the window. This removes any hover state from all elements and prevents 'Update()' from setting the hover
	/// state for elements under the mouse.
	/// @return True if the mouse is not interacting with any elements in the context (see 'IsMouseInteracting'), otherwise false.
	/// @note The mouse is considered activated again after the next call to 'ProcessMouseMove()'.
	bool ProcessMouseLeave();

	/// Fork (P7-C): ends the drag in progress as if it never reached a target: 'dragout' on the element dragged over
	/// (verbose drags), then 'dragend' with the parameter 'cancelled' = 1 on the dragged element. The button still held
	/// clicks nothing when it is released. Does nothing without a drag.
	void CancelDrag();
	/// Fork (P7-C): true while an element is being dragged (its 'dragstart' was sent).
	bool IsDragging() const;

	/// Process touch movements for this context.
	/// @param[in] touches List of touches.
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if no touch points are interacting with any elements in the context, otherwise false.
	bool ProcessTouchMove(const TouchList& touches, int key_modifier_state);
	/// Process touch start (press) for this context.
	/// @param[in] touches List of touches.
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if no touch points are interacting with any elements in the context, otherwise false.
	bool ProcessTouchStart(const TouchList& touches, int key_modifier_state);
	/// Process touch end (release) for this context.
	/// @param[in] touches List of touches.
	/// @param[in] key_modifier_state The state of key modifiers (shift, control, caps-lock, etc.) keys; this should be generated by ORing together
	/// members of the Input::KeyModifier enumeration.
	/// @return True if no touch points are interacting with any elements in the context, otherwise false.
	bool ProcessTouchEnd(const TouchList& touches, int key_modifier_state);
	/// Process touch cancel for this context.
	/// @param[in] touches List of touches.
	/// @return True if no touch points are interacting with any elements in the context, otherwise false.
	bool ProcessTouchCancel(const TouchList& touches);

	/// Fork (engine-gaps G5-1..G5-6): DOM Pointer Events. Every pointer has its own hover chain, active chain, pointer
	/// capture, drag and click. The mouse is pointer MousePointerId (1): these calls with a mouse input drive it as the
	/// ProcessMouse...() calls do, which dispatch the pointer events too. A touch or pen pointer exists from its down
	/// to its up or cancel: it hovers only while down, is captured by its pointerdown target (implicit capture), and
	/// pans the nearest scroll container past the touch slop where 'touch-action' allows it (then it gets
	/// 'pointercancel'). Every pointer also dispatches the compatibility mouse events (mousedown, mouseup, mousemove,
	/// mouseover, mouseout, click, dblclick) with its 'pointer_id'. Event parameters: 'pointer_id', 'pointer_type'
	/// ("mouse", "touch", "pen"), 'is_primary', 'button' (-1 for moves), 'buttons', 'mouse_x', 'mouse_y', 'pressure',
	/// 'width', 'height' and the key modifiers.
	/// @return True if the pointer is not interacting with any element (see 'IsPointerInteracting').
	bool ProcessPointerDown(const PointerInput& input);
	bool ProcessPointerMove(const PointerInput& input);
	bool ProcessPointerUp(const PointerInput& input);
	/// A pointer ends without a click or drop ('pointercancel', no compatibility event).
	bool ProcessPointerCancel(PointerId id);
	/// The pointer left the window: its hover goes (the mouse: as ProcessMouseLeave).
	bool ProcessPointerLeave(PointerId id);
	/// The element the pointer is over (the mouse: GetHoverElement()); nullptr for an unknown pointer.
	Element* GetHoverElement(PointerId id);
	/// The pointer hovers over or has activated an element of this context, or pans one.
	bool IsPointerInteracting(PointerId id) const;
	/// A touch or pen pointer is down (the mouse: always true).
	bool HasPointer(PointerId id) const;
	/// Pointer capture (Element::SetPointerCapture...): the capturing element gets the pointer's move, up and cancel
	/// events (and the compatibility mouse events) wherever it is; ':hover' still follows what is under it. Setting it
	/// needs the pointer to be pressed; 'gotpointercapture' / 'lostpointercapture' fire before its next event.
	bool SetPointerCapture(Element* element, PointerId id);
	bool ReleasePointerCapture(Element* element, PointerId id);
	bool HasPointerCapture(const Element* element, PointerId id) const;

	/// Returns a hint on whether the mouse is currently interacting with any elements in this context, based on previously submitted
	/// 'ProcessMouse...()' commands.
	/// @note Interaction is determined irrespective of background and opacity. See the RCSS property 'pointer-events' to disable interaction for
	/// specific elements.
	/// @return True if the mouse hovers over or has activated an element in this context, otherwise false.
	bool IsMouseInteracting() const;

	/// Sets the default scroll behavior, such as for mouse wheel processing and scrollbar interaction.
	/// @param[in] scroll_behavior The default smooth scroll behavior, set to instant to disable smooth scrolling.
	/// @param[in] speed_factor A factor for adjusting the final smooth scrolling speed, must be strictly positive, defaults to 1.0.
	void SetDefaultScrollBehavior(ScrollBehavior scroll_behavior, float speed_factor);

	/// Retrieves the render manager which can be used to submit changes to the render state.
	RenderManager& GetRenderManager();

	/// Retrieves the text input handler.
	TextInputHandler* GetTextInputHandler() const;

	/// Sets the instancer to use for releasing this object.
	/// @param[in] instancer The context's instancer.
	void SetInstancer(ContextInstancer* instancer);

	/// Creates a data model.
	/// The returned constructor can be used to bind data variables. Elements can bind to the model using the attribute 'data-model="name"'.
	/// @param[in] name The name of the data model.
	/// @param[in] data_type_register The data type register to use for the data model, or null to use the default register.
	/// @param[in] allow_missing_variables If true, allows variables to be bound after document load. Views referencing
	///            not-yet-bound variables will silently produce default values until the variable is bound and dirtied.
	/// @return A constructor for the data model, or empty if it could not be created.
	DataModelConstructor CreateDataModel(const String& name, DataTypeRegister* data_type_register = nullptr, bool allow_missing_variables = false);
	/// Retrieves the constructor for an existing data model.
	/// The returned constructor can be used to add additional bindings to an existing model.
	/// @param[in] name The name of the data model.
	/// @return A constructor for the data model, or empty if it could not be found.
	DataModelConstructor GetDataModel(const String& name);
	/// Retrieves all data models in this context.
	/// @return A map of all data models in this context, keyed by their name.
	UnorderedMap<String, DataModelConstructor> GetDataModels() const;
	/// Removes the given data model.
	/// This also removes all data views, controllers, and bindings contained by the data model.
	/// @warning Invalidates all handles and constructors pointing to the data model.
	/// @param[in] name The name of the data model.
	/// @return True if successfully removed, false if no data model was found.
	bool RemoveDataModel(const String& name);

	/// Sets the base tag name of documents before creation. Default: "body".
	/// @param[in] tag The name of the base tag. Example: "html"
	void SetDocumentsBaseTag(const String& tag);
	/// Gets the base tag name of documents.
	/// @return The current base tag name of documents.
	const String& GetDocumentsBaseTag();

	/// Updates the time until Update() should get called again. This can be used by elements and the application to
	/// implement on-demand rendering and thus drastically save CPU/GPU and reduce power consumption during inactivity.
	/// The context stores the lowest requested timestamp, which can later be retrieved using GetNextUpdateDelay().
	/// @param[in] delay Maximum time until next update.
	void RequestNextUpdate(double delay);
	/// Get the max delay until Update() and Render() should get called again. An application can choose to only call
	/// update and render once the time has elapsed, but there's no harm in doing so more often. The returned value can
	/// be infinity, in which case Update() should be invoked after user input was received. A value of 0 means "render
	/// as fast as possible", for example if an animation is playing.
	/// @return Time until the next update is expected.
	double GetNextUpdateDelay() const;

protected:
	void Release() override;

private:
	String name;
	Vector2i dimensions;
	float density_independent_pixel_ratio = 1.f;
	String documents_base_tag = "body";

	// Wrapper around the render interface for tracking the render state.
	RenderManager* render_manager;

	SmallUnorderedSet<String> active_themes;

	ContextInstancer* instancer;

	using ElementSet = SmallOrderedSet<Element*>;
	using ElementList = Vector<Element*>;
	// Set of elements that are currently in hover state.
	ElementSet hover_chain;
	// List of elements that are currently in active state.
	ElementList active_chain;
	// History of windows that have had focus
	ElementList document_focus_history;

	// Documents that have been unloaded from the context but not yet released.
	OwnedElementList unloaded_documents;

	// Root of the element tree.
	ElementPtr root;
	// The element that currently has input focus.
	Element* focus;
	// The last key down had its default action prevented: the text input it produces is dropped, as in a browser,
	// until the next key down or key up.
	bool suppress_text_input = false;
	// The top-most element being hovered over.
	Element* hover;
	// The element that was being hovered over when the primary mouse button was pressed most recently.
	Element* active;

	// The element that was clicked on last.
	Element* last_click_element;
	// The time the last click occurred.
	double last_click_time;
	// Mouse position during the last mouse_down event.
	Vector2i last_click_mouse_position;

	// Input state; stored from the most recent input events we receive from the application.
	Vector2i mouse_position;
	bool mouse_active;

	// Fork (phase 4): SetPointerPassThrough.
	Function<bool(Element*)> pointer_pass_through;

	// The scrolling state of a touch pointer, required to implement proper inertia scrolling (PointerState::scroll).
	struct TouchState {
		bool scrolling_right = false;
		bool scrolling_down = false;
		Vector2f start_position;
		Vector2f inertia_position;
		Vector2f last_position;
		Element* scroll_container = nullptr;
		double scrolling_last_time = 0;
		double scrolling_start_time_x = 0;
		double scrolling_start_time_y = 0;
		// Fork (P7-H): a press on a draggable element inside a scroll container waits for a long press before it
		// drags; moving away first scrolls the container.
		ObserverPtr<Element> drag_candidate;
		bool drag_candidate_verbose = false;
		double start_time = 0;
	};

	// Fork (engine-gaps G5): the touch and pen pointers (the mouse keeps the members above).
	struct PointerState {
		PointerType type = PointerType::Touch;
		bool primary = false;
		Vector2f position;
		int buttons = 0;
		Element* hover = nullptr;
		ElementSet hover_chain;
		Element* active = nullptr;     // the focus element under the press: the click target
		ElementList active_chain;      // the elements it made ':active'
		Element* capture = nullptr;
		Element* pending_capture = nullptr;
		bool pending_capture_set = false;
		// drag="drag|drag-drop|clone" (G5-5)
		Element* drag = nullptr;
		bool drag_started = false;
		bool drag_verbose = false;
		Element* drag_clone = nullptr;
		Vector2f drag_clone_offset;    // the clone's position minus the pointer's
		Element* drag_hover = nullptr;
		ElementSet drag_hover_chain;
		// touch panning (G5-4)
		TouchState scroll;
		bool pan_x = true, pan_y = true; // what 'touch-action' allows from the press target to its scroll container
		bool panning = false;            // a pan started: the pointer was cancelled, it only scrolls now
	};
	SmallUnorderedMap<PointerId, PointerState> pointers;
	struct PointerClick {
		Element* element = nullptr;
		double time = 0;
		Vector2f position;
	};
	SmallUnorderedMap<PointerId, PointerClick> pointer_clicks;
	// the mouse's capture and buttons (G5-3)
	Element* mouse_capture = nullptr;
	Element* mouse_pending_capture = nullptr;
	bool mouse_pending_capture_set = false;
	int mouse_buttons = 0;
	// ':hover' and ':active' are on while any pointer holds them (G5-1, D9)
	SmallUnorderedMap<Element*, int> hover_refs;
	SmallUnorderedMap<Element*, int> active_refs;

	// Controller for various scroll behavior modes.
	UniquePtr<ScrollController> scroll_controller; // [not-null]

	// Enables cursor handling.
	bool enable_cursor;
	String cursor_name;
	// Document attached to cursor (e.g. while dragging).
	ElementPtr cursor_proxy;

	// The element that is currently being dragged (or about to be dragged).
	Element* drag;
	// True if a drag has begun (ie, the ondragstart event has been fired for the drag element), false otherwise.
	bool drag_started;
	// True if the current drag is a verbose drag (ie, sends ondragover, ondragout, ondragdrop, etc, events).
	bool drag_verbose;
	// Used when dragging a cloned object.
	Element* drag_clone;

	// The element currently being dragged over. This is equivalent to hover, but only set while an element is being
	// dragged, and excludes the dragged element.
	Element* drag_hover;
	// Set of elements that are currently being dragged over; this differs from the hover state as the dragged element
	// itself can't be part of it.
	ElementSet drag_hover_chain;

	UnorderedMap<String, UniquePtr<DataModel>> data_models;

	UniquePtr<DataTypeRegister> default_data_type_register;

	TextInputHandler* text_input_handler;

	// Time in seconds until Update and Render should be called again. This allows applications to only redraw the ui if needed.
	// See RequestNextUpdate() and NextUpdateRequested() for details.
	double next_update_timeout = 0;

	// Internal callback for when an element is detached or removed from the hierarchy.
	void OnElementDetach(Element* element);
	// Internal callback for when a new element gains focus.
	bool OnFocusChange(Element* element, bool focus_visible);

	// Generates an event for faking clicks on an element.
	void GenerateClickEvent(Element* element);

	// Updates the current hover elements, sending required events.
	void UpdateHoverChain(Vector2i old_mouse_position, int key_modifier_state = 0, Dictionary* out_parameters = nullptr,
		Dictionary* out_drag_parameters = nullptr);
	// Resets the current active element and its chain.
	void ResetActiveChain();

	// Creates the drag clone from the given element. The old drag clone will be released if necessary.
	void CreateDragClone(Element* element);
	// Releases the drag clone if one exists.
	void ReleaseDragClone();

	// Scroll the target by the given amount, using smooth scrolling.
	void PerformSmoothscrollOnTarget(Element* target, Vector2f delta_offset, ScrollBehavior scroll_behavior);

	// Returns the data model with the provided name, or nullptr if it does not exist.
	DataModel* GetDataModelPtr(const String& name) const;

	// Builds the parameters for a generic key event.
	void GenerateKeyEventParameters(Dictionary& parameters, Input::KeyIdentifier key_identifier);
	// Builds the parameters for a generic mouse event.
	void GenerateMouseEventParameters(Dictionary& parameters, int button_index = -1);
	// Builds the parameters for the key modifier state.
	void GenerateKeyModifierEventParameters(Dictionary& parameters, int key_modifier_state);
	// Builds the parameters for a drag event.
	void GenerateDragEventParameters(Dictionary& parameters);

	// Releases all unloaded documents pending destruction.
	void ReleaseUnloadedDocuments();


	// Sends the specified event to all elements in new_items that don't appear in old_items.
	static void SendEvents(const ElementSet& old_items, const ElementSet& new_items, EventId id, const Dictionary& parameters);

	// Fork (engine-gaps G5, ContextPointer.cpp).
	void AddHoverRef(Element* element);
	void RemoveHoverRef(Element* element);
	void AddActiveRef(Element* element);
	void RemoveActiveRef(Element* element);
	// the hover chain swap of a pointer: pointerout/mouseout (bubbling), pointerleave/mouseleave (not) to the old
	// elements, then pointerover/mouseover, pointerenter/mouseenter to the new ones, and the hover refs
	void SwapHoverChain(Element* old_hover, Element* new_hover, ElementSet& chain, ElementSet& new_chain, Dictionary& parameters,
		bool compat_events);
	void GeneratePointerEventParameters(Dictionary& parameters, PointerId id, PointerType type, bool primary, Vector2f position, int button,
		int buttons, float pressure, float width, float height, int key_modifier_state);
	void GenerateMousePointerParameters(Dictionary& parameters, int button_index, int key_modifier_state);
	// the capture change set since the pointer's last event: lostpointercapture, then gotpointercapture
	void ProcessPendingCapture(PointerId id);
	// the element the pointer's events go to: its capture, else what it is over
	Element* GetPointerTarget(PointerId id);
	PointerState* LookupPointer(PointerId id);
	void UpdatePointerHover(PointerId id, PointerState& state, int key_modifier_state, bool moved);
	void EndPointer(PointerId id, bool released, Vector2f position, int key_modifier_state);
	void CancelPointerPan(PointerId id, PointerState& state, int key_modifier_state);
	void ComputeTouchAction(PointerState& state, Element* target);
	void DetachFromPointers(Element* element);
	void PositionPointerDragClones();

	friend class Rml::Element;
};

} // namespace Rml
