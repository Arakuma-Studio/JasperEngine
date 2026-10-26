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

namespace jasperengine{

	class JASPERENGINE_API ActionSystem : public SubSystem {

    private:

		// Per-target weighted pose accumulator used to blend keyframe-track
		// animations (translate/rotate/scale/morph) that target the same bone.
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
		};
		struct MorphBlendAccum {
			float sums[MAX_MORPHTARGETS] = {0};
			float weights[MAX_MORPHTARGETS] = {0};
		};
		std::unordered_map<Entity, TransformBlendAccum> transformBlend;
		std::unordered_map<Entity, MorphBlendAccum> morphBlend;

		void clearPoseBlend();
		void flushPoseBlend();

		void actionStateChange(Entity entity, ActionComponent& action);

		void actionComponentStart(ActionComponent& action);
		void actionComponentStop(ActionComponent& action);
		void actionComponentPause(ActionComponent& action);
		void actionUpdate(double dt, ActionComponent& action);

		void animationUpdate(double dt, Entity entity, ActionComponent& action, AnimationComponent& animcomp);
		std::vector<Entity> animationChildEntities(const AnimationComponent& animcomp);
		void animationStopChildren(const std::vector<Entity>& children);
		void animationPauseChildren(const std::vector<Entity>& children);

		// Sprite action functions
		void setSpriteTextureRect(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionStart(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionStop(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionUpdate(double dt, Entity entity, ActionComponent& action, MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);

		void tweenStop(TweenComponent& tween);
		void tweenUpdate(double dt, Entity entity, ActionComponent& action, TweenComponent& tween);

		void positionTweenUpdate(TweenComponent& tween, PositionTweenComponent& posaction, Transform& transform);
		void rotationTweenUpdate(TweenComponent& tween, RotationTweenComponent& rotaction, Transform& transform);
		void scaleTweenUpdate(TweenComponent& tween, ScaleTweenComponent& scaletween, Transform& transform);

		void colorTweenUpdate(TweenComponent& tween, ColorTweenComponent& colortween, Vector4& targetColor);
		void alphaTweenUpdate(TweenComponent& tween, AlphaTweenComponent& alphatween, Vector4& targetColor);

		// an action whose target is a particle system and which has no other kind (D1): its
		// start, stop and pause play, stop and pause the system
		bool isParticleController(Entity entity, const ActionComponent& action);

		//Keyframe
		void keyframeUpdate(double dt, ActionComponent& action, KeyframeTracksComponent& keyframe);
		void translateTracksUpdate(KeyframeTracksComponent& keyframe, TranslateTracksComponent& translatetracks, Entity target, float weight);
		void scaleTracksUpdate(KeyframeTracksComponent& keyframe, ScaleTracksComponent& scaletracks, Entity target, float weight);
		void rotateTracksUpdate(KeyframeTracksComponent& keyframe, RotateTracksComponent& rotatetracks, Entity target, float weight);
		void morphTracksUpdate(KeyframeTracksComponent& keyframe, MorphTracksComponent& morpthtracks, Entity target, float weight);

		void processRunningAction(double dt, Entity entity, ActionComponent& action);

		float getDuration(Entity entity, int depth);
		bool isAnimationReachable(Entity from, Entity target, std::unordered_set<Entity>& visited);

	public:
		ActionSystem(Scene* scene);

		void actionStart(Entity entity);
		void actionStop(Entity entity);
		void actionPause(Entity entity);

		void updateAnimationPreview(double dt, Entity entity);
		void updateAnimationPreview(double dt, const std::vector<Entity>& entities);
		void updateActionPreview(double dt, Entity entity);

		float getDuration(Entity entity);
		float getFrameDuration(const ActionFrame& frame);
		// True if target is reachable from `from` through action frames (from == target counts),
		// i.e. adding `from` as an action of target's animation would create a cycle.
		bool isAnimationReachable(Entity from, Entity target);

		void load() override;
		void resetRunningActions();
		void draw() override;
		void destroy() override;
		void update(double dt) override;

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;
	};

}

#endif //ACTIONSYSTEM_H
