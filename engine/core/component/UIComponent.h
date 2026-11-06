// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UI_COMPONENT_H
#define UI_COMPONENT_H

#include "texture/Texture.h"
#include "render/ObjectRender.h"
#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "shader/ShaderUniforms.h"
#include "texture/Material.h"
#include "math/AABB.h"
#include "ecs/Entity.h"
#include <string>
#include <memory>

namespace jasperengine{

    struct JASPERENGINE_API UIComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        // Render part
        bool loaded = false;
        bool loadCalled = false;

        InterleavedBuffer buffer;
        IndexBuffer indices;

        unsigned int minBufferCount = 0;
        unsigned int minIndicesCount = 0;

        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        uint64_t shaderProperties = 0;
        // optional custom shader: "library:<name>" or a project-relative fork base path
        // (no extension), empty = built-in. customShaderId is the resolved id for ShaderPool keys.
        ShaderRef customShader;
        uint16_t customShaderId = 0;
        int slotVSParams = -1;
        // reflected: a pixel-snapped variant's block ends with pixelSnapDraw (S41), a fork's may not
        unsigned int sizeVSParams = 0;
        int slotFSParams = -1;
        // a Text with outline or shadow: its SDF atlas and u_fs_textParams (UISystem fills
        // textParams: outline colour, shadow colour (linear), x outline, y shadow softness,
        // z shadow dilate (atlas pixels), w SDF distance scale, then the shadow offset in uv)
        bool textSdf = false;
        int slotFSTextParams = -1;
        Vector4 textParams[4];
        // the vertices carry each glyph's colour (a Text Effect): the draw takes only the alpha
        // of `color`, whose rgb the effect already composed in
        bool vertexColorAbsolute = false;
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

        PrimitiveType primitiveType = PrimitiveType::TRIANGLES;
        unsigned int vertexCount = 0;

        AABB aabb = AABB::ZERO;
        AABB worldAABB; // initially NULL

        Texture texture;
        Vector4 color = Vector4(1.0, 1.0, 1.0, 1.0); //linear color

        FunctionSubscribe<void()> onGetFocus;
        FunctionSubscribe<void()> onLostFocus;
        FunctionSubscribe<void(float, float)> onPointerEnter;
        FunctionSubscribe<void(float, float)> onPointerLeave;
        FunctionSubscribe<void(float, float)> onPointerMove;
        FunctionSubscribe<void(float, float)> onPointerDown;
        FunctionSubscribe<void(float, float)> onPointerUp;
        FunctionSubscribe<void(float, float)> onClick;
        FunctionSubscribe<void(float, float)> onDoubleClick;
        FunctionSubscribe<void(float, float)> onDragStart;
        FunctionSubscribe<void(float, float)> onDrag;
        FunctionSubscribe<void(float, float)> onDragEnd;

        bool automaticFlipY = true;
        bool flipY = false;

        bool pointerMoved = false;
        bool focused = false;

        bool needReload = false;

        bool needUpdateAABB = false;
        bool needUpdateBuffer = false;
        bool needUpdateTexture = false;

        // Canvas Group state over the element's chain (UISystem::syncCanvasGroups), runtime,
        // never serialized. groupAlpha multiplies the draw alpha (the chain's product: what a
        // Per Element draw uses); groupLayerAlpha is the same relative to the nearest enclosing
        // layered FLATTEN group (what the element draws with inside that group's layer, which
        // is composited at the group's alpha). color is never changed by a group
        float groupAlpha = 1.0f;
        float groupLayerAlpha = 1.0f;
        bool groupInteractable = true;
        bool groupBlocksRaycasts = true;
        // the nearest group of the chain (Inspector, MCP), NULL_ENTITY without one
        Entity groupEntity = NULL_ENTITY;
        // resolved at load (RenderSystem::loadUI): the blend the draw uses and whether it
        // writes the stencil (a mask still draws at alpha 0)
        MaterialBlendMode drawBlendMode = MaterialBlendMode::AUTO;
        bool writesStencil = false;
        // the pipeline types it was loaded with (a FLATTEN Canvas Group layer draws it with
        // PIP_RTT*; one loaded before needs a reload)
        uint16_t loadedPipelines = 0;
    };

}

#endif //UI_COMPONENT_H