// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POINTS_COMPONENT_H
#define POINTS_COMPONENT_H

#include "util/SpriteFrameData.h"
#include "util/GrowArray.h"
#include "buffer/ExternalBuffer.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "texture/Texture.h"
#include "shader/ShaderUniforms.h"
#include "texture/Material.h"
#include "Engine.h"
#include <string>
#include <memory>

namespace jasperengine{

    struct PointData{
        Vector3 position = Vector3(0.0, 0.0, 0.0);
        Vector4 color = Vector4(1.0, 1.0, 1.0, 1.0);
        float size = 1; // world-space diameter (matches mesh extent at unit scale)
        float rotation = 0;
        Rect textureRect = Rect(0.0, 0.0, 1.0, 1.0);
        bool visible = true;
    };

    struct PointRenderData{
        Vector3 position;
        Vector4 color;
        float size;
        float rotation;
        Rect textureRect;
    };

    struct JASPERENGINE_API PointsComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        bool loaded = false;
        bool loadCalled = false;

        ExternalBuffer buffer;

        std::vector<PointData> points;
        std::vector<PointRenderData> renderPoints; //must be sorted

        unsigned int numFramesRect = 0;
        GrowArray<SpriteFrameData> framesRect;
        // what its arrays hold outside the component (ComponentArray::arrayHeapBytes)
        size_t arrayHeapBytes() const { return framesRect.storageBytes(); }

        unsigned int maxPoints = 100;
        unsigned int numVisible = 0;

        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        uint64_t shaderProperties = 0;
        // optional custom shader: "library:<name>" or a project-relative fork base path
        // (no extension), empty = built-in. customShaderId is the resolved id for ShaderPool keys.
        ShaderRef customShader;
        uint16_t customShaderId = 0;
        int slotVSParams = -1;
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
        // what its shader reads of the scene (ShaderRequire_SceneDepth / _SceneColor)
        uint32_t sceneInputs = 0;
        bool sceneInputsPredicted = false; // see MeshComponent::sceneInputsPredicted
        // its shader's @state priority=, the transparent queue order (points have no sortPriority)
        int sortPriority = 0;
        // added to its camera distance in the transparent queue (a version 2 Particle System's
        // renderer sortingFudge, written by it; runtime): lower draws on top, Unity
        float sortingFudge = 0.0f;
        // a version 2 Particle System's renderer sortPriority, added to sortPriority when queued (runtime)
        int particleSortPriority = 0;

        Texture texture;

        // authored (saved)
        bool transparent = false;
        bool autoTransparency = true;
        // set at load with autoTransparency: the texture has alpha. Runtime only, never saved, so
        // a scene saves the same whether or not it has loaded yet
        bool derivedTransparent = false;
        // drawn in the transparent queue
        bool isTransparent() const { return transparent || (autoTransparency && derivedTransparent); }

        bool hasTextureRect = false;

        // UI particles (Points under a RectTransform): the Canvas Group alpha over the chain,
        // multiplied into each render point (UISystem::syncCanvasGroups), runtime
        float uiGroupAlpha = 1.0f;
        // uiGroupAlpha changed after the render update (a write in onPostUpdate, synced by the
        // UI draw): RenderSystem::draw rebuilds the render points before this frame's passes
        bool needUpdateGroupAlpha = false;
        // the pipeline types it was loaded with (see UIComponent::loadedPipelines)
        uint16_t loadedPipelines = 0;

        bool needUpdate = true;
        bool needUpdateBuffer = false;
        bool needUpdateTexture = false;
        bool needReload = false;
    };
    
}

#endif //POINTS_COMPONENT_H