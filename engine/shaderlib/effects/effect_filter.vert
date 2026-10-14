#version 450

// HTML UI effects, filter form (phase 5): a vertexless fullscreen triangle drawn with
// draw(0, 3, 1) over the filter chain's targets, in the htmlui_post.vert convention:
// v_texcoord (0,0) is the target's first row (the top of the layer) on every backend.
out vec2 v_texcoord;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifndef IS_GLSL
        // only GL stores destination row 0 at clip y = -1
        uv.y = 1.0 - uv.y;
    #endif
    v_texcoord = uv;
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
