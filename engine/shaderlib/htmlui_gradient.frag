#version 450

// HTML UI gradients: RmlUi's linear-gradient, radial-gradient and conic-gradient, plain and
// repeating (the parameter mapping of RmlUi's GL3 renderer). Positions are the geometry's
// texture coordinates (element pixels); the stops are premultiplied sRGB, interpolated
// linearly between positions like a browser, with no colour conversion.
#define MAX_NUM_STOPS 16
#define PI 3.14159265

#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5

uniform u_fs_htmluiGradient {
    vec4 funcAndCount;                          // x: function (above), y: number of stops
    vec4 pv;                                    // xy: p, zw: v (see main)
    vec4 stopColors[MAX_NUM_STOPS];
    vec4 stopPositions[MAX_NUM_STOPS / 4];      // 4 per vec4, normalized: 0 at p, 1 at the end
} gradient;

in vec4 v_color;
in vec2 v_uv1;

out vec4 g_finalColor;

float stopPosition(int i){
    vec4 four = gradient.stopPositions[i / 4];
    int c = i - (i / 4) * 4;
    return (c == 0) ? four.x : ((c == 1) ? four.y : ((c == 2) ? four.z : four.w));
}

vec4 mixStopColors(float t, int numStops){
    vec4 color = gradient.stopColors[0];
    float previous = stopPosition(0);
    for (int i = 1; i < MAX_NUM_STOPS; i++){
        if (i >= numStops)
            break;
        float position = stopPosition(i);
        // a hard stop (equal positions) switches at its position
        float f = (position > previous) ? clamp((t - previous) / (position - previous), 0.0, 1.0) : step(position, t);
        color = mix(color, gradient.stopColors[i], f);
        previous = position;
    }
    return color;
}

void main() {
    int func = int(gradient.funcAndCount.x + 0.5);
    int numStops = int(gradient.funcAndCount.y + 0.5);
    vec2 p = gradient.pv.xy;
    vec2 v = gradient.pv.zw;

    float t = 0.0;
    if (func == LINEAR || func == REPEATING_LINEAR){
        // p: start point, v: vector to the end point
        float distSquare = max(dot(v, v), 1e-12);
        t = dot(v, v_uv1 - p) / distSquare;
    }else if (func == RADIAL || func == REPEATING_RADIAL){
        // p: centre, v: inverse radii
        t = length(v * (v_uv1 - p));
    }else{
        // p: centre, v: unit vector of the start angle
        mat2 R = mat2(v.x, -v.y, v.y, v.x);
        vec2 V = R * (v_uv1 - p);
        t = 0.5 + atan(-V.x, V.y) / (2.0 * PI);
    }

    if (func >= REPEATING_LINEAR && numStops > 1){
        float t0 = stopPosition(0);
        float t1 = stopPosition(numStops - 1);
        if (t1 > t0)
            t = t0 + mod(t - t0, t1 - t0);
    }

    g_finalColor = (numStops > 0) ? v_color * mixStopColors(t, numStops) : vec4(0.0);
}
