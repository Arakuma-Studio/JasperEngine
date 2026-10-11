#pragma once

#include "ID.h"
#include "Tween.h"
#include "Types.h"
#include <cmath>

namespace Rml {

// animation-direction
enum class AnimationDirection : uint8_t { Normal, Reverse, Alternate, AlternateReverse };
// animation-fill-mode
enum class AnimationFillMode : uint8_t { None, Forwards, Backwards, Both };

/* One animation of the element's computed animation list, built from the animation-* longhands (phase 3). */
struct Animation {
	float duration = 0.0f;
	// animation-timing-function: the easing of each keyframe segment that has no animation-timing-function of its own.
	Tween tween = Tween::CubicBezier(0.25f, 0.1f, 0.25f, 1.f);
	float delay = 0.0f;
	// The number of iterations, fractional counts allowed; INFINITY for 'infinite'.
	float iteration_count = 1.0f;
	AnimationDirection direction = AnimationDirection::Normal;
	AnimationFillMode fill_mode = AnimationFillMode::None;
	bool paused = false;
	String name;

	bool IsInfinite() const { return std::isinf(iteration_count); }
};

/* One transition of the element's computed transition list, built from the transition-* longhands (phase 3). */
struct Transition {
	PropertyId id = PropertyId::Invalid; // Invalid: 'all'.
	Tween tween = Tween::CubicBezier(0.25f, 0.1f, 0.25f, 1.f);
	float duration = 0.0f;
	float delay = 0.0f;
	bool allow_discrete = false; // transition-behavior: allow-discrete
};

struct TransitionList {
	bool none = true; // No entry.
	bool all = false; // An entry applies to every property.
	Vector<Transition> transitions;

	TransitionList() {}
	TransitionList(bool none, bool all, Vector<Transition> transitions) : none(none), all(all), transitions(std::move(transitions)) {}

	/// The transition of the given property: the last entry that names it or 'all'; nullptr if there is none.
	const Transition* Find(PropertyId id) const
	{
		for (auto it = transitions.rbegin(); it != transitions.rend(); ++it)
			if (it->id == id || it->id == PropertyId::Invalid)
				return &*it;
		return nullptr;
	}
};

inline bool operator==(const Animation& a, const Animation& b)
{
	return a.duration == b.duration && a.tween == b.tween && a.delay == b.delay && a.iteration_count == b.iteration_count &&
		a.direction == b.direction && a.fill_mode == b.fill_mode && a.paused == b.paused && a.name == b.name;
}
inline bool operator!=(const Animation& a, const Animation& b)
{
	return !(a == b);
}
inline bool operator==(const Transition& a, const Transition& b)
{
	return a.id == b.id && a.tween == b.tween && a.duration == b.duration && a.delay == b.delay && a.allow_discrete == b.allow_discrete;
}
inline bool operator!=(const Transition& a, const Transition& b)
{
	return !(a == b);
}
inline bool operator==(const TransitionList& a, const TransitionList& b)
{
	return a.none == b.none && a.all == b.all && a.transitions == b.transitions;
}
inline bool operator!=(const TransitionList& a, const TransitionList& b)
{
	return !(a == b);
}

} // namespace Rml
