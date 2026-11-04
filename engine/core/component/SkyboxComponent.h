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
#include "buffer/InterleavedBuffer.h"
#include "io/watch/FileIdentity.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"

namespace jasperengine{

    // A scene has one sky, as a Unity scene has one skybox (RenderSettings.skybox): the first
    // SkyboxComponent (component array index 0: the order entities are created in, which
    // removal keeps) is drawn, lights meshes (IBL) and feeds reflection probes and SSR.
    // Any other is not loaded and costs nothing; it takes over when it becomes the first (the
    // first is removed; not when the first is hidden: visible=false draws no sky but keeps its
    // lighting), and the one it replaces unloads (RenderSystem::update). The renderer logs a
    // warning when it loads a sky while the scene has more than one. In the editor the array
    // follows the Hierarchy order (EntityRegistry::sortComponentsByEntityOrder after a move),
    // which is the scene file's, so the sky in use is the same live, after a reload and in an
    // export.
    struct JASPERENGINE_API SkyboxComponent{
        bool loaded = false;
        bool loadCalled = false;

        InterleavedBuffer buffer;

        Matrix4 skyViewProjectionMatrix;

        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        // optional custom shader: "library:<name>" or a project-relative fork base path
        // (no extension), empty = built-in. customShaderId is the resolved id for ShaderPool keys.
        ShaderRef customShader;
        uint16_t customShaderId = 0;
        // the variant's property bits (the post-processing colour, 'Pcl')
        uint64_t shaderProperties = 0;
        int slotVSParams = -1;
        int slotFSParams = -1;
        // values of the fork's custom uniform blocks, by member name
        ShaderUniformValues shaderUniforms;
        bool needUpdateShaderUniforms = false;
        // custom-block members that follow a global shader value (Shader.setGlobal*): member
        // -> global name, over shaderUniforms and the manifest default; globalsVersionSeen is
        // ShaderGlobals::getVersion() when the blocks were last written with them
        ShaderGlobalBindings shaderGlobalBindings;
        uint64_t globalsVersionSeen = 0;
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
        // the image the environment maps were made of, and its TextureDataPool::generation
        std::string environmentId;
        uint32_t environmentGeneration = 0;
        bool needUpdateEnvironment = true;

        bool needUpdateTexture = false;
        bool needUpdateSky = true;
        bool needReload = false;
    };
    
}

#endif //SKYBOX_COMPONENT_H