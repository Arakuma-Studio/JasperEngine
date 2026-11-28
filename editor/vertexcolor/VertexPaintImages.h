// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R33, R41: the pixels the brush stencil and From Texture sample, as the
// op engine takes them (vc::Image, linear). The editor decodes; the engine only samples. Also the one
// other thing the tools read from a target's MeshComponent (one unit includes it, docs/testing.md
// "Build speed"): how the picking BVH numbers a submesh's triangles.

#include "vertexcolor/VertexColorEditing.h"

#include <memory>
#include <optional>
#include <string>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::vertexcolor {

    // a project texture (a project- or assets-relative path, or an absolute one), linear: sRGB-decoded
    // when `colorData`; unset, as the texture's import settings say (R41: "color data (sRGB import)")
    std::shared_ptr<const vc::Image> loadPaintImage(Project* project, const std::string& assetPath, std::optional<bool> colorData, std::string* error);

    // R41 material slots of a target's submesh material
    enum class MaterialSlot { Albedo, Emissive, Occlusion, MetallicRoughness };
    const char* materialSlotName(MaterialSlot slot);
    // the slot's texture times its factor (albedo: base color factor), linear; a slot with no map is
    // its factor alone, one pixel. texCoord: the UV set the material samples it with
    std::shared_ptr<const vc::Image> materialImage(Project* project, uint32_t sceneId, const TargetRef& target, MaterialSlot slot, std::string* error,
                                                   int* texCoord = nullptr);
    // the same by name: "albedo", "emissive", "occlusion", "metallic_roughness", or any texture of the
    // material's shader by its uniform name (R41 "any named texture", decoded as its import settings say)
    std::shared_ptr<const vc::Image> materialImage(Project* project, uint32_t sceneId, const TargetRef& target, const std::string& slot, std::string* error,
                                                   int* texCoord = nullptr);

    // where the picking BVH numbers a submesh's triangles (PickGeometry::submeshFirstTriangle's rule: a
    // source without float positions counts none, a strip n - 2); false unless it is a triangle list
    bool pickTriangleRange(Scene* scene, Entity meshEntity, uint32_t submesh, uint32_t& first, uint32_t& count);

}
