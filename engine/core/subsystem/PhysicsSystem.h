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
#include "object/physics/Contact2D.h"
#include "object/physics/Manifold2D.h"
#include "box2d/box2d.h"
#endif

#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "object/physics/CollideShapeResult3D.h"
#include "object/physics/Contact3D.h"
#include "Jolt/Jolt.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Physics/Body/MotionQuality.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h"
#include "Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h"
#include "Jolt/Physics/Collision/ObjectLayerPairFilterMask.h"
#endif

#include "registry/TagManager.h"
#include "physics/ContactTracker.h"
#include "object/physics/Collision.h"
#include "object/physics/Collision2D.h"

#include <map>
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
	class JoltPairFilter3D;
	class ContactRecorder3D;
	struct RawContact3D;
	struct ContactState2D;
	std::shared_ptr<ContactState2D> makeContactState2D();
	enum class ScriptMessage : uint8_t;
	

	// One contact of a body for the editor and MCP: from the per-object callbacks' pairs when
	// tracked (with the step of its Enter), else read from the backend now
	struct ContactSummary{
		Entity other = NULL_ENTITY;
		int collider = -1;
		int otherCollider = -1;
		bool trigger = false;
		bool tracked = false;
		uint32_t sinceStep = 0;
		bool asleep = false;
	};

	class JASPERENGINE_API PhysicsSystem : public SubSystem {

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

		// static body the joints without bodyB hold on to (Unity: connected to the world)
		b2BodyId groundBody2D = b2_nullBodyId;
		int subSteps2D = 4;

		// joints that broke in the last step, told after its contact events (D6)
		struct JointBreak2D{
			Entity joint;
			Entity bodyA;
			bool destroy;
		};
		std::vector<JointBreak2D> pendingJointBreaks2D;
		// joints made in this world (their auto values captured); a rebuild for a body keeps them
		std::unordered_set<Entity> configuredJoints2D;

		// Box2D create calls made, for the per-collider rebuild tests and diagnostics
		uint64_t shapesCreated2D = 0;
		uint64_t chainsCreated2D = 0;

		// contact callbacks (physics-contacts): the touching shape pairs Box2D told about, the
		// collider pairs of the per-object callbacks
		std::shared_ptr<ContactState2D> contacts2D;
		ContactTracker contactTracker2D;
		// Physics2D.ignoreCollision: the body pairs and the null joint that keeps them apart
		std::map<std::pair<Entity, Entity>, b2JointId> ignoredPairs2D;
		// null joints of pairs no longer ignored, destroyed after the step that locked Box2D
		std::vector<b2JointId> pendingUnignoreJoints2D;
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
        JPH::JobSystemThreadPool* job_system;

		JPH::PhysicsSystem world3D;

		// Handles a teardown could not release while stepping. After world3D so they are
		// destroyed first, releasing while the world they belong to is still alive.
		std::vector<JPH::BodyID> pendingBodyDestroy3D;
		std::vector<JPH::Ref<JPH::TwoBodyConstraint>> pendingJointDestroy3D;

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

		// contact callbacks (physics-contacts): the job threads' records, the collider pairs of
		// the per-object callbacks, and the main-thread job system of synchronous steps
		ContactRecorder3D* contactRecorder3D;
		ContactTracker contactTracker3D;
		std::vector<RawContact3D>* rawContacts3D;
		std::unique_ptr<JPH::JobSystem> syncJobSystem3D;
		bool synchronousContactEvents3D = false;
		bool stepSynchronous3D = false;
		bool loggedAutoSynchronous3D = false;
		// seconds spent in world3D.Update and in the contact processing after it, summed
		double stepSeconds3D = 0.0;
		double contactSeconds3D = 0.0;
		// body pairs kept apart (Physics.ignoreCollision, group ids, joints)
		JPH::Ref<JoltPairFilter3D> pairFilter3D;
		// SCRIPT pair changes made while the world steps, applied after it
		struct PendingIgnore3D{ Entity a; Entity b; bool ignore; };
		std::vector<PendingIgnore3D> pendingIgnores3D;
#endif

		// a script that defines a contact message appeared or went: the listener masks of the
		// bodies are computed again before the next step
		bool listenerMasksDirty = true;
		bool warnedContactSnapshotSetter = false;

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
		// Makes the Box2D joint from every field of the component (anchor space, limits,
		// motors, springs...)
		bool buildJoint2D(Entity entity, Joint2DComponent& joint);
		// Rebuilds the dirty colliders of the body (all of them with `all`: a new body or a
		// changed scale), then its mass once
		bool syncRigidbody2DColliders(Entity entity, Rigidbody2DComponent& body, bool all);
		void createCollider2DShapes(Entity entity, Rigidbody2DComponent& body, size_t index, const Vector2& entityScale, int layer);
		// true when the pieces were made again (not from the cache); logs the problems when `log`
		bool computeCollider2DPieces(Collider2D& collider, const Vector2& entityScale, bool log, size_t index, const std::string& entityName);
		void resolveCollider2DSurface(const Rigidbody2DComponent& body, const Collider2D& collider, float& friction, float& restitution, int& userMaterial) const;
		// Re-applies the materials of bodies using one that changed (PhysicsMaterialRuntime::revision)
		void syncMaterials2D();
		// After a step: joints whose reaction passed breakForce / breakTorque; then told
		void checkJointBreaks2D();
		void dispatchJointBreaks2D();
		static Vector2 absScale2D(const Vector2& scale);
		static float maxScaleXY(const Vector2& scale);

		// The contact events of the step: the legacy global events (filtered by the authored
		// flags), the 2D stay events, then the per-object callbacks (physics-contacts 6.1.4)
		void processContacts2D();
		// The Box2D event flags of a collider's shapes: authored, or a listener asks for them
		bool applyShapeEventFlags2D(const Collider2D& collider, uint16_t mask, b2ShapeId shape) const;
		void refreshListenerMask2D(Entity entity, Rigidbody2DComponent& body);
		void syncIgnoredPairs2D();
		void destroyIgnoreJoint2D(b2JointId joint);

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

		// After the step on the main thread: the global events (unless the step was
		// synchronous), then the per-object callbacks (physics-contacts 6.1.4, 6.1.5)
		void processContacts3D();
		void refreshListenerMask3D(Entity entity, RigidbodyComponent& body);
		void applyIgnoreCollision3D(Entity a, Entity b, bool ignore);
#endif

		// the listener mask of a body: the contact messages its GameObject's scripts, and its
		// colliders' receivers' scripts, define
		uint16_t computeListenerMask(Entity entity, const std::vector<Entity>& receivers) const;
		void refreshListenerMasks();
		// a body that goes: the other side of each of its pairs gets the Exit now (D6)
		void contactBodyRemoved2D(Entity entity);
		void contactBodyRemoved3D(Entity entity);
		// a contact message to a body's GameObject and its collider's receiver
		bool contactTargetWants(Entity target, ScriptMessage message) const;

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
		// The convex pieces Box2D gets for a POLYGON collider of the entity, as the body makes
		// them (after the GameObject's scale; pieces Box2D cannot hull left out and counted in
		// droppedPieces), cached on the collider by its points, the scale and the units: the
		// Inspector, the Scene view and MCP show exactly what the simulation uses. In metres
		// (Collider2D::pieces) or, for the editor, in the collider's own units.
		void updateCollider2DPieces(Entity entity, Collider2D& collider);
		// New points of a POLYGON collider that is one Box2D polygon and stays one convex piece:
		// the shape is changed in place (b2Shape_SetPolygon: the same shape id, the body's other
		// contacts untouched) and the mass applied again. False when it must be made again.
		bool reshapeCollider2DPolygon(Entity entity, Rigidbody2DComponent& body, size_t index);
		std::vector<std::vector<Vector2>> getCollider2DPiecesLocal(Entity entity, Collider2D& collider);
		void setPointsToMeterScale2D(float pointsToMeterScale2D);

		bool isSteppingWorld2D() const;
		// Re-applies the GameObject layer to the shapes of the entity's Rigidbody 2D;
		// inside a step, right after it
		void refreshRigidbody2DLayer(Entity entity);

		// Box2D sub-steps of each fixed step (scene setting physics2DSubSteps, 1..16, default 4)
		int getSubSteps2D() const;
		void setSubSteps2D(int subSteps);

		// the static body of joints without bodyB
		b2BodyId getGroundBody2D();

		// Applies the explicit mass, centre of mass and inertia of a body (useAutoMass false...)
		// over what its colliders give; after any Box2D call that recomputes the mass
		void applyRigidbody2DMass(Entity entity, Rigidbody2DComponent& body);
		// Applies the live body fields (gravity scale, damping, fixed rotation, collision
		// detection, sleeping, mass)
		void applyRigidbody2DParameters(Entity entity, Rigidbody2DComponent& body);
		// Removes one collider: its shapes go now, later colliders shift down an index
		bool removeCollider2D(Entity entity, Rigidbody2DComponent& body, size_t index);
		// Applies the motor, spring, limit... fields to the live joint (no rebuild)
		void applyJoint2DParameters(Joint2DComponent& joint);
		// Sets the shapes' friction, restitution and combine modes from the materials again,
		// so touching pairs pick them up on the next step
		void refreshRigidbody2DMaterials(Entity entity, Rigidbody2DComponent& body, bool force);

		uint64_t getShapesCreated2D() const { return shapesCreated2D; }
		uint64_t getChainsCreated2D() const { return chainsCreated2D; }

		// The collider pairs of the 2D per-object callbacks (editor, MCP, tests)
		const ContactTracker& getContactTracker2D() const { return contactTracker2D; }
		// Physics2D.ignoreCollision: no contacts between the two bodies (a Box2D null joint,
		// made again when a body is); runtime only. False if either has no Rigidbody 2D.
		bool setIgnoreCollision2D(Entity a, Entity b, bool ignore);
		bool getIgnoreCollision2D(Entity a, Entity b) const;
		// A collider of the entity was removed: its pairs end at the next step, the indices of
		// the later ones shift down (removeCollider2D)
		void contactColliderRemoved2D(Entity entity, size_t index);
		// what the body touches now (the Inspector's Contacts foldout, get_runtime_entity)
		std::vector<ContactSummary> getContactSummary2D(Entity entity);
#endif

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

		// The collider pairs of the 3D per-object callbacks (editor, MCP, tests)
		const ContactTracker& getContactTracker3D() const { return contactTracker3D; }

		// The global 3D events (onContactAdded3D...) inside the step with a live, modifiable
		// Contact3D, the step on the main thread (JPH::JobSystemSingleThreaded). Off by
		// default: they come after the step with snapshots. On by itself while
		// shouldCollide3D has a Lua subscriber (which must run in the step).
		void setSynchronousContactEvents3D(bool synchronous);
		bool isSynchronousContactEvents3D() const;
		// the current (or last) step runs synchronously
		bool isStepSynchronous3D() const;

		// Physics.ignoreCollision: no contacts between the two bodies (both woken so resting
		// pairs are tested again); runtime only. False if either has no Rigidbody.
		bool setIgnoreCollision3D(Entity a, Entity b, bool ignore);
		bool getIgnoreCollision3D(Entity a, Entity b) const;
		// Rigidbody::setCollisionGroupID: bodies with the same id never collide
		void setCollisionGroup3D(Entity entity, uint32_t group);
		JoltPairFilter3D* getPairFilter3D();

		// what the body touches now (the Inspector's Contacts foldout, get_runtime_entity)
		std::vector<ContactSummary> getContactSummary3D(Entity entity);

		// Seconds summed over the steps since the last reset: Jolt's step, and the contact
		// records' processing after it (callbacks included); the performance tests
		double getStepSeconds3D() const { return stepSeconds3D; }
		double getContactSeconds3D() const { return contactSeconds3D; }
		void resetTimings3D() { stepSeconds3D = 0.0; contactSeconds3D = 0.0; }

		// a contact removed in a synchronous step (the global onContactRemoved3D at once)
		void dispatchContactRemoved3D(const JPH::BodyID& body1, const JPH::BodyID& body2, uint32_t subShape1, uint32_t subShape2);
#endif

		// A setter called on a Contact3D snapshot: one warning per scene
		void warnContactSnapshotSetter();
		// Told by ScriptMessages: a script defining contact messages appeared or went
		static void onScriptReceiversChanged(Scene* scene, Entity entity);
		void markListenerMasksDirty() { listenerMasksDirty = true; }

#ifdef JASPERENGINE_PHYSICS_2D
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> beginContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> endContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> beginSensorContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> endSensorContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)> hitContact2D;
		FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)> preSolve2D;

		FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> shouldCollide2D;

		// once per step per touching shape pair with the authored flag (contactEvents;
		// sensorEvents / sensor), while either body is awake; the arguments of beginContact2D /
		// beginSensorContact2D
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> stayContact2D;
		FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)> staySensorContact2D;
#endif

#ifdef JASPERENGINE_PHYSICS_3D
		FunctionSubscribe<void(Rigidbody)> onBodyActivated3D;
		FunctionSubscribe<void(Rigidbody)> onBodyDeactivated3D;
		FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)> onContactAdded3D;
		FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)> onContactPersisted3D;
		FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)> onContactRemoved3D;

		FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)> shouldCollide3D;

		// C++ only (not in Lua): every added or persisted manifold, inside the step on Jolt's
		// job threads, with a live Contact3D whose setters work (friction for conveyor belts,
		// sensor overrides). Subscribers must be thread-safe and must not touch the scene.
		FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D&)> onContactModify3D;
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
		// destroys the shapes of one collider (the body's mass is not updated)
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
