// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PHYSICSSYSTEM_H
#define PHYSICSSYSTEM_H

#include "SubSystem.h"
#include "ecs/ComponentFwd.h"
#include "component/PhysicsCommon.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Contact2D.h"
#include "object/physics/Manifold2D.h"
#include "box2d/box2d.h"
#endif

#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "object/physics/CollideShapeResult3D.h"
#include "object/physics/Contact3D.h"
#include "object/physics/ControllerColliderHit.h"
#include "Jolt/Jolt.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Physics/Body/MotionQuality.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/Physics/Body/MassProperties.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h"
#include "Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h"
#include "Jolt/Physics/Collision/ObjectLayerPairFilterMask.h"
#endif

#include "registry/TagManager.h"
#include "physics/ContactTracker.h"
#include "object/physics/Collision.h"
#include "object/physics/Collision2D.h"

#include <atomic>
#include <map>
#include <set>
#include <memory>
#include "physics/PhysicsDebugLines.h"

#include <unordered_map>

#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <span>
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
	class WaterSystem;
	struct WaterSample;
	class JoltPairFilter3D;
	class ContactRecorder3D;
	struct RawContact3D;
	struct ContactState2D;
	std::shared_ptr<ContactState2D> makeContactState2D();
	enum class ScriptMessage : uint8_t;
	class Joint3D;
	struct RagdollComponent;
	

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

		friend class CharacterController2DSystem;
		friend class CharacterControllerSystem;
		friend class WaterSystem;

	private:
		Vector2 gravity2D;
		Vector3 gravity3D;

		// the Water Volumes and the Buoyancies floating in them (S22 P3)
		std::unique_ptr<WaterSystem> water;

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
		// Physics2D.ignoreCollision: the body pairs Box2DAux::CollisionFilter refuses, and the
		// bodies to re-pair after a step that locked Box2D (their contacts made again)
		std::set<std::pair<Entity, Entity>> ignoredPairs2D;
		std::vector<Entity> pendingRefilter2D;
#endif
#ifdef JASPERENGINE_PHYSICS_3D
		bool lock3DBodies;
		bool steppingWorld3D = false;
		// the Jolt world is made when the scene first needs it (a body, a query): editor
		// previews, thumbnails and UI scenes never pay for it
		bool world3DReady = false;
		void ensureWorld3D();
		// pair filter changes asked for inside a step (Jolt's job threads read the filter then),
		// made right after it
		std::vector<std::function<void(JoltPairFilter3D&)>> pendingPairFilter3D;
		// the sensor bodies holding the trigger colliders of bodies that also have solid ones
		// (Jolt's continuous collision takes any sensor hit as the closest one): their ids
		std::unordered_set<uint32_t> triggerCompanions3D;

		void flushPendingDestroys3D();
		bool useLockedInterfaces3D() const;

		JoltActivationListener* activationListener3D;
		JoltContactListener* contactListener3D;

        // Process-wide singletons shared by every scene; not owned here.
        JPH::TempAllocator* temp_allocator;
        JPH::JobSystem* job_system;

		JPH::PhysicsSystem world3D;

		// The settings scripts changed on delivered contacts (Contact3D snapshots), applied to
		// those contacts in the steps after it until they end (under callbackMutex3D)
		struct ContactOverride3D {
			bool hasFriction = false;
			float friction = 0.0f;
			bool hasRestitution = false;
			float restitution = 0.0f;
			bool hasSensor = false;
			bool sensor = false;
		};
		std::map<ContactPairKey3D, ContactOverride3D> contactOverrides3D;
		std::atomic<bool> hasContactOverrides3D{false};

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

		// contact callbacks (physics-contacts): the job threads' records and the collider pairs
		// of the per-object callbacks (a synchronous step runs on
		// JoltShapeBuilder::callingThreadPhysicsJobSystem)
		ContactRecorder3D* contactRecorder3D;
		ContactTracker contactTracker3D;
		std::vector<RawContact3D>* rawContacts3D;
		bool synchronousContactEvents3D = false;
		bool stepSynchronous3D = false;
		bool loggedAutoSynchronous3D = false;
		// seconds spent in world3D.Update and in the contact processing after it, summed
		double stepSeconds3D = 0.0;
		double contactSeconds3D = 0.0;
		// the group filter of every body: same collision group ids, joints with enableCollision
		// off, Physics.ignoreCollision and ragdoll self collision (owned: AddRef / Release)
		JoltPairFilter3D* pairFilter3D = nullptr;

		// what the world was made with (setWorldCapacity3D at its creation)
		unsigned int maxBodies3D = 0;
		unsigned int maxBodyPairs3D = 0;
		unsigned int maxContactConstraints3D = 0;
		// EPhysicsUpdateError kinds already reported in this scene, and the body limit
		uint32_t reportedUpdateErrors3D = 0;
		bool reportedBodyLimit3D = false;

		// joints that broke in the last step, told after it (onJointBreak)
		struct JointBreak3D{
			Entity joint;
			Entity bodyA;
			float force;
			bool destroy;
		};
		std::vector<JointBreak3D> pendingJointBreaks3D;
		// bodies whose type a script changed inside a step, switched right after it
		std::vector<Entity> pendingTypeChanges3D;
#endif

		// the last fixed step (extrapolated bodies are drawn ahead by a part of it)
		float lastFixedStep = 1.0f / 60.0f;
		// the write-back's bodies in hierarchy order, and the Transforms written in this pass
		std::vector<Entity> movedBodies2D;
		std::vector<Entity> writeBackScratch;
		std::vector<std::pair<size_t, Entity>> writeOrderScratch;
		std::vector<Entity> ancestorScratch;
		std::unordered_set<Entity> writtenThisPass;

		// the runtime collider and joint lines (Physics.debugDraw): a LineRenderer on a system
		// entity of the scene, made when first needed
		Entity debugLinesEntity = NULL_ENTITY;
		bool debugLinesShown = false;
#ifdef JASPERENGINE_PHYSICS_3D
		std::unordered_map<Entity, std::vector<PhysicsDebugLines::EdgeCache>> debugEdgeCaches3D;
		// the contact points and normals of the last step (Physics.debugDrawContacts), written by
		// the contact listener on Jolt's job threads
		std::mutex debugContactsMutex3D;
		std::vector<std::pair<Vector3, Vector3>> debugContacts3D;
#endif

		// a script that defines a contact message appeared or went: the listener masks of the
		// bodies are computed again before the next step
		bool listenerMasksDirty = true;

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
		// drops the body's contacts and pairs its shapes again (an ignore change), or after the step
		void repairBody2D(Entity entity);

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

		// mass: the mass properties to give Jolt (null: computed by Jolt from the shape)
		bool createGenericJoltBody(Entity entity, RigidbodyComponent& body, const JPH::ShapeRefC shape, const JPH::MassProperties* mass = nullptr);

		// the body's shape from its colliders' leaf shapes (a compound, a rotated / translated
		// leaf or the leaf), with the centre of mass moved when the mass fields or a trigger
		// collider ask for it; mass properties to give Jolt in `mass` (false: Jolt's own)
		JPH::ShapeRefC buildBodyShape3D(Entity entity, RigidbodyComponent& body, JPH::MassProperties& mass, bool& overrideMass);
		// the colliders' leaves at these indices as one shape (a compound, a rotated /
		// translated leaf or the leaf)
		JPH::ShapeRefC composeColliderShape3D(Entity entity, RigidbodyComponent& body, const std::vector<size_t>& indices);
		// builds the dirty (all: every) colliders' leaves; false when one cannot be made yet
		bool buildColliderLeaves3D(Entity entity, RigidbodyComponent& body, bool all, bool& sourceLoading);
		// resolves every collider's surface (material, trigger) into its JoltSurfaceMaterial
		void resolveColliderSurfaces3D(const RigidbodyComponent& body);
		bool usesManifoldReduction3D(const RigidbodyComponent& body) const;
		bool collidesKinematicVsNonDynamic3D(const RigidbodyComponent& body) const;
		// joints whose bodyA or bodyB is the entity: made again once the body is back
		void invalidateJoints3D(Entity body);
		// gears and racks driving the constraint: destroyed, made again later
		void invalidateDependentJoints3D(const JPH::TwoBodyConstraint* constraint);
		// a hinge or slider with a new limits centre, made again from its own frames
		bool recenterJoint3D(Entity entity, Joint3DComponent& joint, float center);
		// the force limits of the damper-only six-DOF drives, before a step
		void updateSixDOFDampers3D();
		// the trigger colliders of a body with solid ones too: made, swapped or removed with it
		void syncTriggerCompanion3D(Entity entity, RigidbodyComponent& body);
		void destroyTriggerCompanion3D(RigidbodyComponent& body);
		// before a step: the companions where their bodies are, moving as they do
		void followTriggerCompanions3D();
		// the ragdolls' bone drivers (entity animations, Pose Animators, Vertex Animators) paused
		// while simulated and resumed after
		void pauseRagdollDrivers3D(Entity entity, RagdollComponent& ragdoll);
		void resumeRagdollDrivers3D(RagdollComponent& ragdoll);
		void flushRagdollPushes3D(Entity entity, RagdollComponent& ragdoll);
		// the reason JOINT of a joint's body pair, on while it lives with enableCollision off
		void releaseJointPair3D(Joint3DComponent& joint);
		// after a step: joints past breakForce / breakTorque, the read-outs; then told
		void checkJointBreaks3D(float dt);
		void dispatchJointBreaks3D();
		// the type changes asked for inside the step
		void applyPendingTypeChanges3D();
		// Jolt's update errors, reported once per kind in this scene
		void reportUpdateErrors3D(JPH::EPhysicsUpdateError errors);
		// the ragdolls' parts, state and self collision, before the bodies are synced
		void updateRagdolls();
		void collectRagdollParts(Entity entity, RagdollComponent& ragdoll);
		void applyRagdollState(Entity entity, RagdollComponent& ragdoll);
		void applyRagdollSelfCollision(RagdollComponent& ragdoll, bool selfCollision);
		// a ragdoll removed: its pairs collide again, its player plays again
		void releaseRagdoll(RagdollComponent& ragdoll);

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
		// The bodies' Transforms in hierarchy order (parents first), each world matrix made again
		// after it is written, so a body under another body (a ragdoll) is placed from its
		// parent's new pose, not the one before the step. interpolating: the drawn pose of the
		// frame (interpolate()), else the pose of the step.
#ifdef JASPERENGINE_PHYSICS_2D
		void writeBackBodies2D(float alpha, bool interpolating);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
		void writeBackBodies3D(float alpha, bool interpolating);
#endif
		void orderByHierarchy(std::vector<Entity>& entities);
		// the Transforms between the entity and the nearest ancestor written in this pass
		void refreshAncestors(Entity entity);
		bool hasWrittenAncestor(Entity entity) const;
		void markWritten(Entity entity);
		// a body's interpolation with SCENE resolved by the scene setting
		RigidbodyInterpolation interpolationOf(RigidbodyInterpolation mode) const{
			if (mode == RigidbodyInterpolation::SCENE){
				return interpolation ? RigidbodyInterpolation::INTERPOLATE : RigidbodyInterpolation::NONE;
			}
			return mode;
		}
		// the runtime collider lines (Physics.debugDraw), off: hidden and nothing computed
		void updateDebugLines();

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

		// every event, for the two clears above (tools/check_event_clears.py)
		template<typename F> void forEachEvent(F&& f) {
#ifdef JASPERENGINE_PHYSICS_2D
			f(beginContact2D); f(endContact2D); f(beginSensorContact2D); f(endSensorContact2D); f(hitContact2D);
			f(preSolve2D); f(shouldCollide2D); f(stayContact2D); f(staySensorContact2D);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
			f(onBodyActivated3D); f(onRagdollBeforeAnimatedRestore3D); f(onBodyDeactivated3D); f(onContactAdded3D);
			f(onContactPersisted3D); f(onContactRemoved3D); f(shouldCollide3D); f(onContactModify3D);
			f(onControllerColliderHit);
#endif
			(void)f;
		}

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

		// A 2D CharacterController's move (points) and simpleMove (points per second), at once
		// (docs/mcp/guide/physics.md): the collision flags, and whether it is grounded
		int moveCharacter2D(Entity entity, const Vector2& motion);
		bool simpleMoveCharacter2D(Entity entity, const Vector2& speed);

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
		// the custom filter's test (inside the step, from Box2D)
		bool isIgnoredPair2D(Entity a, Entity b) const{
			return !ignoredPairs2D.empty() && ignoredPairs2D.count(a < b ? std::make_pair(a, b) : std::make_pair(b, a)) > 0;
		}
		// A collider of the entity was removed: its pairs end at the next step, the indices of
		// the later ones shift down (removeCollider2D)
		void contactColliderRemoved2D(Entity entity, size_t index);
		// what the body touches now (the Inspector's Contacts foldout, get_runtime_entity)
		std::vector<ContactSummary> getContactSummary2D(Entity entity);
#endif

		// What the water is at a world point: in it, the surface over it with the frame's waves,
		// its normal and the water's velocity (Water Volumes first, then library:water meshes)
		WaterSample sampleWater(const Vector3& point) const;
		// the surface height at a world XZ, NaN where no water is
		float sampleWaterHeight(float x, float z) const;

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

        // For the contact listener (JoltPhysicsAux.h), from Jolt's job threads during a step:
        // the settings a script changed on this contact (Contact3D's setters), if any
        void applyContactOverride3D(const ContactPairKey3D& key, JPH::ContactSettings& settings);
        void forgetContact3D(const ContactPairKey3D& key);
        // Contact3D's setters: from the next step, while the contact lasts
        void setContactFriction3D(const ContactPairKey3D& key, float friction);
        void setContactRestitution3D(const ContactPairKey3D& key, float restitution);
        void setContactSensor3D(const ContactPairKey3D& key, bool sensor);

        const JoltObjectLayerTable& getObjectLayerTable3D() const;
        // Re-applies the GameObject layer and the category/mask bits of the entity's
        // Rigidbody to its Jolt body; inside a step, right after it
        void refreshRigidbodyFilter3D(Entity entity);

		static JPH::EMotionQuality getRigidbodyMotionQualityToJolt(RigidbodyMotionQuality motionQuality);

		// The collider pairs of the 3D per-object callbacks (editor, MCP, tests)
		const ContactTracker& getContactTracker3D() const { return contactTracker3D; }

		// The global 3D events (onContactAdded3D...) inside the step with a live, modifiable
		// Contact3D, the step on the main thread (JPH::JobSystemSingleThreaded). Off by
		// default: they come after the step with snapshots, whose setters apply from the next
		// step. On by itself while shouldCollide3D has a Lua subscriber (which must run in the
		// step); any shouldCollide3D subscriber steps on the main thread, events still after.
		void setSynchronousContactEvents3D(bool synchronous);
		bool isSynchronousContactEvents3D() const;
		// the current (or last) step runs synchronously
		bool isStepSynchronous3D() const;

		// Physics.ignoreCollision: no contacts between the two bodies (both woken so resting
		// pairs are tested again); runtime only. False if either has no Rigidbody.
		bool setIgnoreCollision3D(Entity a, Entity b, bool ignore);
		bool getIgnoreCollision3D(Entity a, Entity b) const;

		// what the body touches now (the Inspector's Contacts foldout, get_runtime_entity)
		std::vector<ContactSummary> getContactSummary3D(Entity entity);
		// a collider of the entity was removed (Rigidbody.removeCollider / removeAllColliders)
		void contactColliderRemoved3D(Entity entity, size_t index) { contactTracker3D.removeCollider(entity, (int)index); }

		// Seconds summed over the steps since the last reset: Jolt's step, and the contact
		// records' processing after it (callbacks included); the performance tests
		double getStepSeconds3D() const { return stepSeconds3D; }
		double getContactSeconds3D() const { return contactSeconds3D; }
		void resetTimings3D() { stepSeconds3D = 0.0; contactSeconds3D = 0.0; }

		// a contact removed in a synchronous step (the global onContactRemoved3D at once)
		void dispatchContactRemoved3D(const JPH::BodyID& body1, const JPH::BodyID& body2, uint32_t subShape1, uint32_t subShape2);
		// the contact listener's points for Physics.debugDrawContacts (any thread)
		void recordDebugContact3D(const JPH::ContactManifold& manifold);

		// The size of the 3D worlds made from now on (project setting physics3DWorld): bodies, body
		// pairs and contact constraints. Defaults 8192 / 8192 / 8192 (a world takes 3.6 MB with them, Physics3DBodyTest). A world past one of them
		// reports it once with the setting to raise.
		static void setWorldCapacity3D(unsigned int maxBodies, unsigned int maxBodyPairs, unsigned int maxContactConstraints);
		static unsigned int getDefaultMaxBodies3D();
		static unsigned int getDefaultMaxBodyPairs3D();
		static unsigned int getDefaultMaxContactConstraints3D();
		unsigned int getMaxBodies3D() const { return maxBodies3D; }
		unsigned int getMaxBodyPairs3D() const { return maxBodyPairs3D; }
		unsigned int getMaxContactConstraints3D() const { return maxContactConstraints3D; }

		JoltPairFilter3D& getPairFilter3D();
		// Turns the collision of two bodies off or on for a reason (JoltPairFilter3D::SCRIPT,
		// RAGDOLL) and wakes both, so resting pairs are tested again
		void setBodyPairIgnored3D(Entity a, Entity b, uint32_t reason, bool ignore);

		// Switches a live body between static, kinematic and dynamic (Rigidbody::setType); false
		// and an error naming the collider when a static-only shape (mesh, heightfield) is asked
		// to move. Inside a step the switch waits until the step ends.
		bool setRigidbodyType(Entity entity, RigidbodyComponent& body, BodyType type);
		// Applies the live body fields (damping, max velocities, gravity factor, sleeping, motion
		// quality, contacts with static and kinematic bodies)
		void applyRigidbodyParameters(Entity entity, RigidbodyComponent& body);
		// The shape and mass properties of a live body again from its colliders (only the dirty
		// ones are made again): the body keeps its id, velocities, joints and other contacts.
		// Without colliders it leaves the world until one is added.
		bool reshapeRigidbody3D(Entity entity, RigidbodyComponent& body);
		// Removes one collider: later colliders shift down an index
		bool removeCollider3D(Entity entity, RigidbodyComponent& body, size_t index);
		// Sets the colliders' surfaces from their materials again (a material changed)
		void refreshRigidbodyMaterials3D(Entity entity, RigidbodyComponent& body, bool force);
		// Re-applies the materials of bodies using one that changed (PhysicsMaterialRuntime::revision)
		void syncMaterials3D();

		// The mass, centre of mass (body-local) and inertia (about it, body axes) the body has or
		// would have, from its colliders and mass fields, without a live body (the Inspector's
		// "automatic" values). False when no collider has mass.
		struct MassInfo3D{
			float mass = 0.0f;
			Vector3 centerOfMass = Vector3::ZERO;
			Vector3 inertiaDiagonal = Vector3::ZERO;
		};
		bool computeMassProperties3D(Entity entity, const RigidbodyComponent& body, MassInfo3D& info);
		// the same with the full inertia tensor (about the centre of mass, body axes)
		bool computeMass3D(Entity entity, const RigidbodyComponent& body, MassInfo3D& info, JPH::Mat44& inertia);

		// Makes the Jolt constraint from every field of the component (anchor space, limits,
		// motors, springs...)
		bool buildJoint3D(Entity entity, Joint3DComponent& joint);
		// Applies the motor, spring, limit... fields to the live joint (no rebuild)
		void applyJoint3DParameters(Entity entity, Joint3DComponent& joint);
		// HINGE: the angle (radians, from the start pose); PRISMATIC: the position (metres)
		float getJoint3DPosition(const Joint3DComponent& joint);
		// HINGE: rad/s; PRISMATIC: m/s (relative velocity of the bodies along / about the axis)
		float getJoint3DSpeed(const Joint3DComponent& joint);
		// DISTANCE: the anchors' distance (metres)
		float getJoint3DLength(const Joint3DComponent& joint);

		// Collects a ragdoll's parts when they may have changed
		void refreshRagdoll(Entity entity);
		// Applies a ragdoll's simulated state now (outside a step; inside one it waits for the
		// next fixed step), then the impulses and forces given while it was not applied
		void updateRagdoll(Entity entity);
		// Ragdoll::applyImpulse / applyForce: now when the state they need is applied, else
		// queued until it is (a script setting simulated = true and pushing in the same frame)
		void applyRagdollImpulse3D(Entity entity, const Vector3& impulse, const Vector3& point);
		void applyRagdollForce3D(Entity entity, const Vector3& force);
		// a ragdoll is about to go back to animated (its entity): Ragdoll::onBeforeAnimatedRestore
		FunctionSubscribe<void(int)> onRagdollBeforeAnimatedRestore3D;

		// Changes the pair filter now, or right after the step when inside one
		void changePairFilter3D(const std::function<void(JoltPairFilter3D&)>& change);
		bool isTriggerCompanion3D(const JPH::BodyID& body) const;
		// the 3D world exists (made by the first body or query)
		bool hasWorld3D() const { return world3DReady; }
#endif

		// Physics.debugDraw: the runtime collider lines of this scene (3D and 2D), drawn while on
		size_t getDebugLineCount() const;
		Entity getDebugLinesEntity() const { return debugLinesEntity; }

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
		// sensor overrides). Called on several threads at once, without a lock and without the
		// crash guard: subscribers must be thread-safe, must not call the engine, the scene or
		// Lua (read the two Rigidbody handles' entities and the contact only), must not throw,
		// and must be added / removed outside the step.
		FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D&)> onContactModify3D;

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
		// T6 V8: a static body of an already built shape for this entity's Rigidbody, not yet in the
		// broad phase. The two after it follow Jolt's batch contract (BodyInterface.h): prepare is
		// thread-safe, finalize runs between steps on the main thread.
		JPH::BodyID createStaticBodyForShape(Entity entity, const JPH::Shape* shape);
		JPH::BodyInterface::AddState prepareBodies(std::span<JPH::BodyID> ids);
		void finalizeBodies(std::span<JPH::BodyID> ids, JPH::BodyInterface::AddState state);
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
