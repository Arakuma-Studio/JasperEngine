// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LINE_RENDERER_COMPONENT_H
#define LINE_RENDERER_COMPONENT_H

#include "buffer/Buffer.h"
#include "shader/ShaderUniforms.h"
#include "texture/Material.h"
#include "Engine.h"
#include <string>
#include <memory>

namespace jasperengine{

    struct LineData{
        Vector3 pointA = Vector3(0,0,0);
        Vector4 colorA = Vector4(1,1,1,1);

        Vector3 pointB = Vector3(0,0,0);
        Vector4 colorB = Vector4(1,1,1,1);

        bool operator==(const LineData& other) const {
            return pointA == other.pointA &&
                colorA == other.colorA &&
                pointB == other.pointB &&
                colorB == other.colorB;
        }

        bool operator!=(const LineData& other) const {
            return !(*this == other);
        }
    };

    struct JASPERENGINE_API LineRendererComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        bool loaded = false;
        bool loadCalled = false;

        ExternalBuffer buffer;

        std::vector<LineData> lines;

        unsigned int maxLines = 100;

        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        uint32_t shaderProperties = 0;
        // optional custom shader: "library:<name>" or a project-relative fork base path
        // (no extension), empty = built-in. customShaderId is the resolved id for ShaderPool keys.
        std::string customShader;
        uint16_t customShaderId = 0;
        int slotVSParams = -1;
        // values of the fork's custom uniform blocks, by member name
        ShaderUniformValues shaderUniforms;
        bool needUpdateShaderUniforms = false;
        CustomUniformBlock customVSParams;
        CustomUniformBlock customFSParams;
        // shader library: textures by @texture uniform name and enabled @feature/@keywords
        // defines of the shader above (library reference or fork)
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        std::vector<std::string> shaderFeatures;
        // AUTO keeps the built-in state (alpha blend, depth write on)
        MaterialBlendMode blendMode = MaterialBlendMode::AUTO;
        MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO;
        // phase 2: stencil test/write and colour channels (AUTO = manifest, else none / all)
        MaterialStencil stencil;
        int colorWrite = -1;
        // resolved at load: the shader's manifest (null without one) and what it reads of
        // the frame block and custom textures
        std::shared_ptr<const ShaderManifest> shaderManifest;
        ShaderLibraryBindings libraryBindings;

        bool needUpdateBuffer = false;
        bool needReload = false;
    };
    
}

#endif //LINE_RENDERER_COMPONENT_H