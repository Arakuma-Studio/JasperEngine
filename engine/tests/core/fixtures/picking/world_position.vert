#version 450

// S26 AC5 (MeshPose.MatchesGpuSkinning): the debug variant of the skinned mesh vertex shader.
// It skins and morphs as every library mesh shader does, through the same includes
// (skinning.glsl, morphtarget.glsl, as includes/surface_vs.glsl calls them), and draws each
// vertex as a point in a cell of its own (16 x 16 cells over the target, vertex i in cell i,
// a little off the cell's centre so it never lands on a pixel edge), carrying its world
// position and its index to the fragment stage (world_position_<x|y|z|index>.frag).

uniform u_vs_pbrParams {
    mat4 modelMatrix;
    mat4 normalMatrix;
    mat4 mvpMatrix;
} pbrParams;

in vec3 a_position;

out vec3 v_position;
out float v_vertex;

#include "includes/skinning.glsl"
#include "includes/morphtarget.glsl"

void main() {
    mat4 boneTransform = getBoneTransform();
    vec3 position = getMorphPosition(a_position);
    position = getSkinPosition(position, boneTransform);
    vec4 worldPos = pbrParams.modelMatrix * vec4(position, 1.0);
    v_position = worldPos.xyz / worldPos.w;
    v_vertex = float(gl_VertexIndex);

    const float CELLS = 16.0;
    vec2 cell = vec2(mod(v_vertex, CELLS), floor(v_vertex / CELLS));
    // at the near end of the depth range, in front of anything a depth pass drew there
    #ifdef IS_VULKAN
        const float NEAR_DEPTH = 0.0005;
    #else
        const float NEAR_DEPTH = -0.999;
    #endif
    gl_Position = vec4((cell + 0.5) / CELLS * 2.0 - 1.0 + 0.001, NEAR_DEPTH, 1.0);
    gl_PointSize = 1.0;
}
