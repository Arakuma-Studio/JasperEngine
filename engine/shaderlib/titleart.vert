#version 450

// Title Art's passes (docs/specs/S80-title-art-core.md §6.3, titleart.frag): a vertexless
// triangle over the whole target, drawn with draw(0, 3, 1) inside a viewport and scissor of the
// pass's region. Every program addresses its target by gl_FragCoord (memory rows on every
// backend, row 0 the first memory row), so no varying is needed and no pass ever flips.

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
