// S100 R2 (CavityGpu.ForkWithACommentBeforeVersionKeepsItsDepthProgram): a project's depth fork
// that starts with a comment, as GLSL allows before #version, and cuts its mesh out of every
// depth pass (no shadow, nothing in a depth pre-pass), whatever the colour pass draws.

#version 450

out vec4 frag_color;
in vec2 v_projZW;

void main() {
    if (v_projZW.y > -1.0e30)
        discard;
    frag_color = vec4(1.0);
}
