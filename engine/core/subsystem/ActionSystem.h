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
#include "component/PointsComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/AnimationComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/LatticeComponent.h"

namespace jasperengine{

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
		};
		struct MorphBlendAccum {
			float sums[MAX_MORPHTARGETS] = {0};
			float weights[MAX_MORPHTARGETS] = {0};
		};
		// Per lattice point of the target (sized to its point count when first touched in a
		// frame). Entries survive clearPoseBlend() with active = false so the vectors keep
		// their capacity; entries of destroyed entities are dropped there.
		struct LatticeBlendAccum {
			std::vector<Vector3> sums;
			std::vector<float> weights;
			bool active = false;
		};
		std::unordered_map<Entity, TransformBlendAccum> transformBlend;
		std::unordered_map<Entity, MorphBlendAccum> morphBlend;
		std::unordered_map<Entity, LatticeBlendAccum> latticeBlend;

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
		void animationApplyEnd(ActionComponent& action, AnimationComponent& animcomp, bool durationPassed);

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

		//Particle helpers functions
		float getParticleInverseScale(float value);
		Vector3 getParticleDisplayScale(Vector3 scale, Transform* targetTransform);
		bool isParticleWorldSpace(ParticleSystemComponent& particles, Transform* targetTransform);
		void updateParticleTargetTransform(Transform& transform);
		void updateParticleTargetCache(ParticleSystemComponent& particles, Transform* targetTransform);
		Vector3 samplePositionInitializer(ParticlePositionInitializer& init);
		int sampleBurstCount(ParticleBurst& burst);
		void sortParticleBursts(ParticleSystemComponent& particles);
		void sortParticleColorGradient(ParticleSystemComponent& particles);
		Vector3 sampleParticleColorGradient(const ParticleColorGradient& gradient, float time, float life);
		float getParticleCycleDuration(ParticleSystemComponent& particles);
		Vector3 getParticleSimulationPosition(ParticleSystemComponent& particles, Transform* targetTransform, Vector3 position);
		Vector3 getParticleSimulationDirection(ParticleSystemComponent& particles, Transform* targetTransform, Vector3 direction);
		Quaternion getParticleSimulationRotation(ParticleSystemComponent& particles, Transform* targetTransform, Quaternion rotation);
		Vector3 getParticleDisplayPosition(ParticleSystemComponent& particles, Transform* targetTransform, Vector3 position);
		Quaternion getParticleDisplayRotation(ParticleSystemComponent& particles, Transform* targetTransform, Quaternion rotation);
		Vector3 getParticleDisplayScale(ParticleSystemComponent& particles, Transform* targetTransform, Vector3 scale);
		void syncParticleInstance(size_t idx, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, Transform* targetTransform);
		void syncParticlePoint(size_t idx, ParticleSystemComponent& particles, PointsComponent& points, Transform* targetTransform);

		int findUnusedParticle(ParticleSystemComponent& particles);

		float getFloatInitializerValue(float& min, float& max);
		Vector3 getVector3InitializerValue(Vector3& min, Vector3& max, bool linearSort);
		Quaternion getQuaternionInitializerValue(Quaternion& min, Quaternion& max, bool shortestPath);
		Rect getSpriteInitializerValue(std::vector<int>& frames, SpriteComponent& sprite);
		Rect getSpriteInitializerValue(std::vector<int>& frames, PointsComponent& points);
		void applyParticleInitializers(size_t idx, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform);
		void applyParticleInitializers(size_t idx, ParticleSystemComponent& particles, PointsComponent& points, Transform* targetTransform);

		float getTimeFromParticleTime(float& time, float& fromTime, float& toTime);
		bool getParticleModifierValue(float& particleTime, float& fromTime, float& toTime, Ease& function, float& value);
		float getFloatModifierValue(float& value, float& fromValue, float& toValue);
		Vector3 getVector3ModifierValue(float& value, Vector3& fromValue, Vector3& toValue);
		Quaternion getQuaternionModifierValue(float& value, Quaternion& fromValue, Quaternion& toValue, bool shortestPath);
		Rect getSpriteModifierValue(float& value, std::vector<int>& frames, SpriteComponent& sprite);
		Rect getSpriteModifierValue(float& value, std::vector<int>& frames, PointsComponent& points);
		void applyParticleModifiers(size_t idx, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform);
		void applyParticleModifiers(size_t idx, ParticleSystemComponent& particles, PointsComponent& points, Transform* targetTransform);
		void advanceParticle(size_t idx, float dt, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform);
		void advanceParticle(size_t idx, float dt, ParticleSystemComponent& particles, PointsComponent& points, Transform* targetTransform);

		void particleActionStart(ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, MeshComponent& mesh);
		void particleActionStart(ParticleSystemComponent& particles, PointsComponent& points);
		void particlesActionUpdate(double dt, Entity entity, Entity target, ActionComponent& action, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh);
		void particlesActionUpdate(double dt, Entity entity, Entity target, ActionComponent& action, ParticleSystemComponent& particles, PointsComponent& points);

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

	public:
		ActionSystem(Scene* scene);

		void actionStart(Entity entity);
		void actionStop(Entity entity);
		void actionPause(Entity entity);
		void particleActionReset(Entity entity);

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
