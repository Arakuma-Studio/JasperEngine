#version 450

// HTML UI fullscreen passes (layer composite, filters, masks): a vertexless triangle drawn
// with draw(0, 3, 1), in the fullscreen.vert convention, so texture coordinate (0,0) is the
// image's first row on every backend. uvTransform selects the source rectangle.
uniform u_vs_htmluiPost {
    vec4 uvTransform;   // xy: scale, zw: offset
} htmluiPost;

out vec2 v_uv;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifndef IS_GLSL
        // only GL stores destination row 0 at clip y = -1
        uv.y = 1.0 - uv.y;
    #endif
    v_uv = uv * htmluiPost.uvTransform.xy + htmluiPost.uvTransform.zw;
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
