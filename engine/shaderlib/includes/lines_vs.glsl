// Shared vertex path of the lines shaders (@shader lines: Line Renderer and Trail Renderer):
// the built-in lines.vert and a library <name>.vert include only this.
//
// Hairline (no LINE_RIBBON): the 1-pixel LINES primitive of a Line Renderer without a width,
// a_position times the MVP matrix.
//
// LINE_RIBBON ('Rib', a width > 0): a triangle list built on the CPU from the centre line
// (engine/core/render/LineRibbonBuilder.h) and turned to the camera here. Each vertex has
// its centre point (a_position), the unit directions of the line in and out of it
// (a_tangentIn, a_tangentOut) and a_lineParams: x its side (-1/+1; a cap vertex: -1 at the
// start, +1 at the end), y its half width (world units, or pixels with LINE_PIXEL_WIDTH),
// z its role (0 in, 1 corner fan, 2 cap fan, 3 out), w the fan parameter (0..1; on an in or out
// vertex 1 when its joint is round). The ribbon faces the camera (View: the normal towards
// it) or lies across the transform's Z (TransformZ). At a turn the outer side spreads into a
// miter (up to 4x the half width, else a bevel) or a round joint; every inner vertex goes to
// the inner miter point, so its triangles have no area. Widths are applied in world space
// after the model matrix: a scaled transform moves the points, not the width (Unity).
// LINE_PIXEL_WIDTH ('Pxw'): the same, built in screen pixels of the render target.
//
// Outputs, by the variant's defines (both stages see them):
//   v_color   HAS_VERTEX_COLOR_VEC3 / VEC4: the vertex colour (linear)
//   v_uv1     LINE_RIBBON: u along the line by the texture mode, v 0 on the -1 side, 1 on the +1
// Varyings pair up by location, in declaration order: includes/lines_fs.glsl declares them
// in this order; a shader adding its own declares them after both.

#ifdef LINE_RIBBON
uniform u_vs_linesParams {
    mat4 mvpMatrix;          // the hairline's member, first (its layout is kept)
    mat4 modelMatrix;        // identity for world-space points
    mat4 viewProjection;     // with the render target's Y flip
    vec4 cameraPosition;     // xyz world position (w 0), or the direction towards the camera (w 1, orthographic)
    vec4 alignAxis;          // xyz the world normal of TransformZ alignment, w 1 TransformZ / 0 View
    vec4 viewport;           // xy render target size in pixels
} linesParams;
#else
uniform u_vs_linesParams {
    mat4 mvpMatrix;
} linesParams;
#endif

in vec3 a_position;

#ifdef HAS_VERTEX_COLOR_VEC3
    in vec3 a_color;
    out vec3 v_color;
#endif

#ifdef HAS_VERTEX_COLOR_VEC4
    in vec4 a_color;
    out vec4 v_color;
#endif

#ifdef LINE_RIBBON
    in vec2 a_texcoord1;
    in vec3 a_tangentIn;
    in vec3 a_tangentOut;
    in vec4 a_lineParams;
    out vec2 v_uv1;

    const float LINE_PI = 3.14159265358979;
    // the miter joint's limit in half widths, past it a bevel
    const float LINE_MITER_LIMIT = 4.0;

    // the unit perpendicular of a direction in the plane facing n (any one when they are
    // parallel: the camera looks along the line, which then shows its width only)
    vec3 linePerpendicular(vec3 tangent, vec3 n){
        vec3 s = cross(tangent, n);
        float len = length(s);
        if (len < 1e-5){
            vec3 other = abs(tangent.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            s = cross(tangent, other);
            len = length(s);
        }
        return s / max(len, 1e-8);
    }

    // The offset of a vertex from its centre point in half widths, in the plane facing n
    vec3 lineOffset(vec3 tin, vec3 tout, vec3 n, vec4 params){
        float side = params.x;
        float role = params.z;
        float w = params.w;
        vec3 sin_ = linePerpendicular(tin, n);
        vec3 sout = linePerpendicular(tout, n);

        if (role > 1.5 && role < 2.5){
            // cap fan: from the +1 side (w 0) round the end to the -1 side (w 1)
            float a = LINE_PI * w;
            // (cross(n, s) is the line's direction in the facing plane)
            if (side < 0.0){
                return cos(a) * sin_ - sin(a) * normalize(cross(n, sin_));
            }
            return cos(a) * sout + sin(a) * normalize(cross(n, sout));
        }

        float turn = dot(cross(tin, tout), n);
        bool level = abs(turn) < 1e-4;
        bool straight = level && dot(tin, tout) > 0.0;
        // a U-turn: both sides go round the front
        bool uturn = level && !straight;
        bool outer = uturn || side * turn > 0.0;

        vec3 sum = sin_ + sout;
        float sumLength = length(sum);
        vec3 miterDirection = sumLength > 1e-4 ? sum / sumLength : sin_;
        float miter = 1.0 / max(dot(miterDirection, sin_), 1e-4);

        if (straight || !outer){
            return miterDirection * min(miter, LINE_MITER_LIMIT) * side;
        }
        bool roundJoint = (role > 0.5 && role < 1.5) || w > 0.5;
        if (!roundJoint && !uturn && miter <= LINE_MITER_LIMIT){
            return miterDirection * miter * side;
        }
        if (role < 0.5){
            return sin_ * side;
        }
        if (role > 2.5){
            return sout * side;
        }
        // corner fan: round from the in side through the front to the out side
        vec3 from = sin_ * side;
        vec3 to = sout * side;
        vec3 front = normalize(cross(n, sin_));
        float angle = acos(clamp(dot(from, to), -1.0, 1.0)) * w;
        return cos(angle) * from + sin(angle) * front;
    }
#endif

void main() {

    #if defined(HAS_VERTEX_COLOR_VEC3) || defined(HAS_VERTEX_COLOR_VEC4)
        v_color = a_color;
    #endif

    #ifdef LINE_RIBBON
        v_uv1 = a_texcoord1;

        vec3 center = (linesParams.modelMatrix * vec4(a_position, 1.0)).xyz;
        mat3 model3 = mat3(linesParams.modelMatrix);
        vec3 tin = normalize(model3 * a_tangentIn);
        vec3 tout = normalize(model3 * a_tangentOut);
        float halfWidth = a_lineParams.y;

        #ifdef LINE_PIXEL_WIDTH
            // in screen pixels: the directions are the screen-space derivatives of the line
            vec4 clip = linesParams.viewProjection * vec4(center, 1.0);
            vec4 clipIn = linesParams.viewProjection * vec4(tin, 0.0);
            vec4 clipOut = linesParams.viewProjection * vec4(tout, 0.0);
            float clipW = abs(clip.w) > 1e-6 ? clip.w : 1e-6;
            vec2 screenIn = (clipIn.xy * clipW - clip.xy * clipIn.w) * linesParams.viewport.xy;
            vec2 screenOut = (clipOut.xy * clipW - clip.xy * clipOut.w) * linesParams.viewport.xy;
            screenIn = length(screenIn) > 1e-12 ? normalize(screenIn) : vec2(1.0, 0.0);
            screenOut = length(screenOut) > 1e-12 ? normalize(screenOut) : screenIn;
            vec3 offset = lineOffset(vec3(screenIn, 0.0), vec3(screenOut, 0.0), vec3(0.0, 0.0, 1.0), a_lineParams);
            gl_Position = clip + vec4(offset.xy * halfWidth * 2.0 / linesParams.viewport.xy * clip.w, 0.0, 0.0);
        #else
            vec3 n;
            if (linesParams.alignAxis.w > 0.5){
                n = normalize(linesParams.alignAxis.xyz);
            }else if (linesParams.cameraPosition.w > 0.5){
                n = normalize(linesParams.cameraPosition.xyz);
            }else{
                vec3 toCamera = linesParams.cameraPosition.xyz - center;
                n = length(toCamera) > 1e-6 ? normalize(toCamera) : vec3(0.0, 0.0, 1.0);
            }
            vec3 offset = lineOffset(tin, tout, n, a_lineParams);
            gl_Position = linesParams.viewProjection * vec4(center + offset * halfWidth, 1.0);
        #endif
    #else
        gl_Position = linesParams.mvpMatrix * vec4(a_position, 1.0);
    #endif

    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
