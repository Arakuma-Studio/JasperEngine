// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PHYSICSSYSTEM_H
#define PHYSICSSYSTEM_H

#include "SubSystem.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "component/Rigidbody2DComponent.h"
#include "component/Joint2DComponent.h"
#include "component/CharacterController2DComponent.h"
#include "object/physics/Contact2D.h"
#include "object/physics/Manifold2D.h"
#include "box2d/box2d.h"
#endif

#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "object/physics/CollideShapeResult3D.h"
#include "object/physics/Contact3D.h"
#include "object/physics/ControllerColliderHit.h"
#include "component/CharacterControllerComponent.h"
#include "Jolt/Jolt.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Physics/Body/MotionQuality.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h"
#include "Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h"
#include "Jolt/Physics/Collision/ObjectLayerPairFilterMask.h"
#endif

#include "registry/TagManager.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace jasperengine{

	class JoltActivationListener;
	class JoltContactListener;
	class JoltObjectLayerTable;
	class JoltBroadPhaseLayers;
	class JoltObjectVsBroadPhaseLayerFilter;
	class JoltObjectLayerPairFilter;
	class CharacterController2DSystem;
	class CharacterControllerSystem;
	

	class JASPERENGINE_API PhysicsSystem : public SubSystem {

		friend class CharacterController2DSystem;
		friend class CharacterControllerSystem;

	private:
		Vector2 gravity2D;
		Vector3 gravity3D;

		bool interpolation;

#ifdef JASPERENGINE_PHYSICS_2D
		b2WorldId world2D;
		float pointsToMeterScale2D;
		bool steppingWorld2D = false;

		// Layer Collision Matrix rows the 2D world last applied, and GameObjects whose
		// layer changed inside a step (Box2D is locked then), applied after it
		uint32_t appliedLayerRows2D[MAX_LAYERS];
		uint32_t appliedLayerVersion2D = 0;
		std::vector<Entity> pendingLayerRefresh2D;
		std::vector<b2ShapeId> shapeScratch2D;

		// the 2D CharacterControllers (S22 P5), moved on this world's queries
		std::unique_ptr<CharacterController2DSystem> characters2D;
#endif
#ifdef JASPERENGINE_PHYSICS_3D
		bool lock3DBodies;
		bool steppingWorld3D = false;

		void flushPendingDestroys3D();
		bool useLockedInterfaces3D() const;

		JoltActivationListener* activationListener3D;
		JoltContactListener* contactListener3D;

        // Process-wide singletons shared by every scene; not owned here.
        JPH::TempAllocatorImpl* temp_allocator;
        JPH::JobSystem* job_system;

		JPH::PhysicsSystem world3D;

		// Handles a teardown could not release while stepping. After world3D so they are
		// destroyed first, releasing while the world they belong to is still alive.
		std::vector<JPH::BodyID> pendingBodyDestroy3D;
		std::vector<JPH::Ref<JPH::TwoBodyConstraint>> pendingJointDestroy3D;

		// the CharacterControllers (S22 P2): Jolt CharacterVirtuals in world3D, so after it
		// (destroyed first, while the world their inner bodies are in still exists)
		std::unique_ptr<CharacterControllerSystem> characters3D;

		// ObjectLayers index this table (GameObject layer, category and mask bits)
		JoltObjectLayerTable* objectLayerTable3D;
		JoltBroadPhaseLayers* broad_phase_layer_interface;
		JoltObjectVsBroadPhaseLayerFilter* object_vs_broadphase_layer_filter;
		JoltObjectLayerPairFilter* object_vs_object_layer_filter;

		// as for 2D: matrix rows last applied, and filter changes made inside a step
		uint32_t appliedLayerRows3D[MAX_LAYERS];
		uint32_t appliedLayerVersion3D = 0;
		std::vector<Entity> pendingLayerRefresh3D;
		std::vector<JPH::BodyID> bodyScratch3D;

		// Jolt runs contact and activation callbacks on its job threads; the subscribers
		// (Lua above all) take turns on this
		std::recursive_mutex callbackMutex3D;

		std::unordered_set<Entity> warnedNonNormalizedRotations;
		std::unordered_set<Entity> reportedInvalidRotations;
#endif

#ifdef JASPERENGINE_PHYSICS_2D
		void ensureWorld2D();
		// Moves the body by what something other than physics moved its Transform, position
		// and rotation each on its own. Inside a step (stepTime > 0) a kinematic body is moved
		// there with a velocity instead, spread over the fixed steps left in the frame, so it
		// carries and pushes other bodies (teleported when a step would take it further than
		// its own size or more than a quarter turn).
		void updateRigidbody2DPosition(Signature signature, Entity entity, Rigidbody2DComponent& body, float stepTime = 0.0f);
		// Puts the parts a following kinematic body is on its way to where its Transform has
		// them, after the last step of the frame
		void finishFollowing2D(Entity entity, Rigidbody2DComponent& body);
		bool loadJoint2D(Entity entity, Joint2DComponent& joint);
		bool syncRigidbody2DColliders(Entity entity, Rigidbody2DComponent& body);
		static Vector2 absScale2D(const Vector2& scale);
		static float maxScaleXY(const Vector2& scale);

		// Re-pairs the shapes on layers whose matrix row changed since the last step
		void syncLayerCollisionMatrix2D();
		void refilterShapes2D(b2BodyId bodyId, bool toggleRefilterBit, int layer);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
		// The 3D counterpart of updateRigidbody2DPosition()
		void updateRigidbodyPosition(Signature signature, Entity entity, RigidbodyComponent& body, float stepTime = 0.0f);
		void finishFollowing3D(Entity entity, RigidbodyComponent& body);
		bool loadJoint3D(Entity entity, Joint3DComponent& joint);
		bool syncRigidbodyColliders(Entity entity, RigidbodyComponent& body);
		bool isShapeSourceLoading(Entity entity, const Collider3D& shapeData) const;
		bool createCollider3DForIndex(Entity entity, RigidbodyComponent& body, size_t index);

		bool createGenericJoltBody(Entity entity, RigidbodyComponent& body, const JPH::ShapeRefC shape);

		static Vector3 absScale(const Vector3& scale);
		static float maxScaleXZ(const Vector3& scale);
		static float maxScaleXYZ(const Vector3& scale);

		// Wakes the bodies on layers whose matrix row changed since the last step
		void syncLayerCollisionMatrix3D();
		void releaseObjectLayer3D(const JPH::BodyID& bodyId);
		// Wakes a body and whatever overlaps it, so resting pairs are tested again
		void wakeAround3D(const JPH::BodyID& bodyId);
#endif

	public:
		PhysicsSystem(Scene* scene);
		virtual ~PhysicsSystem();

		Vector2 getGravity2D() const;
		void setGravity2D(Vector2 gravity);
		void setGravity2D(float x, float y);

		Vector3 getGravity3D() const;
		void setGravity3D(Vector3 gravity);
		void setGravity3D(float x, float y, float z);

		// sets both 2D (x, y) and 3D worlds
		Vector3 getGravity() const;
		void setGravity(Vector3 gravity);
		void setGravity(float x, float y);
		void setGravity(float x, float y, float z);

		// Removes every event callback, as before the code behind them goes away
		void clearSubscriptions();

		// Ends the following of a kinematic body's position and/or rotation (the script took
		// over that part): the velocity the following set stops, one a script set since stays
#ifdef JASPERENGINE_PHYSICS_2D
		void stopFollowing2D(Rigidbody2DComponent& body, bool position, bool rotation);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
		void stopFollowing3D(RigidbodyComponent& body, bool position, bool rotation);
#endif

		// Draws bodies between their last two fixed steps, so they move smoothly at any frame
		// rate (on by default; the scene setting physics_interpolation)
		void setInterpolation(bool interpolation);
		bool isInterpolation() const;
		// Removes the event callbacks with the substring in their tag
		void removeSubscriptionsByTag(const std::string& substring);

#ifdef JASPERENGINE_PHYSICS_2D
		float getPointsToMeterScale2D() const;
		void setPointsToMeterScale2D(float pointsToMeterScale2D);

		bool isSteppingWorld2D() const;
		// Re-applies the GameObject layer to the shapes of the entity's Rigidbody 2D;
		// inside a step, right after it
		void refreshRigidbody2DLayer(Entity entity);

		// A 2D CharacterController's move (points) and simpleMove (points per second), at once
		// (docs/mcp/guide/physics.md): the collision flags, and whether it is grounded
		int moveCharacter2D(Entity entity, const Vector2& motion);
		bool simpleMoveCharacter2D(Entity entity, const Vector2& speed);
#endif

		// The entity's world pose now, its parents first, when the render pass has not
		// resolved its Transform yet
		void resolveTransformWorldPose(Entity entity);
		// Puts the entity at a world pose (written parent-local)
		void writeTransformWorldPose(Entity entity, const Vector3& position, const Quaternion& rotation);

#ifdef JASPERENGINE_PHYSICS_3D
        void setLock3DBodies(bool lock3DBodies);
        bool isLock3DBodies() const;

        // Everything reaching a Jolt body goes through these: during a step the locking
        // variants deadlock, as the calling thread already holds the locks. Queries take
        // body locks of their own, so a ray cast needs the no-lock one too.
        JPH::BodyInterface& getBodyInterface3D();
        const JPH::BodyLockInterface& getBodyLockInterface3D();
        const JPH::NarrowPhaseQuery& getNarrowPhaseQuery3D();

        // True inside a contact, activation or collision callback. Jolt keeps the broad
        // phase and the constraint list locked for the whole step, so a body or joint
        // cannot be created then (refused, retried next update) and teardown is queued.
        bool isSteppingWorld3D() const;
        std::recursive_mutex& getCallbackMutex3D();

        const JoltObjectLayerTable& getObjectLayerTable3D() const;
        // Re-applies the GameObject layer and the category/mask bits of the entity's
        // Rigidbody to its Jolt body; inside a step, right after it
        void refreshRigidbodyFilter3D(Entity entity);

		static JPH::EMotionQuality getRigidbodyMotionQualityToJolt(RigidbodyMotionQuality motionQuality);
#endif

#ifdef JASPERENGINE_PHYSICS_2D
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> beginContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> endContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> beginSensorContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> endSensorContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)> hitContact2D;
		FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)> preSolve2D;

		FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> shouldCollide2D;
#endif

#ifdef JASPERENGINE_PHYSICS_3D
		FunctionSubscribe<void(Rigidbody)> onBodyActivated3D;
		FunctionSubscribe<void(Rigidbody)> onBodyDeactivated3D;
		FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)> onContactAdded3D;
		FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)> onContactPersisted3D;
		FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)> onContactRemoved3D;

		FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)> shouldCollide3D;
		// what a CharacterController's move ran into, after the move (and Lua onControllerColliderHit)
		FunctionSubscribe<void(Entity, ControllerColliderHit)> onControllerColliderHit;

		// A CharacterController's move (world units, no gravity) at once: its CollisionFlags
		int moveCharacter(Entity entity, const Vector3& motion);
		// Moves at speed (y ignored) with its own gravity: whether it is grounded
		bool simpleMoveCharacter(Entity entity, const Vector3& speed);
		// A new capsule, refused (false, the old one kept) when it does not fit there
		bool trySetCharacterSize(Entity entity, float height, float radius, const Vector3& center);
		// S21's Animator: a velocity (world units/s) and a yaw rate (rad/s) the controller moves
		// and turns by in every fixed step until the next call
		void setRootMotion(Entity entity, const Vector3& worldVelocity, float yawRate);
		// the kinematic body a CharacterController with detectCollisions puts in the world
		bool isCharacterInnerBody(const JPH::BodyID& body) const;
#endif

#ifdef JASPERENGINE_PHYSICS_2D
		void createRigidbody2D(Entity entity);
		void removeRigidbody2D(Entity entity);
#endif

#ifdef JASPERENGINE_PHYSICS_3D
		void createRigidbody(Entity entity);
		void removeRigidbody(Entity entity);
#endif

#ifdef JASPERENGINE_PHYSICS_2D
		b2WorldId getWorld2D() const;
		b2BodyId getBody(Entity entity);

		bool loadRigidbody2D(Entity entity);
		void destroyRigidbody2D(Rigidbody2DComponent& body);
		// Makes the body of a Rigidbody 2D that has none yet, placed at its entity, as
		// load() does. Scripts start (init(), C++ constructors) before their scene enters
		// the Engine, so a body they reach then would not exist yet.
		bool ensureRigidbody2DLoaded(Entity entity);
#endif

#ifdef JASPERENGINE_PHYSICS_3D
		JPH::PhysicsSystem* getWorld3D();
		bool loadRigidbody(Entity entity);
		void destroyRigidbody(RigidbodyComponent& body);
		// The 3D counterpart of ensureRigidbody2DLoaded()
		bool ensureRigidbodyLoaded(Entity entity);
#endif

		// Writes the body pose into its entity, alpha of the way from its pose before the last
		// step (1: the pose itself), so the transform sync of the next step does not undo a
		// pose written straight to the body.
#ifdef JASPERENGINE_PHYSICS_2D
		void updateTransformFromRigidbody2D(Entity entity, Rigidbody2DComponent& body, float alpha);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
		void updateTransformFromRigidbody(Entity entity, RigidbodyComponent& body, float alpha);
#endif

#ifdef JASPERENGINE_PHYSICS_3D
		JPH::Quat toValidatedJoltRotation(const Quaternion& rotation, Entity entity, int colliderIndex);
#endif

#ifdef JASPERENGINE_PHYSICS_2D
		int loadCollider2D(Rigidbody2DComponent& body, void* shape, Collider2DType type);
		void destroyCollider2D(Rigidbody2DComponent& body, size_t index);
#endif

#ifdef JASPERENGINE_PHYSICS_3D
		int loadCollider3D(RigidbodyComponent& body, const Vector3& position, const Quaternion& rotation, JPH::ShapeSettings* shapeSettings);
		void destroyCollider3D(RigidbodyComponent& body, size_t index);
#endif

#ifdef JASPERENGINE_PHYSICS_2D
		bool loadDistanceJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchorA, Vector2 anchorB, bool autoAnchors, bool rope);
		bool loadRevoluteJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor);
		bool loadPrismaticJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor, Vector2 axis);
		bool loadMouseJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 target);
		bool loadWheelJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor, Vector2 axis);
		bool loadWeldJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor);
		bool loadMotorJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB);
		void destroyJoint2D(Joint2DComponent& joint);
#endif

#ifdef JASPERENGINE_PHYSICS_3D
		bool loadFixedJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB);
		bool loadDistanceJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, bool autoAnchors);
		bool loadPointJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor);
		bool loadHingeJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 axis, Vector3 normal);
		bool loadConeJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 twistAxis);
		bool loadPrismaticJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 sliderAxis, float limitsMin, float limitsMax);
		bool loadSwingTwistJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 twistAxis, Vector3 planeAxis, float normalHalfConeAngle, float planeHalfConeAngle, float twistMinAngle, float twistMaxAngle);
		bool loadSixDOFJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, Vector3 axisX, Vector3 axisY);
		bool loadPathJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, std::vector<Vector3> positions, std::vector<Vector3> tangents, std::vector<Vector3> normals, Vector3 pathPosition, bool isLooping);
		bool loadGearJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Entity hingeA, Entity hingeB, int numTeethGearA, int numTeethGearB);
		bool loadRackAndPinionJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Entity hinge, Entity slider, int numTeethRack, int numTeethGear, int rackLength);
		bool loadPulleyJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, Vector3 fixedPointA, Vector3 fixedPointB);
		void destroyJoint3D(Joint3DComponent& joint);

		void addBroadPhaseLayer3D(uint8_t index, uint32_t groupsToInclude);
		void addBroadPhaseLayer3D(uint8_t index, uint32_t groupsToInclude, uint32_t groupsToExclude);
#endif

		void load() override;
		void draw() override;
		void destroy() override;
		void update(double dt) override;
		void fixedUpdate(double dt) override;
		// Once a frame after its fixed steps: the bodies physics still owns the Transform of
		// are drawn alpha (Engine::getInterpolationAlpha) of the way from their pose before
		// the last step
		void interpolate(double alpha);

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;
		void onEntityLayerChanged(Entity entity) override;
	};

}

#endif //PHYSICSSYSTEM_H
