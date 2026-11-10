#version 450
precision highp float;
precision highp int;

// Title Art's generator programs (ShaderType::TITLE_ART with the 'Gen' bit, docs/specs/
// S83-title-effects-generators.md R11): the fullscreen triangle of every pass (titleart.vert's),
// or QuadBatch's quads and meshes pulled from the RGBA32F data texture u_taVertexData by
// gl_VertexIndex (tfx_quads.glsl), which needs neither instanced vertex buffers nor storage
// buffers on any backend.

#include "titleart/gen/programs.glsl"

#if TA_GEN_VERTICES == 0

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}

#else

#include "titleart/gen/tfx_quads.glsl"

#endif
