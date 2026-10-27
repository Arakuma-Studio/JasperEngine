#version 450

// The fluid simulation's passes (FluidSimulator, fluid.frag): a vertexless triangle drawn with
// draw(0, 3, 1) over the whole target. The solver and look passes address their target by
// gl_FragCoord (memory rows on every backend), so they need no varying; the screen composite
// (FLUID_LOOK) takes the clip position to map the output onto the view.

#ifdef FLUID_LOOK
    out vec2 v_clip;
#endif

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifdef FLUID_LOOK
        v_clip = gl_Position.xy;
    #endif
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
