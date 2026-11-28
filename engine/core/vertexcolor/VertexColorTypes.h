// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md: the data model of per-object vertex colors. Pure data: no ECS, render
// or editor include, so the op engine, the file, the render path, MCP and Lua share it.
// Values are linear RGBA (D4); 0..1 for RGBA8, any float for RGBA16F.

#include "Export.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::vertexcolor {

    // R5: how values are stored on the GPU and in the file. Format and InitialValue are component
    // fields: int-sized, as the editor's properties (Inspector, MCP, prefab overrides) read and write
    // every enum as an int
    enum class Format : int { RGBA8 = 0, RGBA16F = 1 };

    // R6: what a target holds before its first edit (and what Erase / clear{initial} go back to)
    enum class InitialValue : int { Asset = 0, White = 1, Black = 2, Custom = 3 };

    // R21: a subset of rgba
    using ChannelMask = uint8_t;
    constexpr ChannelMask CHANNEL_R = 1;
    constexpr ChannelMask CHANNEL_G = 2;
    constexpr ChannelMask CHANNEL_B = 4;
    constexpr ChannelMask CHANNEL_A = 8;
    constexpr ChannelMask CHANNELS_RGB = 7;
    constexpr ChannelMask CHANNELS_RGBA = 15;

    // R21
    enum class Blend : uint8_t { Replace, Add, Subtract, Multiply, Screen, Overlay, Min, Max, Lerp };

    // R24: what a shape target writes. Vertex: logical vertices (smooth); Face: whole triangles as
    // corner values (hard edges)
    enum class Element : uint8_t { Vertex, Face };

    // R50: the kinds of ids a selection or an id target holds
    enum class ElementKind : uint8_t { Vertex, Face, Corner };

    // R2: which mesh primitive a target paints. Models: glTF node + primitive (submesh = -1). Plain
    // meshes and Merge Static Meshes models: submesh index (node = primitive = -1).
    struct JASPERENGINE_API TargetKey {
        int32_t node = -1;
        int32_t primitive = -1;
        int32_t submesh = -1;

        bool operator==(const TargetKey& o) const { return node == o.node && primitive == o.primitive && submesh == o.submesh; }
        bool operator!=(const TargetKey& o) const { return !(*this == o); }
        bool operator<(const TargetKey& o) const {
            if (node != o.node) return node < o.node;
            if (primitive != o.primitive) return primitive < o.primitive;
            return submesh < o.submesh;
        }
        // "node 3 primitive 0" / "submesh 1": what messages and MCP name
        std::string describe() const;
    };

    // R9: the mesh a target was painted on
    struct JASPERENGINE_API Fingerprint {
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint64_t topologyHash = 0;   // the index buffer: identical across cook families (D11)
        uint64_t positionHash = 0;   // positions quantized to 1e-4 of the bounds: editor-only check

        bool operator==(const Fingerprint& o) const {
            return vertexCount == o.vertexCount && indexCount == o.indexCount && topologyHash == o.topologyHash && positionHash == o.positionHash;
        }
        bool sameTopology(const Fingerprint& o) const {
            return vertexCount == o.vertexCount && indexCount == o.indexCount && topologyHash == o.topologyHash;
        }
    };

    // R3: a triangle corner (triangle * 3 + k) whose value differs from its GPU vertex's base value
    struct CornerValue {
        uint32_t corner = 0;
        Vector4 value;
    };

    // One painted primitive (R2, R3)
    struct JASPERENGINE_API TargetData {
        TargetKey key;
        Fingerprint fingerprint;
        // per GPU vertex
        std::vector<Vector4> base;
        // sorted by corner, unique; never equal to base[indices[corner]]
        std::vector<CornerValue> corners;
        // editor-only (R9, stripped from exports, R92): local positions and normals per GPU vertex at
        // the time of the last save, so a reimport can transfer the values (R10, R60)
        std::vector<Vector3> snapshotPositions;
        std::vector<Vector3> snapshotNormals;
        // and its triangle list, so the transfer carries corner overrides (hard edges) too. The file
        // keeps it only for a target with overrides: without them the per-vertex transfer is exact
        std::vector<uint32_t> snapshotIndices;

        // corner override or the base value of the corner's vertex
        Vector4 cornerValue(uint32_t corner, const std::vector<uint32_t>& indices) const;
        // sets one corner, dropping the override when it equals the base value
        void setCorner(uint32_t corner, const Vector4& value, const std::vector<uint32_t>& indices);
        // sets a GPU vertex's base value and removes the overrides of its corners (R4 for one vertex;
        // callers set every member of a logical vertex)
        bool hasOverrides() const { return !corners.empty(); }
    };

    // One .vcolors file (R1, R131)
    struct JASPERENGINE_API Data {
        Format format = Format::RGBA8;
        std::vector<TargetData> targets;   // sorted by key

        TargetData* find(const TargetKey& key);
        const TargetData* find(const TargetKey& key) const;
        // inserts an empty target in key order
        TargetData& add(const TargetKey& key);
    };

    // The value a fresh target starts from (R6). `asset` is the mesh's own COLOR_0 for that vertex,
    // or null when the mesh has none (then Asset means white)
    JASPERENGINE_API Vector4 initialValueFor(InitialValue initial, const Vector4& custom, const Vector4* asset);

    // a value as the format stores it (RGBA8 rounds to 1/255 and clamps; RGBA16F rounds to half)
    JASPERENGINE_API Vector4 quantize(const Vector4& value, Format format);

}
