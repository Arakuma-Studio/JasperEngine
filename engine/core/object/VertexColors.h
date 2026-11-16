// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VERTEXCOLORS_H
#define VERTEXCOLORS_H

// docs/specs/V1-vertex-colors.md R80-R83, R125: a GameObject's Vertex Colors at runtime, Lua's
// VertexColors(scene, entity). Writes change the colours in memory, never the .vcolors file, and
// have no undo; a copy of the GameObject that shared the file's data gets its own on the first
// write. paint adds transient gameplay paint over the authored colours (R80).

#include "EntityHandle.h"
#include "json.hpp"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "subsystem/VertexColorScene.h"
#include "vertexcolor/VertexColorRuntimeLayer.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jasperengine {

    struct MeshRaycastHit;
    struct VertexColorsComponent;

    class JASPERENGINE_API VertexColors : public EntityHandle {
    public:
        // R125: getValues returns at most this many values
        static constexpr uint32_t MAX_VALUES = 65536;

        VertexColors(Scene* scene, Entity entity);
        virtual ~VertexColors();
        VertexColors(const VertexColors& rhs);
        VertexColors& operator=(const VertexColors& rhs);
        VertexColors(VertexColors&& rhs) noexcept;
        VertexColors& operator=(VertexColors&& rhs) noexcept;

        // the GameObject has a Vertex Colors component
        bool isValid() const;
        bool isEnabled() const;
        void setEnabled(bool enabled);
        // "rgba8" or "rgba16f"
        std::string getFormat() const;
        // R, G, B, A ("" unnamed)
        std::vector<std::string> getChannelNames() const;
        // the paintable targets, in key order
        std::vector<vertexcolor::SceneTarget> getTargets() const;

        // R125: the op list (edit_mesh_colors' op objects) on every target, or on `only`; all or
        // nothing. `result`: an array of per-target results ({target, changed..., ops, stats, hash})
        bool applyOps(const nlohmann::json& ops, const vertexcolor::TargetKey* only, nlohmann::json& result, std::string& error);
        // authored values of GPU vertices first .. first + count - 1 (count <= MAX_VALUES)
        bool getValues(const vertexcolor::TargetKey& key, uint32_t first, uint32_t count, std::vector<Vector4>& out, std::string& error);
        // sets them, clearing the corner overrides of those vertices
        bool setValues(const vertexcolor::TargetKey& key, uint32_t first, std::span<const Vector4> values, std::string& error);
        // per-channel min, mean, max, percentiles and histogram of a target (sample_mesh_colors stats)
        bool getStats(const vertexcolor::TargetKey& key, nlohmann::json& out, std::string& error);

        // the colour drawn at the surface point nearest to a world point (read in bind pose, D12), or
        // at a MeshRaycast hit on one of its meshes (its triangle and barycentrics): authored with
        // the runtime paint over it. False when nothing is there to sample
        bool sampleAt(const Vector3& worldPoint, Vector4& out, std::string& error);
        bool sampleAt(const MeshRaycastHit& hit, Vector4& out, std::string& error);

        // R80: gameplay paint around a world point (bind pose) or a hit (mapped to bind pose
        // through its triangle, D12), radius in world units; the vertices touched
        uint32_t paint(const Vector3& worldPoint, const vertexcolor::RuntimePaint& paint, std::string& error);
        uint32_t paint(const MeshRaycastHit& hit, const vertexcolor::RuntimePaint& paint, std::string& error);
        // R82: drops the runtime paint
        void clearRuntime();

        // a key from its description: "node 3 primitive 0" style errors name the targets there are
        std::string describeTargets() const;

    private:
        VertexColorsComponent* component() const;
        bool target(const vertexcolor::TargetKey& key, vertexcolor::SceneTarget& out, std::string& error) const;
        // the bind-pose world point of a hit, false when it is not on one of the targets
        bool hitPoint(const MeshRaycastHit& hit, Vector3& world, vertexcolor::SceneTarget& target, std::string& error);
    };

}

#endif // VERTEXCOLORS_H
