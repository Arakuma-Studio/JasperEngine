// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R120, R125, D10: the op objects of edit_mesh_colors and
// VertexColors:applyOps are one format, read here once (the TerrainOpsJson pattern). Lua reaches
// this through LuaJson, so a table and a JSON object mean the same op.
//
// Values: [r, g, b] or [r, g, b, a] linear 0..1, or "#RRGGBB[AA]" (sRGB, converted). Channels:
// "rgba" subset string. Targets: {"kind": "all"|"vertices"|"faces"|"corners"|"selection"|"sphere"|
// "box"|"capsule"|"color", ...}. Field names are snake_case as in Table 1.

#include "Export.h"
#include "json.hpp"
#include "vertexcolor/VertexColorOps.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::vertexcolor {

    // What only the caller can do. A null hook refuses what needs it, naming the field.
    struct OpJsonHooks {
        // from_texture {texture: path} or {material_slot: "albedo"|...}: decoded linear pixels
        std::function<std::shared_ptr<const Image>(const nlohmann::json& item, std::string& field, std::string& error)> loadImage;
        // stroke {stencil: path}
        std::function<std::shared_ptr<const Image>(const std::string& path, std::string& error)> loadStencil;
        // transfer {source_entity: ...}
        std::function<std::shared_ptr<const TransferSource>(const nlohmann::json& value, std::string& error)> transferSource;
        // target {kind: "selection"}: the editor's element selection of this target
        std::function<bool(Target& target, std::string& error)> selection;
        // The first op's number in errors: 0 for JSON callers, 1 for Lua's tables
        size_t firstIndex = 0;
        // Where the field list is, in errors
        std::string fieldsDoc = "jasper://docs/meshes";
    };

    class JASPERENGINE_API OpJsonParser {
    public:
        explicit OpJsonParser(OpJsonHooks hooks);

        // ops[index] into op; false with error ("ops[2].target.radius: must be > 0")
        bool parse(const nlohmann::json& item, size_t index, Op& op);
        bool parseList(const nlohmann::json& list, std::vector<Op>& ops);

        std::string error;

    private:
        OpJsonHooks hooks;
    };

    JASPERENGINE_API bool parseValue(const nlohmann::json& value, Vector4& out, std::string& error);
    JASPERENGINE_API bool parseChannels(const nlohmann::json& value, ChannelMask& out, std::string& error);
    JASPERENGINE_API std::string channelsName(ChannelMask channels);   // "rgb"

    // the result in the shape edit_mesh_colors returns per target and Lua's applyOps returns
    JASPERENGINE_API nlohmann::json toJson(const OpsResult& result);
    JASPERENGINE_API nlohmann::json toJson(const ChannelStats& stats);

}
