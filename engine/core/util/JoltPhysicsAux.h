// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JoltPhysicsAux_h
#define JoltPhysicsAux_h

#include "subsystem/AudioSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "object/physics/Rigidbody.h"
#include "object/physics/CollideShapeResult3D.h"
#include "object/physics/Contact3D.h"

#include "Jolt/Jolt.h"

#include "Jolt/RegisterTypes.h"
#include "Jolt/Core/Factory.h"
#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Physics/PhysicsSettings.h"
#include "Jolt/Physics/PhysicsSystem.h"

#include "Jolt/Physics/Body/BodyLockMulti.h"

#include "Jolt/Physics/Collision/RayCast.h"
#include "Jolt/Physics/Collision/CastResult.h"

#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/CylinderShape.h"
#include "Jolt/Physics/Collision/Shape/ConvexHullShape.h"
#include "Jolt/Physics/Collision/Shape/MeshShape.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#include "Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h"

#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h"
#include "Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h"
#include "Jolt/Physics/Collision/ObjectLayerPairFilterMask.h"
#include "Jolt/Physics/Collision/EstimateCollisionResponse.h"
#include "Jolt/Physics/Collision/GroupFilter.h"

#include "physics/ContactTracker.h"
#include "script/LuaBinding.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include "audio/AudioMarkers.h"

#include <unordered_map>
#include <vector>

#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Body/BodyActivationListener.h"
#include "Jolt/Physics/Collision/GroupFilter.h"
#include "Jolt/Physics/Collision/GroupFilterTable.h"
#include "Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h"

#include "Jolt/Physics/Constraints/FixedConstraint.h"
#include "Jolt/Physics/Constraints/DistanceConstraint.h"
#include "Jolt/Physics/Constraints/PointConstraint.h"
#include "Jolt/Physics/Constraints/HingeConstraint.h"
#include "Jolt/Physics/Constraints/ConeConstraint.h"
#include "Jolt/Physics/Constraints/SliderConstraint.h"
#include "Jolt/Physics/Constraints/SwingTwistConstraint.h"
#include "Jolt/Physics/Constraints/SixDOFConstraint.h"
#include "Jolt/Physics/Constraints/PathConstraint.h"
#include "Jolt/Physics/Constraints/PathConstraintPathHermite.h"
#include "Jolt/Physics/Constraints/GearConstraint.h"
#include "Jolt/Physics/Constraints/RackAndPinionConstraint.h"
#include "Jolt/Physics/Constraints/PulleyConstraint.h"

#include "Jolt/Physics/Collision/CollisionCollectorImpl.h"
#include "Jolt/Physics/Collision/TransformedShape.h"

#include "physics/JoltSurfaceMaterial.h"
#include "registry/PhysicsLayers.h"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine{

	// A body's Jolt ObjectLayer indexes this table, whose entry holds its GameObject
	// layer and its category (group) and mask bits: 5 + 16 + 16 bits do not fit Jolt's
	// 32-bit ObjectLayer. Bodies with the same three share an entry, freed with the last.
	// Only the main thread changes it, between world steps (layer and filter changes made
	// during a step are applied after it); the filters read it from Jolt's job threads.
	class JoltObjectLayerTable{
	public:
		struct Entry{
			uint32_t group = 0xFFFF;
			uint32_t mask = 0xFFFF;
			uint32_t layer = 0;
			uint32_t refs = 0;
		};

	private:
		std::vector<Entry> entries;
		std::vector<JPH::ObjectLayer> freeEntries;
		std::unordered_map<uint64_t, JPH::ObjectLayer> lookup;
		Entry fallback;

		static uint64_t key(int layer, uint16_t group, uint16_t mask){
			return ((uint64_t)(layer & 31) << 32) | ((uint64_t)group << 16) | (uint64_t)mask;
		}

	public:
		JoltObjectLayerTable(){
			entries.reserve(64);
		}

		JPH::ObjectLayer acquire(int layer, uint16_t group, uint16_t mask){
			const uint64_t entryKey = key(layer, group, mask);
			auto found = lookup.find(entryKey);
			if (found != lookup.end()){
				entries[found->second].refs++;
				return found->second;
			}

			JPH::ObjectLayer objectLayer;
			if (!freeEntries.empty()){
				objectLayer = freeEntries.back();
				freeEntries.pop_back();
			}else{
				objectLayer = (JPH::ObjectLayer)entries.size();
				entries.emplace_back();
			}
			Entry& entry = entries[objectLayer];
			entry.group = group;
			entry.mask = mask;
			entry.layer = (uint32_t)(layer & 31);
			entry.refs = 1;
			lookup.emplace(entryKey, objectLayer);
			return objectLayer;
		}

		void release(JPH::ObjectLayer objectLayer){
			if (objectLayer >= entries.size()){
				return;
			}
			Entry& entry = entries[objectLayer];
			if (entry.refs == 0){
				return;
			}
			if (--entry.refs == 0){
				lookup.erase(key((int)entry.layer, (uint16_t)entry.group, (uint16_t)entry.mask));
				freeEntries.push_back(objectLayer);
			}
		}

		inline const Entry& get(JPH::ObjectLayer objectLayer) const{
			return objectLayer < entries.size() ? entries[objectLayer] : fallback;
		}

		// Jolt's own group/mask encoding, which its mask broad phase classes read
		inline JPH::ObjectLayer getGroupMaskLayer(JPH::ObjectLayer objectLayer) const{
			const Entry& entry = get(objectLayer);
			return JPH::ObjectLayerPairFilterMask::sGetObjectLayer(entry.group, entry.mask);
		}
	};

	// Body vs body: the Layer Collision Matrix first, then the category and mask bits
	class JoltObjectLayerPairFilter : public JPH::ObjectLayerPairFilter{
	private:
		const JoltObjectLayerTable& table;
		const LayerCollisionMatrix& matrix;

	public:
		JoltObjectLayerPairFilter(const JoltObjectLayerTable& table, const LayerCollisionMatrix& matrix): table(table), matrix(matrix){ }

		virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override{
			const JoltObjectLayerTable::Entry& entry1 = table.get(inObject1);
			const JoltObjectLayerTable::Entry& entry2 = table.get(inObject2);
			return matrix.collides(entry1.layer, entry2.layer)
				&& (entry1.group & entry2.mask) != 0
				&& (entry2.group & entry1.mask) != 0;
		}
	};

	// Broad phase layers keep being chosen by category bits (PhysicsSystem::addBroadPhaseLayer3D)
	class JoltBroadPhaseLayers : public JPH::BroadPhaseLayerInterface{
	private:
		JPH::BroadPhaseLayerInterfaceMask byGroup;
		const JoltObjectLayerTable& table;

	public:
		JoltBroadPhaseLayers(unsigned int numBroadPhaseLayers, const JoltObjectLayerTable& table): byGroup(numBroadPhaseLayers), table(table){ }

		void ConfigureLayer(JPH::BroadPhaseLayer broadPhaseLayer, uint32_t groupsToInclude, uint32_t groupsToExclude){
			byGroup.ConfigureLayer(broadPhaseLayer, groupsToInclude, groupsToExclude);
		}

		virtual JPH::uint GetNumBroadPhaseLayers() const override{
			return byGroup.GetNumBroadPhaseLayers();
		}

		virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override{
			return byGroup.GetBroadPhaseLayer(table.getGroupMaskLayer(inLayer));
		}

		inline bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const{
			return byGroup.ShouldCollide(table.getGroupMaskLayer(inLayer1), inLayer2);
		}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override{
			return byGroup.GetBroadPhaseLayerName(inLayer);
		}
#endif
	};

	class JoltObjectVsBroadPhaseLayerFilter : public JPH::ObjectVsBroadPhaseLayerFilter{
	private:
		const JoltBroadPhaseLayers& layers;

	public:
		explicit JoltObjectVsBroadPhaseLayerFilter(const JoltBroadPhaseLayers& layers): layers(layers){ }

		virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override{
			return layers.ShouldCollide(inLayer1, inLayer2);
		}
	};

	// Query vs body, tested per body in Jolt's broad phase walk: the GameObject layer
	// must be in layerMask, and the query and body category/mask bits must meet both ways
	class JoltQueryObjectLayerFilter : public JPH::ObjectLayerFilter{
	private:
		const JoltObjectLayerTable& table;
		uint32_t layerMask;
		uint32_t categoryBits;
		uint32_t maskBits;

	public:
		JoltQueryObjectLayerFilter(const JoltObjectLayerTable& table, uint32_t layerMask, uint16_t categoryBits, uint16_t maskBits):
			table(table), layerMask(layerMask), categoryBits(categoryBits), maskBits(maskBits){ }

		virtual bool ShouldCollide(JPH::ObjectLayer inLayer) const override{
			const JoltObjectLayerTable::Entry& entry = table.get(inLayer);
			return ((layerMask >> entry.layer) & 1u) != 0
				&& (categoryBits & entry.mask) != 0
				&& (entry.group & maskBits) != 0;
		}
	};

	// Body pairs that must not collide (physics-3d 6.1.6), the group filter of every body of a
	// scene's world: CollisionGroup(filter, group id, sub group id = the body's entity).
	// - Two bodies with the same valid group id never collide (Rigidbody::setCollisionGroupID).
	// - An entity pair in `ignored` never collides; its value holds the reasons: the count of
	//   joints between the pair with enableCollision off (low 16 bits), SCRIPT
	//   (Physics.ignoreCollision, physics-contacts) and RAGDOLL (a ragdoll's selfCollision off).
	// - Changed only on the main thread between steps; Jolt's job threads read it in a step.
	class JoltPairFilter3D : public JPH::GroupFilter{
	public:
		static constexpr uint32_t JOINT_COUNT_MASK = 0xFFFFu;
		static constexpr uint32_t SCRIPT = 1u << 16;
		static constexpr uint32_t RAGDOLL = 1u << 17;

	private:
		std::unordered_map<uint64_t, uint32_t> ignored;

		static uint64_t pairKey(Entity a, Entity b){
			if (a > b) std::swap(a, b);
			return ((uint64_t)a << 32) | (uint64_t)b;
		}

	public:
		virtual bool CanCollide(const JPH::CollisionGroup& inGroup1, const JPH::CollisionGroup& inGroup2) const override{
			const JPH::CollisionGroup::GroupID group1 = inGroup1.GetGroupID();
			if (group1 != JPH::CollisionGroup::cInvalidGroup && group1 == inGroup2.GetGroupID()){
				return false;
			}
			// a body and the sensor body of its own trigger colliders (same GameObject)
			if (inGroup1.GetSubGroupID() == inGroup2.GetSubGroupID()){
				return false;
			}
			if (ignored.empty()){
				return true;
			}
			return ignored.find(pairKey((Entity)inGroup1.GetSubGroupID(), (Entity)inGroup2.GetSubGroupID())) == ignored.end();
		}

		// a joint between the pair with enableCollision off made (add) or gone (remove)
		void addJoint(Entity a, Entity b){
			ignored[pairKey(a, b)]++;
		}
		void removeJoint(Entity a, Entity b){
			auto found = ignored.find(pairKey(a, b));
			if (found == ignored.end() || (found->second & JOINT_COUNT_MASK) == 0){
				return;
			}
			if (--found->second == 0){
				ignored.erase(found);
			}
		}
		// SCRIPT or RAGDOLL on or off for the pair
		void setReason(Entity a, Entity b, uint32_t reason, bool on){
			const uint64_t key = pairKey(a, b);
			auto found = ignored.find(key);
			if (on){
				ignored[key] |= reason;
			}else if (found != ignored.end()){
				found->second &= ~reason;
				if (found->second == 0){
					ignored.erase(found);
				}
			}
		}
		uint32_t getReasons(Entity a, Entity b) const{
			auto found = ignored.find(pairKey(a, b));
			return found == ignored.end() ? 0u : found->second;
		}
		bool isIgnored(Entity a, Entity b) const{
			return getReasons(a, b) != 0;
		}
		// every pair with one of these entities (a body gone for good)
		void forgetEntity(Entity entity){
			for (auto it = ignored.begin(); it != ignored.end();){
				if ((Entity)(it->first >> 32) == entity || (Entity)(it->first & 0xFFFFFFFFu) == entity){
					it = ignored.erase(it);
				}else{
					++it;
				}
			}
		}
		void clear(){
			ignored.clear();
		}
	};

	// The surface of one side of a contact: its collider's material when it has one, else the
	// body's legacy friction / restitution (no mode)
	inline void contactSurface3D(const JPH::Body& body, const JPH::SubShapeID& subShape, bool friction, float& value, PhysicsMaterialCombine& mode){
		const JoltSurfaceMaterial* surface = JoltSurfaceMaterial::of(body.GetShape()->GetMaterial(subShape));
		if (surface && surface->hasMaterial()){
			value = friction ? surface->friction : surface->restitution;
			mode = friction ? surface->frictionCombine : surface->bounceCombine;
		}else{
			value = friction ? body.GetFriction() : body.GetRestitution();
			mode = PhysicsMaterialCombine::NONE;
		}
	}

	// Jolt's combine functions (ContactConstraintManager::CombineFunction), called from its job
	// threads for every contact, cached ones included: two colliders without a material combine
	// as Jolt always did (geometric mean friction, maximum restitution)
	inline float combineFriction3D(const JPH::Body& body1, const JPH::SubShapeID& subShape1, const JPH::Body& body2, const JPH::SubShapeID& subShape2){
		float a, b;
		PhysicsMaterialCombine modeA, modeB;
		contactSurface3D(body1, subShape1, true, a, modeA);
		contactSurface3D(body2, subShape2, true, b, modeB);
		return combineSurface(a, modeA, b, modeB, PhysicsMaterialCombine::GEOMETRIC_MEAN);
	}

	inline float combineRestitution3D(const JPH::Body& body1, const JPH::SubShapeID& subShape1, const JPH::Body& body2, const JPH::SubShapeID& subShape2){
		float a, b;
		PhysicsMaterialCombine modeA, modeB;
		contactSurface3D(body1, subShape1, false, a, modeA);
		contactSurface3D(body2, subShape2, false, b, modeB);
		return combineSurface(a, modeA, b, modeB, PhysicsMaterialCombine::MAXIMUM);
	}

	// A contact with a trigger collider on either side is a sensor contact (Unity isTrigger)
	inline bool isTriggerContact3D(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold){
		const JoltSurfaceMaterial* surface1 = JoltSurfaceMaterial::of(body1.GetShape()->GetMaterial(manifold.mSubShapeID1));
		if (surface1 && surface1->isTrigger){
			return true;
		}
		const JoltSurfaceMaterial* surface2 = JoltSurfaceMaterial::of(body2.GetShape()->GetMaterial(manifold.mSubShapeID2));
		return surface2 && surface2->isTrigger;
	}

	class OnlyStaticBodyFilter : public JPH::BodyFilter{
	private:
		bool onlyStatic;
		const std::vector<Entity>* ignoreEntities = nullptr;

		bool isIgnored(Entity entity) const{
			if (!ignoreEntities){
				return false;
			}
			for (Entity ignored : *ignoreEntities){
				if (ignored != NULL_ENTITY && entity == ignored){
					return true;
				}
			}
			return false;
		}

	public:
		explicit OnlyStaticBodyFilter(const bool onlyStatic, const std::vector<Entity>* ignoreEntities = nullptr){
			this->onlyStatic = onlyStatic;
			this->ignoreEntities = ignoreEntities;
		}

		virtual bool ShouldCollideLocked(const JPH::Body &inBody) const override{
			if (onlyStatic && inBody.GetMotionType() != JPH::EMotionType::Static){
				return false;
			}
			if (isIgnored(PhysicsCallbacks::bodyEntity(inBody.GetUserData()))){
				return false;
			}
			return true;
		}
	};

	// A contact manifold as Jolt reported it in a step, copied on the job thread for the
	// main thread (physics-contacts spec 6.1.4): no Jolt pointers, the bodies by id and entity
	struct RawContact3D{
		enum Kind : uint8_t{ Added = 0, Persisted = 1, Removed = 2 };
		Kind kind = Added;
		JPH::BodyID body1, body2;
		uint32_t subShape1 = 0, subShape2 = 0;
		Entity entity1 = NULL_ENTITY, entity2 = NULL_ENTITY;
		int collider1 = 0, collider2 = 0;
		uint16_t mask1 = 0, mask2 = 0;
		bool sensor = false;
		bool hasImpulses = false;
		uint8_t numPoints1 = 0, numPoints2 = 0;
		// world space; the points relative to baseOffset (Jolt's manifold)
		Vector3 baseOffset;
		Vector3 normal;          // from body 1 toward body 2 (moves body 2 out)
		float penetration = 0.0f;
		Vector3 pointsOn1[4];
		Vector3 pointsOn2[4];
		// point velocities at the manifold centre (pre-solve)
		Vector3 velocity1, velocity2;
		// EstimateCollisionResponse, on body 2 along the normal and the tangents
		float contactImpulses[4] = {0, 0, 0, 0};
		float frictionImpulses1[4] = {0, 0, 0, 0};
		float frictionImpulses2[4] = {0, 0, 0, 0};
		Vector3 tangent1, tangent2;
		float friction = 0.0f;
		float restitution = 0.0f;

		bool operator<(const RawContact3D& other) const{
			if (kind != other.kind) return kind < other.kind;
			if (body1 != other.body1) return body1 < other.body1;
			if (body2 != other.body2) return body2 < other.body2;
			if (subShape1 != other.subShape1) return subShape1 < other.subShape1;
			return subShape2 < other.subShape2;
		}

		std::shared_ptr<const Contact3DSnapshot> snapshot() const{
			auto copy = std::make_shared<Contact3DSnapshot>();
			// the contact's key: a setter on the snapshot applies to it from the next step
			copy->key = ContactPairKey3D{body1.GetIndexAndSequenceNumber(), body2.GetIndexAndSequenceNumber(), subShape1, subShape2};
			copy->baseOffset = baseOffset;
			copy->worldSpaceNormal = normal;
			copy->penetrationDepth = penetration;
			copy->colliderIndex1 = (size_t)collider1;
			copy->colliderIndex2 = (size_t)collider2;
			for (uint8_t i = 0; i < numPoints1; i++) copy->pointsOn1.push_back(pointsOn1[i]);
			for (uint8_t i = 0; i < numPoints2; i++) copy->pointsOn2.push_back(pointsOn2[i]);
			copy->combinedFriction = friction;
			copy->combinedRestitution = restitution;
			copy->sensor = sensor;
			copy->approachSpeed = std::max(0.0f, (velocity1 - velocity2).dotProduct(normal));
			return copy;
		}
	};

	struct RawActivation3D{
		JPH::BodyID body;
		Entity entity = NULL_ENTITY;
		bool activated = false;
	};

	// The job threads' records of one step: one buffer per thread (a thread-local slot), no
	// lock and no allocation in the steady state; threads past the slots share a locked one
	class ContactRecorder3D{
	public:
		static constexpr int SLOTS = 64;

		void beginStep(){
			for (std::vector<RawContact3D>& slot : slots){
				slot.clear();
			}
			overflow.clear();
		}

		void push(const RawContact3D& record){
			const int slot = threadSlot();
			if (slot < SLOTS){
				slots[slot].push_back(record);
			}else{
				std::lock_guard<std::mutex> lock(overflowMutex);
				overflow.push_back(record);
			}
		}

		// main thread, after the step: every record, in the deterministic order of the spec
		// (kind, bodies, sub-shapes), whatever thread made it
		void collect(std::vector<RawContact3D>& out){
			out.clear();
			for (const std::vector<RawContact3D>& slot : slots){
				out.insert(out.end(), slot.begin(), slot.end());
			}
			out.insert(out.end(), overflow.begin(), overflow.end());
			std::sort(out.begin(), out.end());
		}

		void pushActivation(const RawActivation3D& activation){
			std::lock_guard<std::mutex> lock(activationMutex);
			activations.push_back(activation);
		}

		void takeActivations(std::vector<RawActivation3D>& out){
			std::lock_guard<std::mutex> lock(activationMutex);
			out.swap(activations);
			activations.clear();
		}

	private:
		static int threadSlot(){
			static std::atomic<int> nextSlot{0};
			thread_local int slot = nextSlot.fetch_add(1, std::memory_order_relaxed);
			return slot;
		}

		std::vector<RawContact3D> slots[SLOTS];
		std::vector<RawContact3D> overflow;
		std::mutex overflowMutex;
		std::vector<RawActivation3D> activations;
		std::mutex activationMutex;
	};

	inline Vector3 toVector3(JPH::Vec3Arg v){
		return Vector3(v.GetX(), v.GetY(), v.GetZ());
	}

	inline Vector3 toVector3(const JPH::DVec3& v){
		return Vector3((float)v.GetX(), (float)v.GetY(), (float)v.GetZ());
	}

	// Jolt calls these from its job threads (or the main thread, waking a body from a
	// BodyInterface call): recorded for the dispatch after the step; with
	// PhysicsSystem::isStepSynchronous3D the step runs on the main thread and the global
	// events are called at once
	class JoltActivationListener : public JPH::BodyActivationListener{
	private:
        Scene* scene;
        PhysicsSystem* physicsSystem;
        ContactRecorder3D* recorder;

        void report(const JPH::BodyID& inBodyID, uint64_t inBodyUserData, bool activated){
            FunctionSubscribe<void(Rigidbody)>& event = activated ? physicsSystem->onBodyActivated3D : physicsSystem->onBodyDeactivated3D;
            // a trigger collider's sensor body is never reported as a body of its own
            // nor a CharacterController's inner body (it has no Rigidbody)
            if (!event.hasSubscribers() || physicsSystem->isTriggerCompanion3D(inBodyID) || physicsSystem->isCharacterInnerBody(inBodyID)){
                return;
            }
            const Entity entity = PhysicsCallbacks::bodyEntity(inBodyUserData);
            if (physicsSystem->isStepSynchronous3D() && physicsSystem->isSteppingWorld3D() && LuaBinding::isLuaThread()){
                std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());
                event(Rigidbody(scene, entity));
                return;
            }
            recorder->pushActivation({inBodyID, entity, activated});
        }

	public:
        JoltActivationListener(Scene* scene, PhysicsSystem* physicsSystem, ContactRecorder3D* recorder){
            this->scene = scene;
            this->physicsSystem = physicsSystem;
            this->recorder = recorder;
        }

		virtual void OnBodyActivated(const JPH::BodyID &inBodyID, uint64_t inBodyUserData) override{
			report(inBodyID, inBodyUserData, true);
		}

		virtual void OnBodyDeactivated(const JPH::BodyID &inBodyID, uint64_t inBodyUserData) override{
			report(inBodyID, inBodyUserData, false);
		}
	};

	// Contacts (physics-contacts spec 6.1.4). On the job threads: the synchronous steps (the
	// C++ onContactModify3D, and in synchronous mode the global events with live contacts),
	// then a record for the main thread when a body listens (user data mask) or a global event
	// has subscribers. No Lua runs here except in synchronous mode, whose step runs on the
	// main thread (JobSystemSingleThreaded).
	class JoltContactListener : public JPH::ContactListener{
    private:
        Scene* scene;
        PhysicsSystem* physicsSystem;

        // A CharacterController's inner body has no RigidbodyComponent: its contacts with bodies
        // are not reported (scripts would get a Rigidbody with nothing behind it), a trigger's are
        // (its Rigidbody for the controller side is not isValid())
        bool hidden(const JPH::Body& body1, const JPH::Body& body2) const{
            const bool inner = physicsSystem->isCharacterInnerBody(body1.GetID()) || physicsSystem->isCharacterInnerBody(body2.GetID());
            return inner && !body1.IsSensor() && !body2.IsSensor();
        }

        ContactRecorder3D* recorder;

        void record(RawContact3D::Kind kind, const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, const JPH::ContactSettings &settings, bool global){
            const uint64_t userData1 = inBody1.GetUserData();
            const uint64_t userData2 = inBody2.GetUserData();
            const uint16_t mask1 = PhysicsCallbacks::bodyMask(userData1);
            const uint16_t mask2 = PhysicsCallbacks::bodyMask(userData2);
            // a trigger pair for trigger listeners, a solid one for collision listeners
            const uint16_t wanted = settings.mIsSensor ? PhysicsCallbacks::TRIGGER_3D : PhysicsCallbacks::COLLISION_3D;
            const bool listened = ((mask1 | mask2) & wanted) != 0;
#if JASPERENGINE_AUDIO_CAPTURE
            // a capture marks every contact, with or without a listener
            if (!listened && !global && !AudioMarkers::isEnabled()){
                return;
            }
#else
            if (!listened && !global){
                return;
            }
#endif

            RawContact3D raw;
            raw.kind = kind;
            raw.body1 = inBody1.GetID();
            raw.body2 = inBody2.GetID();
            raw.subShape1 = inManifold.mSubShapeID1.GetValue();
            raw.subShape2 = inManifold.mSubShapeID2.GetValue();
            raw.entity1 = PhysicsCallbacks::bodyEntity(userData1);
            raw.entity2 = PhysicsCallbacks::bodyEntity(userData2);
            raw.collider1 = (int)inBody1.GetShape()->GetSubShapeUserData(inManifold.mSubShapeID1);
            raw.collider2 = (int)inBody2.GetShape()->GetSubShapeUserData(inManifold.mSubShapeID2);
            raw.mask1 = mask1;
            raw.mask2 = mask2;
            raw.sensor = settings.mIsSensor;
            raw.baseOffset = toVector3(inManifold.mBaseOffset);
            raw.normal = toVector3(inManifold.mWorldSpaceNormal);
            raw.penetration = inManifold.mPenetrationDepth;
            raw.friction = settings.mCombinedFriction;
            raw.restitution = settings.mCombinedRestitution;
            raw.numPoints1 = (uint8_t)std::min<size_t>(4, inManifold.mRelativeContactPointsOn1.size());
            raw.numPoints2 = (uint8_t)std::min<size_t>(4, inManifold.mRelativeContactPointsOn2.size());
            JPH::Vec3 centre = JPH::Vec3::sZero();
            for (uint8_t i = 0; i < raw.numPoints1; i++){
                raw.pointsOn1[i] = toVector3(inManifold.mRelativeContactPointsOn1[i]);
                centre += inManifold.mRelativeContactPointsOn1[i];
            }
            for (uint8_t i = 0; i < raw.numPoints2; i++){
                raw.pointsOn2[i] = toVector3(inManifold.mRelativeContactPointsOn2[i]);
                centre += inManifold.mRelativeContactPointsOn2[i];
            }
            const size_t points = (size_t)raw.numPoints1 + raw.numPoints2;
            if (points > 0){
                centre /= (float)points;
            }
            const JPH::RVec3 worldCentre = inManifold.mBaseOffset + centre;
            raw.velocity1 = toVector3(inBody1.GetPointVelocity(worldCentre));
            raw.velocity2 = toVector3(inBody2.GetPointVelocity(worldCentre));

            // impulses only for collision listeners (Jolt has no solver impulses for listeners)
            if (listened && !settings.mIsSensor && raw.numPoints1 > 0){
                JPH::CollisionEstimationResult estimate;
                JPH::EstimateCollisionResponse(inBody1, inBody2, inManifold, estimate, settings.mCombinedFriction, settings.mCombinedRestitution);
                const size_t count = std::min<size_t>(4, estimate.mImpulses.size());
                for (size_t i = 0; i < count; i++){
                    raw.contactImpulses[i] = estimate.mImpulses[i].mContactImpulse;
                    raw.frictionImpulses1[i] = estimate.mImpulses[i].mFrictionImpulse1;
                    raw.frictionImpulses2[i] = estimate.mImpulses[i].mFrictionImpulse2;
                }
                raw.tangent1 = toVector3(estimate.mTangent1);
                raw.tangent2 = toVector3(estimate.mTangent2);
                raw.hasImpulses = true;
            }

            recorder->push(raw);
        }

        void contact(RawContact3D::Kind kind, const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings){
            // per-collider triggers first (Jolt sets the settings up again for every call)
            if (isTriggerContact3D(inBody1, inBody2, inManifold)){
                ioSettings.mIsSensor = true;
            }
            // then what a script set on a delivered Contact3D of this contact
            const ContactPairKey3D key{inBody1.GetID().GetIndexAndSequenceNumber(), inBody2.GetID().GetIndexAndSequenceNumber(),
                                       inManifold.mSubShapeID1.GetValue(), inManifold.mSubShapeID2.GetValue()};
            physicsSystem->applyContactOverride3D(key, ioSettings);
            physicsSystem->recordDebugContact3D(inManifold);
            // the global events leave out a CharacterController's inner body (hidden)
            const bool shown = !hidden(inBody1, inBody2);

            // C++ contact modification: every manifold, live, on the job threads
            if (shown && physicsSystem->onContactModify3D.hasSubscribers()){
                Rigidbody body1(scene, PhysicsCallbacks::bodyEntity(inBody1.GetUserData()));
                Rigidbody body2(scene, PhysicsCallbacks::bodyEntity(inBody2.GetUserData()));
                Contact3D live(scene, &inBody1, &inBody2, &inManifold, &ioSettings);
                // concurrently on every job thread: no crash guard (its signal handlers are
                // process-wide), no lock; the subscribers' contract (PhysicsSystem.h)
                physicsSystem->onContactModify3D.callDirect(body1, body2, live);
            }

            FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>& event = (kind == RawContact3D::Added) ? physicsSystem->onContactAdded3D : physicsSystem->onContactPersisted3D;
            const bool subscribers = shown && event.hasSubscribers();
            if (subscribers && physicsSystem->isStepSynchronous3D()){
                // synchronous mode: the old in-step delivery with a live Contact3D, now on the
                // main thread
                std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());
                const AudioSystem::PhysicsCallbackScope audioDefers;
                Rigidbody body1(scene, PhysicsCallbacks::bodyEntity(inBody1.GetUserData()));
                Rigidbody body2(scene, PhysicsCallbacks::bodyEntity(inBody2.GetUserData()));
                event(body1, body2, Contact3D(scene, &inBody1, &inBody2, &inManifold, &ioSettings));
            }

            record(kind, inBody1, inBody2, inManifold, ioSettings, subscribers && !physicsSystem->isStepSynchronous3D());
        }

	public:
        JoltContactListener(Scene* scene, PhysicsSystem* physicsSystem, ContactRecorder3D* recorder){
            this->scene = scene;
            this->physicsSystem = physicsSystem;
            this->recorder = recorder;
        }

		// See: ContactListener. With a shouldCollide3D subscriber the step runs its jobs on the
		// main thread (callingThreadPhysicsJobSystem); a Lua one also makes it synchronous
		virtual JPH::ValidateResult	OnContactValidate(const JPH::Body &inBody1, const JPH::Body &inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult &inCollisionResult) override{
			if (!physicsSystem->shouldCollide3D.hasSubscribers()){
				return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
			}
			std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());

			Rigidbody body1(scene, PhysicsCallbacks::bodyEntity(inBody1.GetUserData()));
			Rigidbody body2(scene, PhysicsCallbacks::bodyEntity(inBody2.GetUserData()));

			if (!physicsSystem->shouldCollide3D.callRet(body1, body2, Vector3(inBaseOffset.GetX(), inBaseOffset.GetY(), inBaseOffset.GetZ()), CollideShapeResult3D(scene, &inBody1, &inBody2, &inCollisionResult), true)){
				return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
			}

			return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
		}

		// Added and persisted contacts: the settings a script changed on this contact apply
		// first; the event is recorded (a copy of the contact) and delivered after the step
		virtual void OnContactAdded(const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings) override{
			contact(RawContact3D::Added, inBody1, inBody2, inManifold, ioSettings);
		}

		virtual void OnContactPersisted(const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings) override{
			contact(RawContact3D::Persisted, inBody1, inBody2, inManifold, ioSettings);
		}

		// The bodies cannot be read here: the ids are kept and resolved after the step (the
		// per-object callbacks never need it, they diff the pairs)
		virtual void OnContactRemoved(const JPH::SubShapeIDPair &inSubShapePair) override{
			// a setting a script changed on the contact ends with it
			physicsSystem->forgetContact3D(ContactPairKey3D{inSubShapePair.GetBody1ID().GetIndexAndSequenceNumber(),
			                                                inSubShapePair.GetBody2ID().GetIndexAndSequenceNumber(),
			                                                inSubShapePair.GetSubShapeID1().GetValue(), inSubShapePair.GetSubShapeID2().GetValue()});
			if (!physicsSystem->onContactRemoved3D.hasSubscribers()){
				return;
			}
			const JPH::BodyID body1Id = inSubShapePair.GetBody1ID();
			const JPH::BodyID body2Id = inSubShapePair.GetBody2ID();
			if (body1Id.IsInvalid() || body2Id.IsInvalid()){
				return;
			}

			// as hidden(): an inner body's removal is reported only against a trigger
			if (physicsSystem->isCharacterInnerBody(body1Id) || physicsSystem->isCharacterInnerBody(body2Id)){
				const JPH::BodyLockInterface& locks = physicsSystem->getWorld3D()->GetBodyLockInterfaceNoLock();
				const JPH::Body* body1 = locks.TryGetBody(body1Id);
				const JPH::Body* body2 = locks.TryGetBody(body2Id);
				if (!body1 || !body2 || (!body1->IsSensor() && !body2->IsSensor())){
					return;
				}
			}

			if (physicsSystem->isStepSynchronous3D()){
				physicsSystem->dispatchContactRemoved3D(body1Id, body2Id, inSubShapePair.GetSubShapeID1().GetValue(), inSubShapePair.GetSubShapeID2().GetValue());
				return;
			}

			RawContact3D raw;
			raw.kind = RawContact3D::Removed;
			raw.body1 = body1Id;
			raw.body2 = body2Id;
			raw.subShape1 = inSubShapePair.GetSubShapeID1().GetValue();
			raw.subShape2 = inSubShapePair.GetSubShapeID2().GetValue();
			recorder->push(raw);
		}
	};

}

#endif //JoltPhysicsAux_h
