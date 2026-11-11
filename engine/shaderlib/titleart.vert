#version 450

// Title Art's passes (ShaderType::TITLE_ART, titleart.frag): a vertexless triangle drawn with
// draw(0, 3, 1) over the whole target. Every pass addresses its target by gl_FragCoord in memory
// rows (row 0 is the first memory row, the top row of the PNG after readback, on every backend:
// docs/specs/S80-title-art-core.md §6.3), so no pass needs a varying and none ever flips.

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
