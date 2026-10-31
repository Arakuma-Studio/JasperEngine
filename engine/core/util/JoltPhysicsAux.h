// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JoltPhysicsAux_h
#define JoltPhysicsAux_h

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
			if (isIgnored(static_cast<Entity>(inBody.GetUserData()))){
				return false;
			}
			return true;
		}
	};

	class JoltActivationListener : public JPH::BodyActivationListener{
	private:
        Scene* scene;
        PhysicsSystem* physicsSystem;
	public:
        JoltActivationListener(Scene* scene, PhysicsSystem* physicsSystem){
            this->scene = scene;
            this->physicsSystem = physicsSystem;
        }

		// Jolt calls these from its job threads: subscribers run one at a time (see
		// PhysicsSystem::getCallbackMutex3D), and not at all when there are none
		virtual void OnBodyActivated(const JPH::BodyID &inBodyID, uint64_t inBodyUserData) override{
			if (!physicsSystem->onBodyActivated3D.hasSubscribers()){
				return;
			}
			std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());
			physicsSystem->onBodyActivated3D(Rigidbody(scene, inBodyUserData));
		}

		virtual void OnBodyDeactivated(const JPH::BodyID &inBodyID, uint64_t inBodyUserData) override{
			if (!physicsSystem->onBodyDeactivated3D.hasSubscribers()){
				return;
			}
			std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());
			physicsSystem->onBodyDeactivated3D(Rigidbody(scene, inBodyUserData));
		}
	};

	class JoltContactListener : public JPH::ContactListener{
    private:
        Scene* scene;
        PhysicsSystem* physicsSystem;
	public:
        JoltContactListener(Scene* scene, PhysicsSystem* physicsSystem){
            this->scene = scene;
            this->physicsSystem = physicsSystem;
        }

		// See: ContactListener
		// Called from Jolt's job threads, see JoltActivationListener
		virtual JPH::ValidateResult	OnContactValidate(const JPH::Body &inBody1, const JPH::Body &inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult &inCollisionResult) override{
			if (!physicsSystem->shouldCollide3D.hasSubscribers()){
				return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
			}
			std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());

			Entity entity1 = inBody1.GetUserData();
			Rigidbody body1(scene, entity1);

			Entity entity2 = inBody2.GetUserData();
			Rigidbody body2(scene, entity2);

			if (!physicsSystem->shouldCollide3D.callRet(body1, body2, Vector3(inBaseOffset.GetX(), inBaseOffset.GetY(), inBaseOffset.GetZ()), CollideShapeResult3D(scene, &inBody1, &inBody2, &inCollisionResult), true)){
				return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
			}

			return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
		}

		virtual void OnContactAdded(const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings) override{
			// per-collider triggers first (Jolt sets the settings up again for every call)
			if (isTriggerContact3D(inBody1, inBody2, inManifold)){
				ioSettings.mIsSensor = true;
			}
			physicsSystem->recordDebugContact3D(inManifold);
			if (!physicsSystem->onContactAdded3D.hasSubscribers()){
				return;
			}
			std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());

			Entity entity1 = inBody1.GetUserData();
			Rigidbody body1(scene, entity1);

			Entity entity2 = inBody2.GetUserData();
			Rigidbody body2(scene, entity2);

			physicsSystem->onContactAdded3D(body1, body2, Contact3D(scene, &inBody1, &inBody2, &inManifold, &ioSettings));
		}

		virtual void OnContactPersisted(const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings) override{
			if (isTriggerContact3D(inBody1, inBody2, inManifold)){
				ioSettings.mIsSensor = true;
			}
			physicsSystem->recordDebugContact3D(inManifold);
			if (!physicsSystem->onContactPersisted3D.hasSubscribers()){
				return;
			}
			std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());

			Entity entity1 = inBody1.GetUserData();
			Rigidbody body1(scene, entity1);

			Entity entity2 = inBody2.GetUserData();
			Rigidbody body2(scene, entity2);

			physicsSystem->onContactPersisted3D(body1, body2, Contact3D(scene, &inBody1, &inBody2, &inManifold, &ioSettings));
		}

		virtual void OnContactRemoved(const JPH::SubShapeIDPair &inSubShapePair) override{
			if (!physicsSystem->onContactRemoved3D.hasSubscribers()){
				return;
			}

			JPH::BodyInterface &body_interface = physicsSystem->getWorld3D()->GetBodyInterfaceNoLock();

			const JPH::BodyID body1Id = inSubShapePair.GetBody1ID();
			const JPH::BodyID body2Id = inSubShapePair.GetBody2ID();

			if (body1Id.IsInvalid() || body2Id.IsInvalid()){
				return;
			}

			if (!body_interface.IsAdded(body1Id) || !body_interface.IsAdded(body2Id)){
				return;
			}

			auto shape1 = body_interface.GetShape(body1Id);
			auto shape2 = body_interface.GetShape(body2Id);

			if (!shape1 || !shape2){
				return;
			}

			Entity entity1 = body_interface.GetUserData(body1Id);
			Entity entity2 = body_interface.GetUserData(body2Id);

			size_t shapeIndex1 = shape1->GetSubShapeUserData(inSubShapePair.GetSubShapeID1());
			size_t shapeIndex2 = shape2->GetSubShapeUserData(inSubShapePair.GetSubShapeID2());

			std::lock_guard<std::recursive_mutex> lock(physicsSystem->getCallbackMutex3D());
			physicsSystem->onContactRemoved3D(Rigidbody(scene, entity1), Rigidbody(scene, entity2), (unsigned long)shapeIndex1, (unsigned long)shapeIndex2);
		}
	};

}

#endif //JoltPhysicsAux_h
