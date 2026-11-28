#pragma once

#include "../../Include/RmlUi/Core/Header.h"
#include "../../Include/RmlUi/Core/ScrollTypes.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

/**
    Implements scrolling behavior that occurs over time.

    Scrolling modes are activated externally, targeting a given element. The actual scrolling takes place during update calls.
 */

class ScrollController {
public:
	enum class Mode {
		None,
		Smoothscroll, // Smooth scrolling to target distance.
		Autoscroll,   // Scrolling with middle mouse button.
		Inertia,      // Applying scrolling inertia when using swipe gesture
	};

	void ActivateAutoscroll(Element* target, Vector2i start_position);
	void ActivateSmoothscroll(Element* target, Vector2f delta_distance, ScrollBehavior scroll_behavior);
	// Inertia with the default physics keeps 6.3's friction; other physics (fork, S93 R9) decelerate as uGUI's ScrollRect
	// and spring an elastic overscroll back (also with no velocity).
	void ActivateInertia(Element* target, Vector2f velocity, const ScrollPhysics& physics = ScrollPhysics());
	// Fork (S93 R10): the inertia's velocity (0 in the other modes), and a stop that keeps an elastic return going.
	Vector2f GetVelocity() const { return mode == Mode::Inertia ? inertia_scroll_velocity : Vector2f(0.f); }
	void StopInertia();

	void InstantScrollOnTarget(Element* target, Vector2f delta_distance);

	bool Update(Vector2i mouse_position, float dp_ratio);

	void IncrementSmoothscrollTarget(Vector2f delta_distance);

	// Resets any active mode and its state.
	void Reset();

	// Sets the scroll behavior for mouse wheel processing and scrollbar interaction.
	void SetDefaultScrollBehavior(ScrollBehavior scroll_behavior, float speed_factor);

	// Returns the autoscroll cursor based on the active scroll velocity.
	String GetAutoscrollCursor(Vector2i mouse_position, float dp_ratio) const;
	// Returns true if autoscroll mode is active and the cursor has been moved outside the idle scroll area.
	bool HasAutoscrollMoved() const;

	Mode GetMode() const { return mode; }
	Element* GetTarget() const { return target; }

private:
	// Updates time to now, and returns the delta time since the previous time update.
	float UpdateTime();

	void UpdateAutoscroll(float dt, Vector2i mouse_position, float dp_ratio);

	void UpdateSmoothscroll(float dt, float dp_ratio);

	void UpdateInertia(float dt);
	// Fork (S93 R9): the inertia of non-default physics.
	void UpdatePhysics(float dt);
	// Fork (S93 R9): another scroll mode takes over the target: an elastic overscroll ends at rest.
	void SettleOverscroll();

	bool HasSmoothscrollReachedTarget() const;

	void PerformScrollOnTarget(Vector2f delta_distance);

	Mode mode = Mode::None;

	Element* target = nullptr;
	double previous_update_time = 0;

	Vector2i autoscroll_start_position;
	Vector2f autoscroll_accumulated_length;
	bool autoscroll_moved = false;

	bool smoothscroll_prefer_instant = false;
	float smoothscroll_speed_factor = 1.f;

	Vector2f smoothscroll_target_distance;
	Vector2f smoothscroll_scrolled_distance;
	Vector2f smoothscroll_accumulated_fractional_distance;

	Vector2f inertia_scroll_velocity;
	// Fork (S93 R9): the physics of the inertia, and its exact scroll position (scroll offsets are whole pixels).
	ScrollPhysics inertia_physics;
	Vector2f inertia_position;
};

} // namespace Rml
