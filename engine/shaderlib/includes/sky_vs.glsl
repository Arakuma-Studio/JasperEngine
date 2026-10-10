// Shared vertex path of library sky shaders (@shader sky): a <name>.vert includes only
// this. The engine draws a unit cube around the camera with the sky's view-projection
// (camera rotation, the Skybox's rotation, the projection); the cube sits on the far plane.
//
// One output, which the fragment shader reads (normalising per pixel):
//   v_direction       the view direction in the sky's own space, the Skybox's rotation
//                     applied (what the built-in cube map samples with)
//   v_worldDirection  instead, with SKY_WORLD_DIRECTION (defined in both stages): the
//                     view direction in world space, the space of the frame block's main
//                     light; the Skybox's rotation does not apply to it

uniform u_vs_skyParams {
    mat4 vpMatrix;
} skyParams;

in vec3 a_position;

#ifndef SKY_WORLD_DIRECTION
    out vec3 v_direction;
#else
    #ifndef FRAME_VS
        #define FRAME_VS
    #endif
    #include "includes/frame.glsl"

    out vec3 v_worldDirection;
#endif

void main(){
    vec4 clip = skyParams.vpMatrix * vec4(a_position, 1.0);

    #ifndef SKY_WORLD_DIRECTION
        v_direction = a_position;
    #else
        // back through the camera's projection (flipped like the sky's for offscreen
        // targets) to view space, then by the camera's rotation to world space; one
        // inverse per vertex of the 36
        vec4 viewPosition = inverse(frame.projectionMatrix) * clip;
        v_worldDirection = transpose(mat3(frame.viewMatrix)) * (viewPosition.xyz / viewPosition.w);
    #endif

    gl_Position = clip.xyww;
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
