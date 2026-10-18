// HTML UI effects (phase 5): the interface every effect shares. An effect file includes the
// library files it uses, declares `uniform u_fs_effectParams { ... } params;` (vec4 members
// first, then vec2, then float; floats only, arrays only for palettes), defines
// jfx_paint (paint effects) or jfx_filter (filter effects) and ends with
// #include "effects/lib/main_paint.glsl" or "effects/lib/main_filter.glsl".
#ifndef JFX_COMMON_GLSL
#define JFX_COMMON_GLSL

#define JFX_PI 3.14159265359
#define JFX_TAU 6.28318530718

// written by the engine per draw (EffectFrameBlock, 14 x vec4)
uniform u_fs_effectFrame {
    vec4 size;          // xy area size CSS px, zw 1/size (0 for an empty axis)
    vec4 time;          // x clock*time-scale+time-offset (s), y frame dt (0 when fixed), z dpr, w 1 when fixed
    vec4 anim;          // integrated @time clocks, declaration order
    vec4 pointer;       // xy area-local CSS px, z inside 0/1, w primary down 0/1
    vec4 pointerSmooth; // xy smoothed local px, z presence 0..1, w 0
    vec4 pointerEx;     // xy velocity CSS px/s, z s since last press inside (1e6 none), w s since enter (1e6 outside)
    vec4 layer;         // filter form: xy layer size device px, zw 1/size
    vec4 toLocalX;      // filter form: local.x = dot(toLocalX.xyz, vec3(layerPx, 1))
    vec4 toLocalY;
    vec4 toLayerX;      // filter form: layerPx.x = dot(toLayerX.xyz, vec3(localPx, 1))
    vec4 toLayerY;
    vec4 srcRect;       // filter form: valid source uv rect (scissor texel centres), xy min zw max
    vec4 radii;         // area border radii CSS px: tl tr br bl
    vec4 misc;          // x opacity, y blend index, z element seed [0,1), w 1: paint form clipped to the rounded area
} frame;

#ifdef JFX_FILTER_FORM
    // what the effect processes (premultiplied sRGB, the layer's pixels) and its blurred copy
    // (@prepass blur)
    uniform texture2D u_effectSource;
    uniform sampler u_effectSource_smp;
    uniform texture2D u_effectBlurred;
    uniform sampler u_effectBlurred_smp;
#endif
#if defined(JFX_STEP_FORM)
    // phase 6, a stateful effect's step and scatter forms: main_step.glsl sets these for the
    // target texel (its position in the area), so jfx_context() and the draw-form code of the
    // effect work there too
    vec2 v_texcoord = vec2(0.0);
    vec2 v_uv = vec2(0.0);
    vec4 v_color = vec4(1.0);
#elif defined(JFX_FILTER_FORM)
    in vec2 v_texcoord;
#else
    in vec2 v_uv;
    in vec4 v_color;
#endif

out vec4 frag_color;

struct JfxContext {
    vec2 uv;                // 0..1 over the effect area, y down
    vec2 px;                // uv * size: CSS px from the area's top-left corner
    vec2 size;              // the area in CSS px
    float aspect;           // size.x / size.y (guarded)
    float time;             // clock * time-scale + time-offset, seconds
    vec4 anim;              // the integrated @time clocks
    float dt;               // the frame's delta (0 with a fixed clock)
    float dpr;              // device px per CSS px
    vec2 pointer;           // area-local CSS px
    vec2 pointerSmooth;     // smoothed (80 ms half-life)
    float pointerInside;    // 0/1
    float pointerPresence;  // eased 0..1 over 150 ms
    float pointerDown;      // primary button 0/1
    vec2 pointerVelocity;   // CSS px/s
    float sincePress;       // s since the last press inside (1e6: none)
    float sinceEnter;       // s since the pointer entered (1e6: outside)
    vec4 radii;             // border radii CSS px: tl tr br bl
    float seed;             // the element's seed [0,1)
};

#ifdef JFX_FILTER_FORM
// this fragment in layer device px (top-left origin)
vec2 jfx_layer_px(){
    return v_texcoord * frame.layer.xy;
}

vec2 jfx_layer_to_local(vec2 layerPx){
    vec3 p = vec3(layerPx, 1.0);
    return vec2(dot(frame.toLocalX.xyz, p), dot(frame.toLocalY.xyz, p));
}

vec2 jfx_local_to_layer_uv(vec2 localPx){
    vec3 p = vec3(localPx, 1.0);
    return vec2(dot(frame.toLayerX.xyz, p), dot(frame.toLayerY.xyz, p)) * frame.layer.zw;
}
#endif

JfxContext jfx_context(){
    JfxContext c;
#ifdef JFX_FILTER_FORM
    c.px = jfx_layer_to_local(jfx_layer_px());
    c.uv = c.px * frame.size.zw;
#else
    c.uv = v_uv;
    c.px = v_uv * frame.size.xy;
#endif
    c.size = frame.size.xy;
    c.aspect = frame.size.x / max(frame.size.y, 1e-6);
    c.time = frame.time.x;
    c.anim = frame.anim;
    c.dt = frame.time.y;
    c.dpr = frame.time.z;
    c.pointer = frame.pointer.xy;
    c.pointerSmooth = frame.pointerSmooth.xy;
    c.pointerInside = frame.pointer.z;
    c.pointerPresence = frame.pointerSmooth.z;
    c.pointerDown = frame.pointer.w;
    c.pointerVelocity = frame.pointerEx.xy;
    c.sincePress = frame.pointerEx.z;
    c.sinceEnter = frame.pointerEx.w;
    c.radii = frame.radii;
    c.seed = frame.misc.z;
    return c;
}

#endif // JFX_COMMON_GLSL
