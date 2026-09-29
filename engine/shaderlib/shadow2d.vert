#version 450

// 1D polar shadow map pass for 2D lights: every occluder segment is drawn as a
// line into a single 1px-tall atlas row. X maps the angle around the light
// (theta/pi -> NDC [-1,1]) and depth maps the normalized distance to the light.
// The pass is drawn three times with offset.x = -2 / 0 / +2 so segments that
// cross the theta = +-pi seam rasterize on both ends of the row.

uniform u_vs_shadow2dParams {
    vec4 lightPos_range; // xy = light world position, w = range
    vec4 offset;         // x = NDC x offset for seam duplication (-2, 0, +2)
} params;

in vec2 a_position;  // this endpoint of the segment (world space)
in vec2 a_texcoord1; // the OTHER endpoint of the same segment (world space)

// The fragment shader intersects each fragment's polar ray with the segment
// analytically, so it needs the segment endpoints in a vertex-order-independent
// (canonical) order: both vertices output identical values, making the varyings
// constant across the primitive.
out vec2 v_segA;
out vec2 v_segB;
out float v_angle; // unwrapped angle: interpolates exactly (x IS the angle axis)
out vec4 v_lightPosRange;

const float PI = 3.14159265359;

void main(){
    vec2 lightPos = params.lightPos_range.xy;

    float aSelf = atan(a_position.y - lightPos.y, a_position.x - lightPos.x);

    // Unwrap against the segment MIDPOINT angle. The midpoint is identical for
    // both vertices of the segment (a+b is commutative), so both make the same
    // unwrap decision and stay a short continuous span. Unwrapping against the
    // other endpoint instead is wrong: on a seam-crossing segment each vertex
    // would jump to the opposite branch and the line would rasterize across the
    // whole row.
    vec2 mid = 0.5 * (a_position + a_texcoord1) - lightPos;
    float aMid = atan(mid.y, mid.x);
    if (aSelf - aMid >  PI) aSelf -= 2.0 * PI;
    if (aSelf - aMid < -PI) aSelf += 2.0 * PI;

    float x = aSelf / PI + params.offset.x;

    // z in [0,1] used only for clipping and coarse ordering; the precise value
    // is recomputed per-fragment and written to gl_FragDepth / the color output
    float dist = clamp(length(a_position - lightPos) / params.lightPos_range.w, 0.0, 1.0);

    // canonical endpoint order (lexicographic), so both vertices agree
    bool selfFirst = (a_position.x < a_texcoord1.x) ||
                     (a_position.x == a_texcoord1.x && a_position.y < a_texcoord1.y);
    v_segA = selfFirst ? a_position : a_texcoord1;
    v_segB = selfFirst ? a_texcoord1 : a_position;

    v_angle = aSelf;
    v_lightPosRange = params.lightPos_range;

    gl_Position = vec4(x, 0.0, dist, 1.0);
}
