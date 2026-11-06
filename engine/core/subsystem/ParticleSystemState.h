// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLE_SYSTEM_STATE_H
#define PARTICLE_SYSTEM_STATE_H

// The runtime of one ParticleSystemComponent (not saved, not copied): ParticleSubsystem's
// simulation (the emitter, its effect, culling, the frame's inputs and events) and
// RenderSystem's GPU objects for it.

#include "component/ParticleSystemComponent.h"
#include "particles/EmitterRuntime.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticlePreview.h"
#include "particles/ParticleWorld.h"
#include "render/BufferRender.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "shader/ShaderUniforms.h"
#include "texture/Texture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct ShaderManifest;

    namespace particles {

        // A mesh the Renderer draws (Mesh mode): its vertices (position, normal, uv) and
        // triangles on the GPU, made once per source
        struct ParticleMeshBuffers {
            BufferRender vertices;
            BufferRender indices;
            uint32_t indexCount = 0;
            bool created = false;
        };

        // RenderSystem's objects of one particle system (RenderSystemParticles.cpp)
        struct ParticleRenderData {
            bool loaded = false;
            // the effect's render settings changed: loaded again before its next draw
            bool needReload = true;
            // what the load was made from (the effect's Renderer, trails and texture sheet)
            uint64_t loadedKey = 0;

            std::shared_ptr<ShaderRender> shader;
            std::shared_ptr<ShaderRender> trailShader;
            std::shared_ptr<ShaderRender> meshShader;
            uint64_t properties = 0;
            uint64_t trailProperties = 0;
            uint64_t meshProperties = 0;
            uint16_t customShaderId = 0;
            uint16_t trailCustomShaderId = 0;
            std::shared_ptr<const ShaderManifest> manifest;
            std::shared_ptr<const ShaderManifest> trailManifest;

            // billboards (and stretched), mesh particles (one object per mesh) and trails
            ObjectRender quads;
            ObjectRender meshes[kMaxRendererMeshes];
            ObjectRender trails;
            ParticleMeshBuffers meshBuffers[kMaxRendererMeshes];
            uint32_t meshCount = 0;
            BufferRender trailIndices;
            uint32_t trailIndexCapacity = 0;   // the strips' points the index buffer covers
            uint32_t trailIndexPoints = 0;

            // uniform block slots of the three programs (quads/meshes share the shader's)
            struct Slots {
                int vsParams = -1;
                int fsParams = -1;
                int fsShadowData = -1;
                std::pair<int, int> shadowAtlasSlot = {-1, -1};
                ShaderLibraryBindings bindings;
                CustomUniformBlock customVS;
                CustomUniformBlock customFS;
            };
            Slots quadSlots;
            Slots meshSlots;
            Slots trailSlots;

            Texture texture;
            Texture trailTexture;
            Texture sixWayA;
            Texture sixWayB;
            Texture emission;
            std::vector<std::pair<std::string, Texture>> shaderTextures;
            ShaderUniformValues shaderUniforms;

            RenderState state;
            uint32_t sceneInputs = 0;
            int sortPriority = 0;

            // the sub-emitter children's, one per EmitterRuntime child (null where it has none):
            // empty, with nothing made, for a system without sub-emitters
            std::vector<std::unique_ptr<ParticleRenderData>> children;
        };

        struct ParticleSystemState {
            EmitterRuntime runtime;
            // the effect it runs (the asset or the inline document, the exposed values applied)
            std::shared_ptr<const CompiledEffect> effect;
            std::string effectPath;          // the asset it was read from (empty: inline)
            uint64_t effectVersion = 0;      // the asset's version it was compiled from
            // the sub-emitter files it was bound from (their paths and versions, down the tree),
            // and the library's generation when that was last checked
            uint64_t subEmitterSignature = 0;
            uint64_t subEmitterGeneration = 0;
            std::vector<Diagnostic> diagnostics;
            bool started = false;            // Play On Awake ran (or a script played it)
            // it was playing (or paused) after the last frame: its stop action runs when it is not
            bool running = false;
            uint32_t seed = 1;               // the seed of this play
            CullState cull;
            uint64_t visibleFrame = 0;       // the last frame a camera saw its bounds
            bool culled = false;
            // the world bounds of its particles last frame (for culling and the budget)
            float boundsMin[3] = {0, 0, 0};
            float boundsMax[3] = {0, 0, 0};
            bool hasBounds = false;
            float significance = 1.0f;

            // the frame's inputs, kept so the world's jobs read stable memory
            float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            float planes[kMaxCollisionPlanes][4] = {};
            uint32_t planeCount = 0;
            std::vector<ForceFieldSample> fields;

            // stats (get_particle_stats)
            uint32_t ticks = 0;
            uint32_t budget = 0;
            float emissionScale = 1.0f;
            double simulateMicros = 0.0;
            double renderMicros = 0.0;
            uint32_t sortedViews = 0;
            uint32_t drawnViews = 0;

            // the editor's preview scrubbing (seek with checkpoints, R29)
            std::unique_ptr<ParticlePreview> preview;

            ParticleRenderData render;
        };

    }

}

#endif // PARTICLE_SYSTEM_STATE_H
