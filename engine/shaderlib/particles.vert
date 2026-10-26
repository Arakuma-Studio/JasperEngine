#version 450

// Particle systems (docs/specs/S20-particles.md): billboards, stretched billboards, meshes and
// trails, per instance from the 32-byte particle record. Library particles shaders share this
// vertex stage (includes/particles_vs.glsl).

#include "includes/particles_vs.glsl"
