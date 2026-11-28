#pragma once

namespace Rml {

enum class ScrollBehavior {
	Auto,    // Scroll using the context's configured setting.
	Smooth,  // Scroll using a smooth animation.
	Instant, // Scroll instantly.
};

enum class ScrollAlignment {
	Start,    // Align to the top or left edge of the scroll container.
	Center,   // Align to the center of the scroll container.
	End,      // Align to the bottom or right edge of the scroll container.
	Nearest,  // Align with minimal scroll change.
	Adaptive, // Do not scroll if already in view, otherwise align to the center of the scroll container.
};

enum class ScrollParentage {
	All,     // Scroll all ancestor scroll containers as needed.
	Closest, // Scroll only the closest scroll container.
};

/**
    Fork (S93 R8): the drag scrolling physics of a scroll container, uGUI ScrollRect's, from its properties
    'scroll-drag', 'scroll-movement', 'scroll-elasticity', 'scroll-inertia' and 'scroll-deceleration-rate'.
 */
struct ScrollPhysics {
	enum class Drag { Touch, Pointer };                       // what drags the content: touch only, or the mouse and pen too
	enum class Movement { Clamped, Elastic, Unrestricted };   // what happens past the content's edges
	Drag drag = Drag::Touch;
	Movement movement = Movement::Clamped;
	float elasticity = 0.1f;          // [s] the SmoothDamp time of the elastic return
	bool inertia = true;              // a release keeps the drag's velocity
	float deceleration_rate = -1.f;   // velocity kept per second (uGUI: v *= rate^dt); < 0: 'auto', the default curve

	// The defaults: today's touch pan and inertia, taken by the code paths of 6.3 and the engine-gaps pointers.
	bool IsDefault() const { return movement == Movement::Clamped && inertia && deceleration_rate < 0.f; }
};

/**
    Defines behavior of Element::ScrollIntoView.
 */
struct ScrollIntoViewOptions {
	ScrollIntoViewOptions(ScrollAlignment vertical = ScrollAlignment::Start, ScrollAlignment horizontal = ScrollAlignment::Nearest,
		ScrollBehavior behavior = ScrollBehavior::Instant, ScrollParentage parentage = ScrollParentage::All) :
		vertical(vertical), horizontal(horizontal), behavior(behavior), parentage(parentage)
	{}
	ScrollAlignment vertical;
	ScrollAlignment horizontal;
	ScrollBehavior behavior;
	ScrollParentage parentage;
};

} // namespace Rml
