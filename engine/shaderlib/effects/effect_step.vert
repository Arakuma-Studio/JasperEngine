#version 450

// HTML UI effects, step form (phase 6): a vertexless fullscreen triangle drawn with
// draw(0, 3, 1) over a state image of a stateful effect. The fragment stage addresses the
// target by gl_FragCoord (the texel, in memory rows on every backend), so there is no varying.
void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
