#version 450

// @shader postprocess
// @name "CRT"
// @category "Post-processing"
// @description "An old tube screen: barrel curvature, scanlines, an aperture grille, colour fringing and dark corners. A user post-process pass (the engine's post-processing has a cheaper CRT in its uber pass)."
// @property curvature range(0, 1) 0.3 "Curvature"
// @property lines float 240 "Scanlines over the height"
// @property scanlines range(0, 1) 0.5 "Scanline darkness"
// @property mask range(0, 1) 0.3 "Aperture grille"
// @property fringe range(0, 4) 1 "Colour fringing (pixels)"
// @property vignette range(0, 1) 0.4 "Dark corners"

// The tube bends the picture outwards (each axis by the square of the other), so the image
// shrinks into a rounded screen with black around it; scanlines darken between rows, the
// grille favours one primary per column of output pixels, red and blue sit apart along the
// radius. Display colour in and out.

// the defaults Vulkan GLSL has anyway, stated: glslang warns about implicit ones in a shader
// using precision qualifiers (the mediump below reaches GLES as RelaxedPrecision)
precision highp float;
precision highp int;

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_postParams {
    vec4 resolution; // engine-written: xy output size, zw 1 / size
    float curvature;
    float lines;
    float scanlines;
    float mask;
    float fringe;
    float vignette;
} postParams;

void main(){
    highp vec2 p = v_texcoord * 2.0 - 1.0;
    p *= 1.0 + postParams.curvature * 0.3 * (p.yx * p.yx);
    highp vec2 uv = p * 0.5 + 0.5;
    mediump float inside = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);

    highp vec2 shift = (uv - 0.5) * postParams.fringe * postParams.resolution.zw * 2.0;
    mediump vec3 color;
    color.r = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv - shift).r;
    color.g = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).g;
    color.b = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + shift).b;

    // a dark gap between scanlines (in the bent picture's rows)
    mediump float row = 0.5 + 0.5 * cos(6.2831853 * uv.y * max(postParams.lines, 1.0));
    color *= 1.0 - postParams.scanlines * (1.0 - row) * 0.8;

    // the aperture grille: each output column favours one primary
    mediump float column = mod(floor(gl_FragCoord.x), 3.0);
    mediump vec3 grille = vec3(column < 0.5 ? 1.0 : 0.65, (column > 0.5 && column < 1.5) ? 1.0 : 0.65, column > 1.5 ? 1.0 : 0.65);
    color *= mix(vec3(1.0), grille * 1.2, postParams.mask);

    // the tube's corners
    mediump vec2 t = clamp(uv, 0.0, 1.0);
    mediump float corners = pow(clamp(16.0 * t.x * (1.0 - t.x) * t.y * (1.0 - t.y), 0.0, 1.0), 0.3);
    color *= mix(1.0, corners, postParams.vignette) * inside;

    frag_color = vec4(color, 1.0);
}
