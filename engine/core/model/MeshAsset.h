// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESHASSET_H
#define MESHASSET_H

// The .mesh asset (docs/specs/S98-editor-tooling-gaps.md F1, Deviations): a mesh's geometry in a
// file of its own, as Unity's Mesh asset, so a scene or prefab names it instead of holding its
// buffers. A Mesh Renderer refers to one through MeshComponent::meshAsset; MeshSystem reads it
// into the component. Materials, shaders and the skin's palette stay with the component.
//
//   header   magic "JMSH", u16 major, u16 minor                                        (8 bytes)
//   strings  varint count, then each string as a varint length and its UTF-8 bytes
//   body     the cooked scenes' codecs (serialization/codecs/CookTypes.h): the vertices' bounds,
//            the vertex count, the interleaved vertex buffer, the index buffer, the external
//            buffers, then each submesh's primitive type, vertex count and attributes
//
// Readers refuse another major version; a newer minor only appends to the body.

#include "Export.h"
#include "math/AABB.h"
#include "render/Render.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace jasperengine {

    struct MeshComponent;

    namespace MeshAsset {

        constexpr char MAGIC[4] = {'J', 'M', 'S', 'H'};
        constexpr uint16_t FORMAT_MAJOR = 1;
        constexpr uint16_t FORMAT_MINOR = 0;
        constexpr const char* EXTENSION = ".mesh";

        // What a file holds, without a component to read it into (get_asset_info, the Inspector)
        struct Info {
            uint16_t major = 0;
            uint16_t minor = 0;
            unsigned int vertexCount = 0;
            unsigned int indexCount = 0;
            size_t geometryBytes = 0;       // the buffers' bytes
            AABB bounds;
            struct Submesh {
                PrimitiveType primitiveType = PrimitiveType::TRIANGLES;
                unsigned int vertexCount = 0;
            };
            std::vector<Submesh> submeshes;
            std::vector<AttributeType> attributes;   // what the vertex buffers hold, of all of them
        };

        // The bytes of the vertex, index and external buffers: what a scene saving the mesh inline holds
        JASPERENGINE_API size_t geometryBytes(const MeshComponent& mesh);

        // The file's bytes of the mesh's geometry (deterministic: the same geometry, the same bytes)
        JASPERENGINE_API std::vector<uint8_t> encode(const MeshComponent& mesh);

        // The geometry in `bytes` replaces the mesh's buffers, its submeshes' primitive types, vertex
        // counts and attributes; submeshes past the file's count go, new ones start as defaults. The
        // mesh is left as it was when false (with the reason in `error`).
        JASPERENGINE_API bool decode(std::span<const uint8_t> bytes, MeshComponent& mesh, std::string* error = nullptr);

        JASPERENGINE_API bool inspect(std::span<const uint8_t> bytes, Info& info, std::string* error = nullptr);

        // decode of a file read through the engine's file system (the path as the scene names it:
        // relative to the assets root, in a pack or an app bundle alike)
        JASPERENGINE_API bool load(const std::string& path, MeshComponent& mesh, std::string* error = nullptr);

        // encode written atomically (AtomicFile); an unchanged file is left untouched
        JASPERENGINE_API bool save(const std::filesystem::path& file, const MeshComponent& mesh, std::string* error = nullptr);

    }

}

#endif // MESHASSET_H
