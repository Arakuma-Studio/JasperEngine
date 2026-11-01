// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// origin/main's version 2 Particle Systems (its Unity-shaped modules, "particlesystem:
// {version: 2, main: {...}, ...}" in a scene or prefab) read into S20 effects
// (docs/specs/S20-particles.md, Upstream absorption). The modules are the same model; what
// differs is converted: rotations and angular speeds in radians become degrees, linear colours
// sRGB, a random range's order (min, max), the shape's Euler order (X then Y then Z there, Z
// then X then Y here), the shape types (Cone Volume, Box Shell and Box Edge are emit-from modes
// of Cone and Box), the render alignment, and the defaults that differ (the Texture Sheet's 30
// frames per second, Limit Velocity's drag by velocity). The particles drew through the
// GameObject's quad MeshComponent (its material: the texture, base colour, tiling, soft
// particles, blend) or a Points component: the Renderer module takes those over.

#include "math/Vector3.h"
#include "particles/ParticleEffect.h"
#include "texture/Material.h"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::upstreamparticles {

    // a particlesystem node written by origin/main's version 2 systems
    bool isVersion2(const YAML::Node& system);

    // What the GameObject that drew the system (its action's target) held for its particles
    struct Target {
        enum class Kind { None, Mesh, Points };
        Kind kind = Kind::None;
        // Mesh: the MeshComponent's first material and its switches
        Material material;
        bool meshEnabled = true;
        bool castShadows = false;
        bool receiveShadows = false;
        // Points: its texture, shader and blend
        std::string texture;
        std::string textureGuid;
        std::string pointsShader;
        std::vector<std::string> pointsFeatures;
        std::vector<particles::ShaderProperty> pointsProperties;
        std::vector<particles::ShaderTexture> pointsTextures;
        int pointsBlend = -1;          // particles::ParticleBlend, -1 the default (alpha)
        bool pointsOpaque = false;
        int pointsDepthWrite = -1;     // -1 auto, 0 off, 1 on
        // the sprite frames Texture Sheet Animation's Sprites mode picks by id (the target's
        // SpriteComponent or Points frames), in texture UVs
        std::vector<particles::SpriteRect> frames;
    };

    // a GameObject's components as a target (Kind::None when it has neither a mesh nor points)
    Target targetOf(const YAML::Node& components);

    struct Result {
        particles::ParticleEffectDocument effect;
        bool playOnAwake = true;
        uint32_t seed = 0;       // 0: a new seed at every play (Auto Random Seed)
        // Mesh render mode: the particles draw the GameObject's own MeshComponent (kept, disabled)
        bool drawsTargetMesh = false;
        std::vector<std::string> notes;
    };

    // the system's effect and the component's own values
    Result convert(const YAML::Node& system, const Target& target);

    // The shape rotation of origin/main (Euler degrees applied X, then Y, then Z) as the S20
    // shape's (Z, then X, then Y): the same turn
    Vector3 shapeRotation(const Vector3& xyzDegrees);

    // origin/main's (and Unity's) render mode (Billboard 0, Stretch 1, Horizontal 2, Vertical 3,
    // Mesh 4) and alignment (View 0, World 1, Local 2, Facing 3, Velocity 4) on the Renderer:
    // a billboard's alignment is a render mode here, a mesh's the mesh alignment; false when the
    // alignment of a stretched, horizontal or vertical billboard had to be left out
    bool applyRenderMode(int renderMode, int alignment, bool localSimulation, particles::RendererModule& renderer);

    // a mesh name of origin/main's renderer ("quad", "cube", "sphere", "cylinder", "capsule",
    // "plane") as a primitive; another name is a model path (`primitive` false)
    particles::MeshSource meshSourceOf(const std::string& name, bool& primitive);

    // a Material drawn by origin/main's particle quads (library:unlit with PARTICLE_TEXTURE: its
    // base map, base colour, tiling, soft particles and blend) on the Renderer module; what it
    // cannot carry is noted
    void applyMaterial(const Material& material, particles::RendererModule& renderer, std::vector<std::string>& notes);

}
