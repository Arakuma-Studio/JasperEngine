// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SKYBOX_COMPONENT_H
#define SKYBOX_COMPONENT_H

#include "buffer/Buffer.h"
#include "texture/Texture.h"
#include "shader/ShaderUniforms.h"
#include "texture/Material.h"
#include <string>
#include <memory>

namespace jasperengine{

    struct JASPERENGINE_API SkyboxComponent{
        bool loaded = false;
        bool loadCalled = false;

        InterleavedBuffer buffer;

        Matrix4 skyViewProjectionMatrix;

        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        // optional custom shader: "library:<name>" or a project-relative fork base path
        // (no extension), empty = built-in. customShaderId is the resolved id for ShaderPool keys.
        std::string customShader;
        uint16_t customShaderId = 0;
        int slotVSParams = -1;
        int slotFSParams = -1;
        // values of the fork's custom uniform blocks, by member name
        ShaderUniformValues shaderUniforms;
        bool needUpdateShaderUniforms = false;
        CustomUniformBlock customVSParams;
        CustomUniformBlock customFSParams;
        // shader library: textures by @texture uniform name and enabled @feature/@keywords
        // defines of the shader above (library reference or fork)
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        std::vector<std::string> shaderFeatures;
        // resolved at load: the shader's manifest (null without one) and what it reads of
        // the frame block and custom textures
        std::shared_ptr<const ShaderManifest> shaderManifest;
        ShaderLibraryBindings libraryBindings;

        Texture texture;
        Vector4 color = Vector4(1.0, 1.0, 1.0, 1.0); //sRGB

        float rotation = 0;

        bool visible = true; // when false the sky is not drawn but still feeds IBL

        // IBL environment maps generated from texture (shared via TexturePool,
        // generation is expensive so maps are cached by sky texture id)
        std::shared_ptr<TextureRender> irradianceMap;
        std::shared_ptr<TextureRender> prefilteredMap;
        bool envMapsLoaded = false;
        bool needUpdateEnvironment = true;

        bool needUpdateTexture = false;
        bool needUpdateSky = true;
        bool needReload = false;
    };
    
}

#endif //SKYBOX_COMPONENT_H