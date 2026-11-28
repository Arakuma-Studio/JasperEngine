// The editor's Vertex Color Draw Mode (docs/specs/V1-vertex-colors.md R102, D8): a mesh variant's
// view of its raw vertex colour. ShaderVariantCompiler wraps both mains of a variant with the
// 'Vcv' bit and includes this after them: the vertex stage's main runs, then hands its vertex
// colour on (VERTEX_COLOR_VIEW_VS); the fragment stage draws that colour unlit
// (VERTEX_COLOR_VIEW_FS). Not the instance colour: the colour the mesh stores. A variant reading
// no vertex colour (none on the mesh, or a library shader without @require color) shows white.
//
// VERTEX_COLOR_VIEW_CHANNEL: -1 the colour (as a material tints with it: display encoded), 0-3 one
// of R, G, B, A as grey, its value as it is (0.5 is mid grey: the number a mask holds). A is
// drawn over a screen-space checker, which shows where it is 0.

#ifndef VERTEX_COLOR_VIEW_GLSL
#define VERTEX_COLOR_VIEW_GLSL

#ifdef VERTEX_COLOR_VIEW_VS
    out vec4 v_vertexColorView;

    // (a_color only where the stage declared it: mesh.vert and includes/surface_vs.glsl say so)
    void vertexColorViewWrite(){
        #if defined(VERTEX_COLOR_ATTRIBUTE) && defined(HAS_VERTEX_COLOR_VEC3)
            v_vertexColorView = vec4(a_color, 1.0);
        #elif defined(VERTEX_COLOR_ATTRIBUTE) && defined(HAS_VERTEX_COLOR_VEC4)
            v_vertexColorView = a_color;
        #else
            v_vertexColorView = vec4(1.0);
        #endif
    }
#endif

#ifdef VERTEX_COLOR_VIEW_FS
    #include "includes/srgb.glsl"

    in vec4 v_vertexColorView;

    vec4 vertexColorViewColor(){
        vec4 color = clamp(v_vertexColorView, 0.0, 1.0);
        #if VERTEX_COLOR_VIEW_CHANNEL < 0
            return vec4(linearTosRGB(color.rgb), 1.0);
        #elif VERTEX_COLOR_VIEW_CHANNEL < 3
            return vec4(vec3(color[VERTEX_COLOR_VIEW_CHANNEL]), 1.0);
        #else
            // 8 px squares of two greys, under white at the alpha
            vec2 cell = floor(gl_FragCoord.xy / 8.0);
            float checker = mod(cell.x + cell.y, 2.0) < 0.5 ? 0.2 : 0.35;
            return vec4(vec3(mix(checker, 1.0, color.a)), 1.0);
        #endif
    }
#endif

#endif
