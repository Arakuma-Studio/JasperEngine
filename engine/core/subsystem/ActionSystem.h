// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ACTIONSYSTEM_H
#define ACTIONSYSTEM_H

#include "SubSystem.h"

#include <unordered_map>
#include <unordered_set>

#include "component/Transform.h"
#include "component/SpriteComponent.h"
#include "component/MeshComponent.h"
#include "component/RectTransformComponent.h"
#include "component/UIComponent.h"
#include "component/ActionComponent.h"
#include "component/TweenComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/LatticeComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "animation/ClipTypes.h"
#include "animation/PropertyBindings.h"

namespace jasperengine{

	class AnimationClip;
	namespace anim{
		class ClipPreviewSession;
		class AnimationPlayerRuntime;
		struct TrackSample;
		struct ChannelRest;
		struct PropertyRest;
		struct ClipBindings;
		struct PlayerStateRuntime;
		struct QueuedState;
		struct PlayerPoseScratch;
	}

	class JASPERENGINE_API ActionSystem : public SubSystem {

    private:

		// Per-target weighted pose accumulator used to blend keyframe-track
		// animations (translate/rotate/scale/morph/lattice) that target the same bone.
		// Each contributing animation adds its sampled value scaled by its blend
		// weight; flushPoseBlend() then writes the normalized (weighted-average)
		// result once per target. This makes blending order-independent and lets
		// multiple animations crossfade smoothly.
		struct TransformBlendAccum {
			Vector3 positionSum = Vector3(0, 0, 0);
			float positionWeight = 0.0f;
			Vector3 scaleSum = Vector3(0, 0, 0);
			float scaleWeight = 0.0f;
			Quaternion rotationSum = Quaternion(0, 0, 0, 0);
			Quaternion rotationRef;
			bool hasRotationRef = false;
			float rotationWeight = 0.0f;
			// an Animation Player's rest value of the channel (animation clips spec 9.R1): when the
			// total weight W of every contributor is < 1, (1 - W) * rest joins the average. Never
			// set by entity animations (their output is unchanged).
			bool hasRestPosition = false;
			bool hasRestRotation = false;
			bool hasRestScale = false;
			Vector3 restPosition = Vector3(0, 0, 0);
			Quaternion restRotation;
			Vector3 restScale = Vector3(1, 1, 1);

			// back to "nothing accumulated" (clearPoseBlend keeps the map's nodes between frames;
			// the reference and rest values only count with their flags set)
			void reset(){
				positionSum.x = positionSum.y = positionSum.z = 0.0f;
				positionWeight = 0.0f;
				scaleSum.x = scaleSum.y = scaleSum.z = 0.0f;
				scaleWeight = 0.0f;
				rotationSum.w = rotationSum.x = rotationSum.y = rotationSum.z = 0.0f;
				rotationWeight = 0.0f;
				hasRotationRef = false;
				hasRestPosition = hasRestRotation = hasRestScale = false;
			}
			bool empty() const { return positionWeight <= 0.0f && scaleWeight <= 0.0f && rotationWeight <= 0.0f; }
		};
		struct MorphBlendAccum {
			float sums[MAX_MORPHTARGETS] = {0};
			float weights[MAX_MORPHTARGETS] = {0};
			bool hasRest[MAX_MORPHTARGETS] = {false};
			float rest[MAX_MORPHTARGETS] = {0};
		};
		// Per lattice point of the target (sized to its point count when first touched in a
		// frame). Entries survive clearPoseBlend() with active = false so the vectors keep
		// their capacity; entries of destroyed entities are dropped there.
		struct LatticeBlendAccum {
			std::vector<Vector3> sums;
			std::vector<float> weights;
			bool active = false;
			// per point, as TransformBlendAccum's rest (empty when no player registered one)
			std::vector<char> hasRest;
			std::vector<Vector3> rest;
		};
		// One animated property of an entity (anim-properties spec 8, Accumulation), keyed by
		// (entity, property key): per component the weighted sums in key space and the rest fill
		// (as morph weights); a discrete property the contributor with the largest weight (ties:
		// the later one) against the rest's 1 - W. Nodes stay between frames (`active` false).
		struct PropertyBlendAccum {
			Entity target = NULL_ENTITY;
			// the rest and the binding written through (anim::PropertyRest::binding): valid
			// while active (a rest map node of a player or preview, alive until the flush)
			const anim::PropertyRest* rest = nullptr;
			float sums[4] = {0, 0, 0, 0};
			float weights[4] = {0, 0, 0, 0};
			float total = 0.0f;			// discrete: Σ w
			float bestWeight = -1.0f;	// discrete: the winner so far (-1: none)
			float bestValue = 0.0f;
			bool active = false;
		};
		// a write that runs user callbacks (GameObject active, a script's enabled): applied after
		// every other write of the flush and outside the update's pose blend, in hierarchy order
		// (parents first, siblings in their order), each re-checked (spec decision 6)
		struct DeferredPropertyWrite {
			Entity target = NULL_ENTITY;
			float value = 0.0f;			// key space
			anim::PropertyBinding binding;
		};
		std::unordered_map<Entity, TransformBlendAccum> transformBlend;
		std::unordered_map<Entity, MorphBlendAccum> morphBlend;
		std::unordered_map<Entity, LatticeBlendAccum> latticeBlend;
		std::unordered_map<uint64_t, PropertyBlendAccum> propertyBlend;
		// the property accumulator of (target, key) for this frame (reset on its first touch)
		PropertyBlendAccum& propertyAccum(Entity target, uint32_t key, const anim::PropertyRest* rest);
		// writes the properties (the deferred ones are appended to `deferred`, applied by
		// applyDeferredPropertyWrites after everything else)
		void flushPropertyBlend(std::vector<DeferredPropertyWrite>& deferred);
		// the flushes' deferred writes not applied yet (update() applies them once its pose blend
		// scope ended, so an onEnable they run can sample a player)
		std::vector<DeferredPropertyWrite> deferredPropertyWrites;
		void applyDeferredPropertyWrites();
	public:
		// an entity's place in hierarchy order (its Transform's index: parents before their
		// children, siblings in order; SIZE_MAX without a Transform), the order deferred
		// property writes are applied in
		static size_t hierarchyIndex(Scene* scene, Entity entity);
	private:

		void clearPoseBlend();
		void flushPoseBlend();

		// Applies the stop, start and pause triggers. Returns the entity's ActionComponent after
		// their callbacks (which can move it), or nullptr when one removed it or the entity
		ActionComponent* actionStateChange(Entity entity, ActionComponent& action);

		void actionComponentStart(ActionComponent& action);
		void actionComponentStop(ActionComponent& action);
		void actionComponentPause(ActionComponent& action);
		void actionUpdate(double dt, ActionComponent& action);

		void animationUpdate(double dt, Entity entity, ActionComponent& action, AnimationComponent& animcomp);
		std::vector<Entity> animationChildEntities(const AnimationComponent& animcomp);
		void animationStopChildren(const std::vector<Entity>& children);
		void animationPauseChildren(const std::vector<Entity>& children);
		void animationApplyEnd(ActionComponent& action, AnimationComponent& animcomp, bool durationPassed);

		// Sprite action functions
		void setSpriteTextureRect(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionStart(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionStop(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionUpdate(double dt, Entity entity, ActionComponent& action, MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);

		void tweenStop(TweenComponent& tween);
		// true when a non-looping tween reached its end
		bool tweenUpdate(double dt, Entity entity, ActionComponent& action, TweenComponent& tween);

		void positionTweenUpdate(TweenComponent& tween, PositionTweenComponent& posaction, Transform& transform);
		void rotationTweenUpdate(TweenComponent& tween, RotationTweenComponent& rotaction, Transform& transform);
		void scaleTweenUpdate(TweenComponent& tween, ScaleTweenComponent& scaletween, Transform& transform);

		void colorTweenUpdate(TweenComponent& tween, ColorTweenComponent& colortween, Vector4& targetColor);
		void alphaTweenUpdate(TweenComponent& tween, AlphaTweenComponent& alphatween, Vector4& targetColor);

		// an action whose target is a particle system and which has no other kind (D1): its
		// start, stop and pause play, stop and pause the system
		bool isParticleController(Entity entity, const ActionComponent& action);
		// ---- the legacy timeline's sound (docs/specs/S40-timeline-audio.md R17-R21)
		// an action entity with an AudioSource (audio action) or an AudioSnapshotComponent (Mixer
		// Snapshot action) and no other kind of action
		bool isTimelineAudioAction(Entity entity);
		// what animationUpdate passes to the start of an audio or snapshot child it places
		struct TimelineStartContext {
			bool active = false;
			uint32_t delay = 0;
			float speedScale = 1.0f;
			Entity animation = NULL_ENTITY;
			int frame = -1;
			float startTime = 0.0f;
		};
		TimelineStartContext timelineStart;
		// inside the editor's animation and action previews: no game sound (the editor previews
		// timeline audio with its own voices, S40 R23)
		bool timelinePreview = false;
		// an Animation ending by itself (not a stop()): its audio actions' sounds play out (S40 D6)
		bool animationEndingNaturally = false;
		void startTimelineAudio(Entity entity, ActionComponent& action, ActionState before);
		void stopTimelineAudio(Entity entity);
		// the audio and snapshot frames of a running Animation (R20): each starts once per pass,
		// placed inside the frame, and stops when its frame ends. T: the timecount at entry, advance: this update's
		void animationAudioFrames(Entity entity, double dt, float advance, int& passed, float& actionsEnd, bool afterWrap);
		// an editor preview steps no systems: a running controller advances its particles itself
		void previewParticleController(double dt, Entity entity, const ActionComponent& action);

		//Keyframe
		void keyframeUpdate(double dt, ActionComponent& action, KeyframeTracksComponent& keyframe);
		void translateTracksUpdate(KeyframeTracksComponent& keyframe, TranslateTracksComponent& translatetracks, Entity target, float weight);
		void scaleTracksUpdate(KeyframeTracksComponent& keyframe, ScaleTracksComponent& scaletracks, Entity target, float weight);
		void rotateTracksUpdate(KeyframeTracksComponent& keyframe, RotateTracksComponent& rotatetracks, Entity target, float weight);
		void morphTracksUpdate(KeyframeTracksComponent& keyframe, MorphTracksComponent& morpthtracks, Entity target, float weight);
		void latticeTracksUpdate(KeyframeTracksComponent& keyframe, LatticeTracksComponent& latticetracks, Entity target, LatticeComponent& latticecomp, float weight);

		void processRunningAction(double dt, Entity entity, ActionComponent& action);

		float getDuration(Entity entity, int depth);
		bool isAnimationReachable(Entity from, Entity target, std::unordered_set<Entity>& visited);

		// Animation Players (animation clips spec 3.3; ActionSystemPlayers.cpp): they
		// live in this system so they share the pose blend with entity animations. update()
		// calls updatePlayers after the entity animations and before flushPoseBlend, and
		// dispatchPlayerCallbacks after it (events and onFinish, in time order: callbacks may
		// add components and move arrays, so nothing is held across them).
		void updatePlayers(double dt);
		void dispatchPlayerCallbacks();

		// players with events or finished states to dispatch
		std::vector<Entity> playersWithCallbacks;
		// "<clip>|<function>" of clip events already warned about having no handler (spec 3.5:
		// once per (clip, function), whatever the number of players)
		std::unordered_set<std::string> warnedEventHandlers;
		// a build without the soundlayer module met audio cues (S40 §6.6), said once
		bool warnedNoSoundLayer = false;
		// AnimationClipPool::revision() seen by the last player update (a change: compare generations)
		uint64_t playerPoolRevision = 0;
		// update() is between its clearPoseBlend and flushPoseBlend (samplePlayer then waits for it)
		bool poseBlendScope = false;
		// this player update's hierarchy log (EntityRegistry::takeHierarchyLog): the top ancestors
		// of renamed entities and the destroyed ones, for the bind validation
		uint64_t playerFrame = 0;
		std::unordered_set<Entity> renamedTops;
		std::unordered_set<Entity> destroyedEntities;
		bool hierarchyLogOverflow = true;

		// loads the states' clips, follows models and the hierarchy (rebinding); false while
		// waiting for a model under the root to load (R10)
		bool preparePlayer(Entity entity, AnimationPlayerComponent& player, float dt, bool poolChanged);
		void advancePlayer(Entity entity, AnimationPlayerComponent& player, float dt);
		void startQueuedState(Entity entity, AnimationPlayerComponent& player, const anim::QueuedState& head, float offset, float dt);
		void samplePlayerStates(Entity entity, AnimationPlayerComponent& player);
		// the audio cues its states crossed this update, then the state_exit stops (S40 R4-R7)
		void firePlayerCues(Entity entity, AnimationPlayerComponent& player, float dt);
		// one clip at `clipTime` with `weight` into the pose blend, rest values from `rest` (taken
		// there on the first write of an entity); `written` (optional) gets the entries written
		// (`writtenProperties`, optional: the (entity, property key) entries written)
		void accumulateClip(const AnimationClip& clip, anim::ClipBindings& bindings, float clipTime, float weight,
		                    std::unordered_map<Entity, anim::ChannelRest>& rest,
		                    std::vector<std::pair<Entity, ClipChannel>>* written,
		                    std::vector<std::pair<Entity, uint32_t>>* writtenProperties = nullptr);

		// The layered evaluator (anim layers spec 7.3; ActionSystemLayers.cpp). A player that is
		// not anim::isDefaultLayering samples through it: preparePlayerLayers (end of
		// preparePlayer) loads the layers' masks, resolves the humanoid rig a humanoid mask needs,
		// builds the slot table and the per-track mask weights and additive references when their
		// stamps change; evaluateLayeredPlayer composes the layers into the pose blend.
		void preparePlayerLayers(Entity entity, AnimationPlayerComponent& player, float dt, bool modelsReloaded);
		void evaluateLayeredPlayer(Entity entity, AnimationPlayerComponent& player, bool includeFinishing);
		// the shared core (players and the layered preview): scratch.stateInputs per
		// scratch.layerInputs, composed bottom-up, every composed slot added to the pose blend
		// with weight 1; `written` (optional) gets the (entity, channel) entries written
		void composePlayerLayers(anim::PlayerPoseScratch& scratch, std::vector<std::pair<Entity, ClipChannel>>* written,
		                         std::vector<std::pair<Entity, uint32_t>>* writtenProperties = nullptr);
		// AnimatorAssetPool::revision() seen by the last player update (layer masks)
		uint64_t playerMaskPoolRevision = 0;
		bool playerMaskPoolChanged = false;

	public:
		ActionSystem(Scene* scene);

		// newRun false: a restart inside the same run of its animation (keeps a relative track's base)
		void actionStart(Entity entity, bool newRun = true);
		void actionStop(Entity entity);
		void actionPause(Entity entity);

		// Edit-mode preview of clips on a root (animation clips spec 9.R21): what the Animation
		// window's clip mode and MCP preview_animation drive (through AnimationClipController).
		struct PlayerPreviewState {
			// with `clip` null: the name of an entry of the root's AnimationPlayerComponent::clips
			// (the authored list, state names by anim::playerStateNames), loaded through
			// AnimationClipPool; the component's runtime is neither needed nor created in edit mode
			std::string state;
			// an explicit clip (a document revision, a clip not in the list, a preview model)
			std::shared_ptr<const AnimationClip> clip;
			// a state time in seconds (unwrapped values allowed: a playing preview just adds dt),
			// mapped into the clip by `wrap`; Default = the entry's wrap (R14) for a named state,
			// else the clip's own
			float time = 0.0f;
			ClipWrapMode wrap = ClipWrapMode::Default;
			float weight = 1.0f;
			// layered previews: the index into PlayerPreview::layers it plays on
			int layer = 0;
			// humanoid clips (anim-retarget): -1 the entry's Mirror / Foot IK (an explicit clip:
			// off), else 0 / 1; a named state of a Retarget entry plays its clip retargeted
			int mirror = -1;
			int footIK = -1;
			// -1 the entry's Retarget (an explicit clip: off), else 0 / 1: a generic clip played
			// retargeted from its source model (the one retarget path); `reference` is an explicit
			// clip's reference (a sub-clip names its model; else the clip's source or Preview Model)
			int retarget = -1;
			std::string reference;
		};
		// one layer of a layered preview (anim layers spec 7.5): the Animation window's "Preview
		// over", MCP preview_animation `over`
		struct PlayerPreviewLayer {
			float weight = 1.0f;		// ignored for layer 0
			AnimationLayerBlend blend = AnimationLayerBlend::Override;
			std::string mask;			// asset-relative .mask (AnimatorAssetPool::mask), "" = none
		};
		struct PlayerPreview {
			std::vector<PlayerPreviewState> states;
			// events crossed since the session's previous call (session.previousTimes) reach the
			// root's player onEvent; off by default: edit-mode previews never run game code
			bool fireEvents = false;
			// compose the states per layer through the layered evaluator (layers, their masks and
			// blends); false: the states blend as one layer, as without layers
			bool layered = false;
			std::vector<PlayerPreviewLayer> layers;
		};
		struct PlayerPreviewResult {
			// every state's clip was available (false: a state name unknown, its clip not loaded
			// or failed: nothing of that state is written)
			bool ready = false;
			// the entities written this call, in clip track order, each once
			std::vector<Entity> written;
			// the (entity, channel) entries written this call (also accumulated in session.written)
			std::vector<std::pair<Entity, ClipChannel>> channels;
			// the (entity, property key) entries written this call (session.writtenProperties)
			std::vector<std::pair<Entity, uint32_t>> properties;
			// paths of tracks that did not resolve (unique)
			std::vector<std::string> unresolved;
			// a humanoid clip whose humanoid tracks did not play: why (no humanoid rig on the root)
			std::string humanoidError;
		};
		// Its own clear/flush scope (like updateAnimationPreview), runs while the system is paused.
		// Rest values come from `session.rest`: on the first write of an entity's channel the
		// current scene value is taken there (the editor may set it before with setRest); blending
		// as in play (R1: (1 - W) * rest where the states' total weight W < 1). Entries no state
		// animates are not written. Drops the session's bindings of clips this call does not name.
		// Main thread.
		PlayerPreviewResult previewPlayer(Entity root, const PlayerPreview& preview, anim::ClipPreviewSession& session);
		// Ends a preview: writes session.rest back to every entry in session.written (the same
		// update flags as the flush: anim::writeChannelValue), then session.reset(). Main thread.
		void restorePlayerPreview(anim::ClipPreviewSession& session);
		// Headless sampling (MCP get_animation `times`, curves view): every track of `clip` at
		// `time` (a clip time: wrap is the caller's), raw values (relative tracks not combined)
		static void sampleClip(const AnimationClip& clip, float time, std::vector<anim::TrackSample>& out);
		// Back to the starting state (playAutomatically decided again, clips loaded again, rest
		// forgotten): the editor calls it when Play stops (Project.cpp, next to resetAnimators)
		void resetPlayers();
		// AnimationPlayer::sample(): the player's states at their current times written once now
		// (inside this system's update the running pass does it). Main thread.
		void samplePlayer(Entity entity);

		void updateAnimationPreview(double dt, Entity entity);
		void updateAnimationPreview(double dt, const std::vector<Entity>& entities);
		void updateActionPreview(double dt, Entity entity);

		float getDuration(Entity entity);
		float getFrameDuration(const ActionFrame& frame);
		// The pose relative tracks add to, where an anchored UI element moves by its layout offset
		bool getRelativeTrackPose(Entity target, Vector3& position, Quaternion& rotation, Vector3& scale);
		// Maps relative translate values from base to the target's parent space
		Matrix4 getRelativeTrackMatrix(Entity target, const Vector3& base);
		// True if target is reachable from `from` through action frames (from == target counts),
		// i.e. adding `from` as an action of target's animation would create a cycle.
		bool isAnimationReachable(Entity from, Entity target);

		void load() override;
		void resetRunningActions();
		void draw() override;
		void destroy() override;
		void update(double dt) override;

		// the unscaled delta of an update whose scaled delta is dt (Tween.useUnscaledTime)
		static double unscaledDeltatime(double dt);

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;

	private:
		// previewPlayer's layered branch: the states' bindings (`bindings`, by index) composed per
		// preview.layers through composePlayerLayers (ActionSystemLayers.cpp)
		struct LayeredPreviewState {
			size_t bindings = 0;
			float time = 0.0f;
			float weight = 0.0f;
			int layer = 0;
		};
		void previewLayers(Entity root, const PlayerPreview& preview, anim::ClipPreviewSession& session,
		                   std::vector<anim::ClipBindings>& bindings, const std::vector<LayeredPreviewState>& states,
		                   std::vector<std::pair<Entity, ClipChannel>>& written,
		                   std::vector<std::pair<Entity, uint32_t>>& writtenProperties);
	};

}

#endif //ACTIONSYSTEM_H
