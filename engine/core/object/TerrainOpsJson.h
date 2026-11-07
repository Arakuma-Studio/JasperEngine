// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_OPS_JSON_H
#define TERRAIN_OPS_JSON_H

// T1 R68, R75: the op objects of edit_terrain and Terrain:applyOps are one format, read here once,
// and the result goes back in one shape. Lua reaches this through LuaJson, so a table and a JSON
// object mean the same op.

#include "Export.h"
#include "json.hpp"
#include "math/Matrix4.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "terrain/TerrainOps.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::terrain {

    // Terrain-local from the caller's space (DC5): XZ through the inverse model, heights by the Y scale
    struct JASPERENGINE_API OpSpace {
        bool world = true;
        Matrix4 model;
        Matrix4 inverse;
        float originY = 0.0f;
        float scaleY = 1.0f;
        float scaleXZ = 1.0f;
        bool rotated = false;

        static OpSpace of(const Matrix4& model, const Vector3& worldPosition, const Vector3& worldScale, bool rotated, bool world);

        Vector2 point(float x, float z) const;
        float height(float y) const { return world ? (y - originY) / scaleY : y; }
        float delta(float d) const { return world ? d / scaleY : d; }
        float length(float l) const { return world ? l / scaleXZ : l; }
        float worldHeight(float local) const { return world ? local * scaleY + originY : local; }
        float worldDelta(float local) const { return world ? local * scaleY : local; }
        Vector2 worldPoint(float x, float z) const;
    };

    // What only the caller can do. A null hook refuses what needs it.
    struct OpJsonHooks {
        // A mask shape's image at `path`: fills shape.maskWidth, maskHeight and maskPixels
        std::function<bool(const std::string& path, Shape& shape, std::string& error)> loadMask;
        // import_heightmap's fields: fills op.imported*; on failure names the field
        std::function<bool(const nlohmann::json& item, Op& op, std::string& field, std::string& error)> importHeightmap;
        // Ops this caller does not take, with what to use instead
        std::vector<std::pair<OpType, std::string>> refused;
        // Where the field list is, in errors
        std::string fieldsDoc = "jasper://docs/terrain";
        // The first op's number in errors: 0 for JSON callers, 1 for Lua's tables
        size_t firstIndex = 0;
    };

    class JASPERENGINE_API OpJsonParser {
    public:
        OpJsonParser(const OpSpace& space, OpJsonHooks hooks);

        // ops[index] into op; false with error ("ops[2].shape.radius: ...")
        bool parse(const nlohmann::json& item, size_t index, Op& op);
        // A whole list; false with error at the first bad op
        bool parseList(const nlohmann::json& list, std::vector<Op>& ops);

        std::string error;

    private:
        const OpSpace& space;
        OpJsonHooks hooks;
        std::string where;

        bool failWith(const std::string& field, const std::string& message);
        bool point(const nlohmann::json& value, const std::string& field, Vector2& out, float* pressure = nullptr);
        bool parseShape(const nlohmann::json& value, Shape& shape);
        bool parseFilter(const nlohmann::json& value, Filter& filter);
        bool parseRule(const nlohmann::json& value, size_t index, Op& op);
        bool parseRegistered(const nlohmann::json& item, Op& op);
    };

    // "height", "hole", "blend:0", "density:1"
    JASPERENGINE_API std::string mapIdName(MapId map);
    JASPERENGINE_API std::string hashHex(uint64_t value);

    // The result (§6): changed maps, per-op entries, warnings, created maps and stats, in the
    // caller's space; a warning's op counts from firstIndex, as the caller's errors do
    JASPERENGINE_API nlohmann::json opResultJson(const TerrainOpResult& result, const OpSpace& space, const std::vector<Op>& ops,
                                                 size_t firstIndex = 0);

}

#endif // TERRAIN_OPS_JSON_H
