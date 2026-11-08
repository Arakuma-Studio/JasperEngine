// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_VOLUME_H
#define VOXEL_VOLUME_H

// T6 R73: the Lua-facing handle of a Voxel Volume. Everything here is in world space; the system converts
// to the volume's frame.

#include "object/EntityHandle.h"

#include "component/VoxelVolumeComponent.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "voxel/VoxelEdit.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    class GameObject;
    class VoxelSystem;

    // VoxelShape.sphere(center, radius) and friends
    struct JASPERENGINE_API VoxelShape{
        voxel::ShapeKind kind = voxel::ShapeKind::Sphere;
        Vector3 center;
        float radius = 1.0f;
        Vector3 halfExtents = Vector3(1.0f, 1.0f, 1.0f);
        Quaternion rotation;
        Vector3 from, to;
        float scale = 1.0f;
        Entity meshEntity = NULL_ENTITY;
        Scene* meshScene = nullptr;
        std::string stampPath;

        static VoxelShape sphere(Vector3 center, float radius);
        static VoxelShape box(Vector3 center, Vector3 halfExtents, Quaternion rotation);
        static VoxelShape capsule(Vector3 from, Vector3 to, float radius);
        static VoxelShape mesh(GameObject go);
        static VoxelShape stamp(const std::string& path, Vector3 position, Quaternion rotation, float scale);
    };

    struct JASPERENGINE_API VoxelEditOptions{
        float blend = 0.0f;
        int material = -1;
        float weight = 1.0f;
        float falloff = 0.0f;
        float amount = 0.5f;
        int iterations = 1;
    };

    // What every edit returns (R27, R73)
    struct JASPERENGINE_API VoxelEditReport{
        bool ok = false;
        std::string error;
        AABB bounds;
        int voxelsChanged = 0;
        int surfaceVoxelsChanged = 0;
        int blocksTouched = 0;
        int memoryDelta = 0;
        int clampedVoxels = 0;
        float removedVolume = 0.0f;
        float addedVolume = 0.0f;
    };

    struct JASPERENGINE_API VoxelRayHit{
        bool hit = false;
        Vector3 point;
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        float distance = 0.0f;
        int slot = 0;
    };

    struct JASPERENGINE_API VoxelMaterialHit{
        int slot = 0;
        int second = 0;
        int weight = 0;
    };

    struct JASPERENGINE_API VoxelVolumeStats{
        int blocks = 0;
        int uniformBlocks = 0;
        int nonUniformBlocks = 0;
        int modifiedBlocks = 0;
        int memoryBytes = 0;
        int meshBytes = 0;
        int triangles = 0;
        int chunks = 0;
        int collisionBodies = 0;
        int jobsPending = 0;
    };

    class JASPERENGINE_API VoxelVolume: public EntityHandle{
    public:
        VoxelVolume(Scene* scene, Entity entity);
        virtual ~VoxelVolume();

        VoxelVolume(const VoxelVolume& rhs);
        VoxelVolume& operator=(const VoxelVolume& rhs);
        VoxelVolume(VoxelVolume&& rhs) noexcept;
        VoxelVolume& operator=(VoxelVolume&& rhs) noexcept;

        bool isValid() const;
        // A physics hit's GameObject, a chunk or the volume's own, back to the volume (R19)
        static VoxelVolume fromGameObject(GameObject go);

        VoxelEditReport dig(const VoxelShape& shape, const VoxelEditOptions& options);
        VoxelEditReport fill(const VoxelShape& shape, const VoxelEditOptions& options);
        VoxelEditReport smooth(const VoxelShape& shape, const VoxelEditOptions& options);
        VoxelEditReport paint(const VoxelShape& shape, const VoxelEditOptions& options);

        VoxelRayHit raycast(Vector3 origin, Vector3 direction, float maxDistance);
        float getValueAt(Vector3 point);
        VoxelMaterialHit getMaterialAt(Vector3 point);

        void ensureCollision(const AABB& bounds);
        bool isCollisionReady(const AABB& bounds);
        VoxelVolumeStats getStats() const;

        // Runtime saves (R79). The error text is the file's code.
        bool saveChanges(const std::string& slot);
        bool loadChanges(const std::string& slot);
        bool hasSavedChanges(const std::string& slot) const;
        void revertChanges();
        std::string getLastError() const { return lastError; }

        FunctionSubscribe<void(VoxelEditEvent)>& getOnEdited();
        FunctionSubscribe<void(AABB)>& getOnCollisionUpdated();
        FunctionSubscribe<void(GameObject)>& getOnBodyRescued();

    private:
        VoxelEditReport apply(voxel::EditKind kind, const VoxelShape& shape, const VoxelEditOptions& options);
        VoxelSystem* system() const;
        std::string lastError;
    };

}

#endif
