#pragma once

#include "../../Include/RmlUi/Core/Animation.h"
#include "../../Include/RmlUi/Core/Header.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/Tween.h"

namespace Rml {

struct AnimationKey {
	AnimationKey(float time, const Property& property, Tween tween, bool has_tween = true) :
		time(time), property(property), tween(tween), has_tween(has_tween)
	{}
	float time; // The key's offset in the iteration, in [0, 1].
	Property property;
	Tween tween;    // The easing of the segment that starts at this key (the last key's is unused).
	bool has_tween; // False: the segment takes the animation's timing function (a keyframe without its own).
};

// The origin is tracked for determining its behavior when adding and removing animations.
// User: Animation started by the Element API, applied to the inline style.
// Animation: A CSS animation started by the 'animation-*' properties, applied to the animation layer.
// Transition: A CSS transition, applied to the transition layer.
enum class ElementAnimationOrigin : uint8_t { User, Animation, Transition };

// The phase of an animation at a time (Web Animations): during its delay, while it runs, or after its end.
enum class AnimationPhase : uint8_t { Idle, Before, Active, After };

/**
    The animated values of one property: keys at offsets in [0, 1] of an iteration, interpolated with the easing of the
    segment's start key.
 */
class AnimationTrack {
public:
	AnimationTrack(PropertyId property_id) : property_id(property_id) {}

	/// Appends a key; keys must be added in increasing offset. Returns false if the value cannot be animated.
	bool AddKey(float offset, const Property& property, Element& element, Tween tween, bool has_tween);

	/// The value at the (directed) iteration progress; the default easing is used for keys without their own.
	Property GetValue(float progress, const Tween& default_tween, bool before_flag, Element& element) const;

	PropertyId GetPropertyId() const { return property_id; }
	const Vector<AnimationKey>& GetKeys() const { return keys; }
	Vector<AnimationKey>& GetKeys() { return keys; }
	bool IsInitialized() const { return !keys.empty(); }

private:
	PropertyId property_id;
	Vector<AnimationKey> keys;
};

/**
    One animation of an element: a user animation or a transition of one property, or a CSS animation of the properties
    of its keyframes. Its time follows the Web Animations timing model: a start time on the context's clock, a delay
    (with backwards fill), iterations of a duration (fractional and infinite counts), a direction, a fill mode and a
    play state that holds the local time while paused.
 */
class ElementAnimation {
public:
	ElementAnimation() {}
	/// A user animation or a transition of one property, starting at 'current_value'. Keys are added with AddKey.
	ElementAnimation(PropertyId property_id, ElementAnimationOrigin origin, const Property& current_value, Element& element, double start_world_time,
		float duration, int num_iterations, bool alternate_direction);
	/// A CSS animation: the timing of an entry of the element's animation list; its tracks are added with AddTrack.
	ElementAnimation(const Animation& animation, int occurrence, double start_world_time);

	/// Adds a key at 'target_time' seconds into the iteration (user animations and transitions); 'extend_duration' makes
	/// it the end of the iteration.
	bool AddKey(float target_time, const Property& property, Element& element, Tween tween, bool extend_duration);

	/// Adds the track of one property (CSS animations).
	void AddTrack(AnimationTrack track);

	/// The phase and directed progress at a time.
	struct Sample {
		AnimationPhase phase = AnimationPhase::Before;
		bool has_value = false;      // The animation applies a value (active, or filling).
		float progress = 0.f;        // Directed iteration progress, in [0, 1].
		double iteration = 0;        // The current iteration.
		bool before_flag = false;    // For steps(): before the active interval in the playback direction.
		float elapsed_time = 0.f;    // Active time (seconds, without the delay), for events.
	};
	Sample GetSample(double world_time) const;

	/// The value of a track for a sample.
	Property GetValue(size_t track_index, const Sample& sample, Element& element) const;

	/// Advances a user animation (or a test) to the given time and returns its value; an empty property when it has no
	/// value (before its start).
	Property UpdateAndGetProperty(double world_time, Element& element);

	/// Pauses or resumes at the given time (animation-play-state): the local time is held while paused.
	void SetPaused(bool paused, double world_time);
	/// Updates the timing of a running CSS animation from its (changed) entry, keeping its start time and play state.
	void UpdateTiming(const Animation& animation, double world_time);

	/// The transition's values, for the CSS reversing rule.
	void SetTransitionData(const Transition& transition, const Property& reversing_adjusted_start, float reversing_shortening_factor);
	const Property& GetReversingAdjustedStart() const { return reversing_adjusted_start; }
	float GetReversingShorteningFactor() const { return reversing_shortening_factor; }
	const Property* GetEndValue() const;
	const Property* GetStartValue() const;

	/// The phase the events were last dispatched for, and the iteration.
	AnimationPhase GetEventPhase() const { return event_phase; }
	double GetEventIteration() const { return event_iteration; }
	void SetEventState(AnimationPhase phase, double iteration)
	{
		event_phase = phase;
		event_iteration = iteration;
	}

	PropertyId GetPropertyId() const { return tracks.empty() ? PropertyId::Invalid : tracks[0].GetPropertyId(); }
	size_t GetNumTracks() const { return tracks.size(); }
	const AnimationTrack& GetTrack(size_t index) const { return tracks[index]; }
	float GetDuration() const { return duration; }
	float GetDelay() const { return delay; }
	/// The duration of all iterations (infinite: INFINITY).
	float GetActiveDuration() const;
	bool IsPaused() const { return paused; }
	/// True when the animation has reached its end (a finite animation in its after phase).
	bool IsComplete() const { return animation_complete; }
	void SetComplete(bool complete) { animation_complete = complete; }
	bool IsTransition() const { return origin == ElementAnimationOrigin::Transition; }
	bool IsInitalized() const { return !tracks.empty() && tracks[0].IsInitialized(); }
	/// The eased progress of a transition or user animation at its last update (CSS reversing rule).
	float GetInterpolationFactor() const { return last_eased_progress; }
	ElementAnimationOrigin GetOrigin() const { return origin; }
	const String& GetName() const { return name; }
	int GetOccurrence() const { return occurrence; }
	const Animation& GetAnimation() const { return animation; }
	bool AllowsDiscrete() const { return allow_discrete; }

	/// Records the eased progress of a single-track animation at a sample (transitions, user animations).
	void RecordProgress(const Sample& sample);

private:
	ElementAnimationOrigin origin = ElementAnimationOrigin::User;
	Vector<AnimationTrack> tracks;

	// Timing.
	double start_time = 0;   // World time of local time 0 (the start of the delay).
	float delay = 0;
	float duration = 0;      // Of one iteration.
	float iteration_count = 1;
	AnimationDirection direction = AnimationDirection::Normal;
	AnimationFillMode fill_mode = AnimationFillMode::None;
	bool paused = false;
	double hold_time = 0;    // The local time while paused.
	Tween timing_function;   // CSS animations: the easing of keys without their own; transitions: their easing.

	// CSS animations.
	String name;
	int occurrence = 0;
	Animation animation;

	// Transitions.
	Property reversing_adjusted_start;
	float reversing_shortening_factor = 1.f;
	bool allow_discrete = false;

	AnimationPhase event_phase = AnimationPhase::Idle;
	double event_iteration = 0;
	float last_eased_progress = 0.f;
	bool animation_complete = false;
};

} // namespace Rml
