// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R124: the vertex color facts get_model_info and get_entity report

#include "ecs/Entity.h"
#include "mcp/McpJson.h"

#include <cstdint>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp {

    // One entry per paintable target (a submesh of a mesh GameObject): its address for
    // edit_mesh_colors, vertex_count, has_vertex_colors and, once painted, split_vertices and stale.
    // Null when the entity holds nothing paintable.
    Json vertexColorTargetsJson(Project* project, uint32_t sceneId, Entity entity);

}
