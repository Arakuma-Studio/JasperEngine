// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERUNIFORMS_H
#define SHADERUNIFORMS_H

#include "ShaderData.h"
#include "math/Vector4.h"
#include "pixelart/PaletteBinding.h"
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <memory>

namespace jasperengine::pixelart{
    struct PaletteSwapTextures;
}

namespace jasperengine{

    struct ShaderManifest;
    struct GlobalsBlockLayout;

    // values of a forked shader's custom uniform block, by member name
    // (a vector keeps the serialized order stable)
    typedef std::vector<std::pair<std::string, Vector4>> ShaderUniformValues;

    // the custom-block members of a material or component that follow a global shader value
    // (ShaderGlobals): member -> global name, in a stable order
    typedef std::vector<std::pair<std::string, std::string>> ShaderGlobalBindings;

    class JASPERENGINE_API ShaderUniforms{
    public:
        // "time", "gameTime" and "resolution", written by the engine every frame
        static bool isReserved(const std::string& name);
        // float/int scalars and vectors; matrices and arrays take no value
        static bool isEditable(const ShaderUniform& uniform);

        // false (and an error) for a reserved name
        static bool set(ShaderUniformValues& values, const std::string& name, const Vector4& value);
        static Vector4 get(const ShaderUniformValues& values, const std::string& name);
        static bool has(const ShaderUniformValues& values, const std::string& name);
        static bool remove(ShaderUniformValues& values, const std::string& name);
    };

    // a user-declared uniform block (u_vs_customParams, u_fs_customParams,
    // u_fs_postParams) resolved by name and packed as std140 bytes
    struct JASPERENGINE_API CustomUniformBlock{
        int slot = -1; // -1 when the shader does not declare the block
        std::vector<ShaderUniform> members; // short names
        std::vector<uint8_t> data; // rounded up to the vec4 stride
        int timeMember = -1;
        int gameTimeMember = -1;
        int resolutionMember = -1;
        bool mixedTypes = false; // int and float members together, which GL uploads wrong

        bool resolve(ShaderData& shaderData, const std::string& blockName);
        void clear();

        // every member from the value of the same name (zero when unset), reserved ones skipped
        void writeValues(const ShaderUniformValues& values);
        // the same, a member taking the first of: the global it is bound to (bindings, when
        // given: ShaderGlobals' current value, zero while the global does not exist), values,
        // fallbackValues (when given), the manifest's @property default (when given), zero
        void writeValues(const ShaderUniformValues& values, const ShaderUniformValues* fallbackValues, const ShaderManifest* manifest,
                         const ShaderGlobalBindings* bindings = nullptr);
        void writeTime(float seconds);
        // Engine::getShaderGameTime: the clock that holds while the game is paused
        void writeGameTime(float seconds);
        void writeResolution(float width, float height);

        static void writeUniform(std::vector<uint8_t>& block, const ShaderUniform& uniform, const Vector4& value);
        // an array member (std140: 16-byte stride), at most its declared count of elements;
        // each element takes the components its type has (UI effect palettes, phase 5)
        static void writeUniformArray(std::vector<uint8_t>& block, const ShaderUniform& uniform, const Vector4* values, size_t count);
    };

    // A custom texture (manifest @texture, or a shaderTextures entry of a shader without a
    // manifest) a loaded shader declares: its bind slot, resolved by name at load
    struct JASPERENGINE_API ShaderTextureSlot{
        std::pair<int, int> slot = {-1, -1};
        int source = -1;      // index in the object's shaderTextures, -1: the default texture
        int fallback = 0;     // ShaderTextureDefault
        bool cube = false;    // declared as a textureCube
    };

    // A manifest @palette a loaded shader reads (a feature of it is on): its two texture slots,
    // and the swap textures bound there, made from `bound` (the material's binding) at palette
    // library `generation` (a change of either binds again: hot reload, an edit)
    struct JASPERENGINE_API ShaderPaletteSlot{
        std::string uniform;
        std::pair<int, int> cells = {-1, -1};
        std::pair<int, int> rows = {-1, -1};
        std::shared_ptr<pixelart::PaletteSwapTextures> textures;
        pixelart::PaletteBinding bound;
        uint32_t generation = 0;
    };

    // a texture uniform of a loaded shader that a texture global (ShaderGlobals) binds to: one
    // the manifest and the object's shaderTextures do not claim, named like a global
    struct JASPERENGINE_API GlobalTextureSlot{
        std::pair<int, int> slot = {-1, -1};
        int globalId = 0;
        bool cube = false;
    };

    // What a loaded shader reads of the shader library's engine data: the frame block
    // (u_vs_frame / u_fs_frame, includes/frame.glsl), its custom textures and the scene
    // inputs (includes/scene.glsl: u_fs_sceneInputs, u_sceneDepthTexture,
    // u_sceneColorTexture). Slots are resolved once at load, so a draw does no name lookup;
    // -1 when not declared.
    struct JASPERENGINE_API ShaderLibraryBindings{
        int slotVSFrame = -1;
        int slotFSFrame = -1;
        unsigned int frameSize = 0;   // bytes of the vertex block the shader declares
        unsigned int frameSizeFS = 0; // of the fragment block (larger with the post colour)
        // the post-processing colour (phase 4, includes/post_color.glsl): its own block when
        // the shader has no frame or Lit block to hold it, and its LUT
        int slotFSPostColor = -1;
        std::pair<int, int> slotPostColorLut = {-1, -1};
        // how the draw's blend takes that colour (RenderSystem's PostColorBlend: which pass
        // block it uploads), for a shader without a manifest (no BLEND_* define); set by the
        // load after the render state is resolved, 0 (its own colour) otherwise
        uint8_t postColorBlend = 0;
        std::vector<ShaderTextureSlot> textures;
        std::vector<ShaderPaletteSlot> palettes;
        int slotFSSceneInputs = -1;
        unsigned int sceneInputsSize = 0;
        std::pair<int, int> slotSceneDepth = {-1, -1};
        std::pair<int, int> slotSceneColor = {-1, -1};
        // the reflection environment (includes/environment.glsl, @require environment)
        int slotFSEnvironment = -1;
        std::pair<int, int> slotEnvironment = {-1, -1};
        // the frame's blurred scene (phase 4, includes/blurred_scene.glsl: frosted UI panels)
        int slotFSBlurredScene = -1;
        std::pair<int, int> slotBlurredScene = {-1, -1};
        // the height fog of the pass (includes/height_fog.glsl, the fogged mesh variants)
        int slotFSHeightFog = -1;
        // the global shader values (ShaderGlobals: u_vs_globals / u_fs_globals and texture
        // globals), packed once per change and shared by the programs with the same block
        int slotVSGlobals = -1;
        int slotFSGlobals = -1;
        std::shared_ptr<GlobalsBlockLayout> vsGlobals;
        std::shared_ptr<GlobalsBlockLayout> fsGlobals;
        std::vector<GlobalTextureSlot> globalTextures;

        void clear(){
            slotVSFrame = -1;
            slotFSFrame = -1;
            frameSize = 0;
            frameSizeFS = 0;
            slotFSPostColor = -1;
            slotPostColorLut = {-1, -1};
            postColorBlend = 0;
            textures.clear();
            palettes.clear();
            slotFSSceneInputs = -1;
            sceneInputsSize = 0;
            slotSceneDepth = {-1, -1};
            slotSceneColor = {-1, -1};
            slotFSEnvironment = -1;
            slotEnvironment = {-1, -1};
            slotFSBlurredScene = -1;
            slotBlurredScene = {-1, -1};
            slotFSHeightFog = -1;
            slotVSGlobals = -1;
            slotFSGlobals = -1;
            vsGlobals.reset();
            fsGlobals.reset();
            globalTextures.clear();
        }
        // the shader declares a scene input (its draws get the pass's, RenderSystem::applySceneInputs)
        bool readsScene() const{
            return slotFSSceneInputs != -1 || slotSceneDepth.first != -1 || slotSceneColor.first != -1;
        }
    };

}

#endif //SHADERUNIFORMS_H
