#version 450

// The volume ratio of the lattices deforming the mesh reaches the fragment stage
// (v_latticeStretch, includes/lattice.glsl); the vertex colour stays out of v_color (the
// instance colour still applies)
#define LATTICE_STRETCH
#define SURFACE_NO_VERTEX_COLOR

#include "includes/surface_vs.glsl"
