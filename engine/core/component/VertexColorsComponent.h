// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VERTEX_COLORS_COMPONENT_H
#define VERTEX_COLORS_COMPONENT_H

// docs/specs/V1-vertex-colors.md R1-R7, R11-R15, R80: the vertex colors of one placed object, on the
// entity holding its ModelComponent or MeshComponent. The values live in a .vcolors asset; the source
// model or primitive is never written. Absent component: no work at all (R95).

#include "asset/AssetRef.h"
#include "math/Vector4.h"
#include "vertexcolor/VertexColorTypes.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace jasperengine{

    namespace vertexcolor { class RuntimeLayer; }

    struct JASPERENGINE_API VertexColorsComponent{
        // component checkbox (R15): off unbinds the streams, the mesh draws as without it
        bool enabled = true;

        // the .vcolors asset (R1); empty until the first edit creates it
        AssetRef data;

        // R5: storage precision of the GPU stream and the file
        vertexcolor::Format format = vertexcolor::Format::RGBA8;

        // R6
        vertexcolor::InitialValue initialValue = vertexcolor::InitialValue::Asset;
        Vector4 customInitial = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

        // R7: "Moss", "Wetness", ... for R, G, B, A (empty: unnamed)
        std::array<std::string, 4> channelNames;

        // ---- runtime (not serialized)
        // the decoded file (editor: the live, possibly unsaved, values)
        std::shared_ptr<vertexcolor::Data> loaded;
        std::string loadedPath;
        // loadedPath could not be read (logged once; a new `data` path tries again)
        bool loadFailed = false;
        // bumped by every write to `loaded`; the render path re-uploads when it differs from what it bound
        uint64_t revision = 0;
        uint64_t boundRevision = 0;
        bool needUpdate = true;
        // R80-R82: gameplay paint, per target
        std::map<vertexcolor::TargetKey, std::shared_ptr<vertexcolor::RuntimeLayer>> runtimeLayers;
        // R83: logged once
        bool cpuGeometryLogged = false;
    };

}

#endif //VERTEX_COLORS_COMPONENT_H
