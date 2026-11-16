// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Inspector > Vertex Colors (docs/specs/V1-vertex-colors.md R5-R7, R10, R100, R124, Table 2): what
// its rows say, so the tests read the same text the Inspector draws. The drawing is
// InspectorWindow::drawVertexColorsComponent (VertexColorInspector.cpp).

#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    enum class VertexColorInspectorRowKind : uint8_t { Asset, Format, Enum, Color, ChannelName };
    struct VertexColorInspectorRow {
        const char* property;   // the Catalog's name
        const char* label;      // Unity-style
        VertexColorInspectorRowKind kind;
        const char* help;
    };
    // Data, Format, Initial Value, Custom Initial, then R, G, B and A Channel (the channel names: never
    // "Element 0" to "Element 3")
    const std::vector<VertexColorInspectorRow>& vertexColorInspectorRows();

    // one painted or paintable mesh of the GameObject
    struct VertexColorTargetRow {
        std::string name;       // "Body (node 3, primitive 0)"
        std::string detail;     // "1,204 vertices, +96 vertices for hard edges" / "not painted"
        bool stale = false;     // R10
        bool painted = false;
    };
    std::vector<VertexColorTargetRow> vertexColorTargetRows(Project* project, uint32_t sceneId, Entity entity);

    // the red strip: the file that did not load, R10's remap issues, Table 2's material warnings for
    // the channels the paint uses
    std::vector<std::string> vertexColorProblemLines(Project* project, uint32_t sceneId, Entity entity);

    // "1,204"
    std::string vertexColorCount(uint64_t value);

}
