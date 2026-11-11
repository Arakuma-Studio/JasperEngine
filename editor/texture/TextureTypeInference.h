// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a texture is, from what references it (spec S14 R2, R3): the S04 dependency edges into
// it, read once when a texture first gets import settings (written then, never silently
// re-inferred), and at every cook for the CPU-readable slots. A normal slot (a material's or a
// glTF's normalTexture, a manifest `@texture ... normal`) gives normal; sprites, tilemaps and
// particles sprite; UI images and HTML UI images ui; occlusion slots single_channel; .hdr and
// .exr hdr; otherwise default. Metallic, roughness and height slots are linear data.

#pragma once

#include "asset/DepEdge.h"
#include "texture/TextureImportSettings.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // SourceFile: the reference reads the source file itself (HTML UI images, terrain surface layers)
    enum class TextureSlot : uint8_t { Color, Normal, Occlusion, LinearData, Sprite, UI, CpuReadable, Cube, SourceFile };

    // one reference, classified
    struct TextureReferenceUse {
        TextureSlot slot = TextureSlot::Color;
        std::string where;      // "MeshComponent.submeshes[0].material.normalTexture of scenes/level.scene"
    };

    struct TextureInference {
        TextureImportType type = TextureImportType::Default;
        std::string reason;                 // inferredFrom
        bool linear = false;                // srgb: false (data, not colour)
        TextureShape shape = TextureShape::Tex2D;
        // R3: a reference reads the pixels on the CPU
        bool cpuReadable = false;
        std::string cpuReason;
        // a reference reads the source file itself (terrain layers, HTML UI images, a glTF's
        // external images): exports ship the source
        bool readsSource = false;
        // every reference does: nothing cooks it
        bool sourceOnly = false;
        std::vector<TextureReferenceUse> uses;
    };

    class TextureTypeInference {
    public:
        // In EdgeContext::componentsOf's set: the entity's terrain has Allow Runtime Edits (T1 R77),
        // so its blend maps are read on the CPU. Not a component name.
        static constexpr const char* TERRAIN_RUNTIME_EDITS_MARK = "#terrainRuntimeEdits";

        // What the references of `projectRelative` (and its file name) say
        // (main thread: the index's overlay of unsaved documents)
        static TextureInference infer(Project& project, const std::string& projectRelative);
        // the same in two steps: the edges into it (main thread), then their reading (any
        // thread: it reads referrer files and shader manifests)
        static std::vector<DepEdge> referenceEdges(Project& project, const std::string& projectRelative);
        static TextureInference fromEdges(const Project& project, const std::string& projectRelative, const std::vector<DepEdge>& edges);

        // The same from classified uses (the unit tests feed these)
        static TextureInference fromUses(const std::vector<TextureReferenceUse>& uses, const std::string& fileName);

        // One edge, classified. `componentsOf` gives the component names of an entity of a
        // referrer file (its sprites); `shaderTextureKind` the manifest's `@texture` of a uniform
        // (normal, cube) for the shader the edge's material uses.
        struct EdgeContext {
            std::function<std::set<std::string>(const Guid& referrer, uint64_t entity)> componentsOf;
            std::function<TextureSlot(const DepEdge& edge, const std::string& uniform, bool& cube)> shaderTextureKind;
            std::function<std::vector<TextureSlot>(const DepEdge& edge)> modelSlots;   // glTF image uses
            std::function<std::string(const Guid& referrer)> referrerPath;          // project-relative
        };
        static std::vector<TextureSlot> classify(const DepEdge& edge, const EdgeContext& context);

        // R2's warning: a reference that does not fit the stored type ("a colour slot binds this
        // normal map: ...")
        static std::vector<std::string> conflicts(TextureImportType type, const TextureInference& references);
        // the type of a slot ("normal" for Normal ...), for messages
        static const char* slotName(TextureSlot slot);
    };

}
