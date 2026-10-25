#version 450

// The Frame Debugger's pass output preview (editor, spec S15 R12): one attachment of a pass,
// copied into the preview target (RGBA8) right after the pass ended (SokolPassPreview). The
// source is read texel-exact (unfilterable float: any colour format and depth alike) at the
// pass's mip level, and its slice or cube face. Modes: 0 colour, 1 alpha, 2 depth, 3 red,
// 4 green, 5 blue. Depth is linearised with the camera's near/far when known, else mapped from
// [range.z, range.w] to black..white.

in vec2 v_texcoord;
out vec4 frag_color;

// @image_sample_type u_previewSource unfilterable_float
// @sampler_type u_previewSource_smp nonfiltering
#if defined(PREVIEW_ARRAY)
uniform texture2DArray u_previewSource;
#elif defined(PREVIEW_CUBE)
uniform textureCube u_previewSource;
#elif defined(PREVIEW_3D)
uniform texture3D u_previewSource;
#else
uniform texture2D u_previewSource;
#endif
uniform sampler u_previewSource_smp;

uniform u_fs_passPreview {
    vec4 params;   // x mode, y slice (array layer, cube face, 3D depth 0..1), z flip the source vertically, w linear depth
    vec4 range;    // x near, y far (linear depth); z, w the depth mapped to black and white
} preview;

vec4 sampleSource(vec2 uv){
#if defined(PREVIEW_ARRAY)
    return textureLod(sampler2DArray(u_previewSource, u_previewSource_smp), vec3(uv, preview.params.y), 0.0);
#elif defined(PREVIEW_CUBE)
    // the face's texel at uv, as the cube map's direction for that face
    vec2 c = uv * 2.0 - 1.0;
    int face = int(preview.params.y + 0.5);
    vec3 dir;
    if (face == 0) dir = vec3(1.0, -c.y, -c.x);
    else if (face == 1) dir = vec3(-1.0, -c.y, c.x);
    else if (face == 2) dir = vec3(c.x, 1.0, c.y);
    else if (face == 3) dir = vec3(c.x, -1.0, -c.y);
    else if (face == 4) dir = vec3(c.x, -c.y, 1.0);
    else dir = vec3(-c.x, -c.y, -1.0);
    return textureLod(samplerCube(u_previewSource, u_previewSource_smp), dir, 0.0);
#elif defined(PREVIEW_3D)
    return textureLod(sampler3D(u_previewSource, u_previewSource_smp), vec3(uv, preview.params.y), 0.0);
#else
    return textureLod(sampler2D(u_previewSource, u_previewSource_smp), uv, 0.0);
#endif
}

void main(){
    vec2 uv = v_texcoord;
    // GL's offscreen targets are rendered flipped (the source and this target alike)
    if (preview.params.z > 0.5)
        uv.y = 1.0 - uv.y;
    vec4 texel = sampleSource(uv);
    int mode = int(preview.params.x + 0.5);
    if (mode == 1){
        frag_color = vec4(vec3(texel.a), 1.0);
    }else if (mode == 2){
        float depth = texel.r;
        float value;
        if (preview.params.w > 0.5){
            // perspective depth [0, 1] to view distance, shown over near..far
            float n = preview.range.x;
            float f = preview.range.y;
            float linear = (n * f) / max(f - depth * (f - n), 1e-6);
            value = clamp((linear - n) / max(f - n, 1e-6), 0.0, 1.0);
        }else{
            value = clamp((depth - preview.range.z) / max(preview.range.w - preview.range.z, 1e-6), 0.0, 1.0);
        }
        frag_color = vec4(vec3(value), 1.0);
    }else if (mode == 3){
        frag_color = vec4(texel.r, 0.0, 0.0, 1.0);
    }else if (mode == 4){
        frag_color = vec4(0.0, texel.g, 0.0, 1.0);
    }else if (mode == 5){
        frag_color = vec4(0.0, 0.0, texel.b, 1.0);
    }else{
        frag_color = vec4(texel.rgb, 1.0);
    }
}
