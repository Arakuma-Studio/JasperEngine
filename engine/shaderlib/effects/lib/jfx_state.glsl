// HTML UI effects (phase 6): persistent state of a stateful effect (docs/html-ui/phase-6-stateful-effects.md
// 3.9). An effect with @state lines declares each state with JFX_STATE(<name>) ('-' in the
// manifest name as '_' here), defines vec4 jfx_step(JfxStep s) for its passes (and
// vec4 jfx_scatter(JfxStep s, JfxPlaced a) when a pass scatters), and ends with
// #include "effects/lib/main_step.glsl" after its main_paint/main_filter include.
//
// State texels are addressed in memory rows on every backend: texel (x, y) is what
// <name>_fetch(ivec2(x, y)) reads and what a pass writes at gl_FragCoord (x + 0.5, y + 0.5);
// <name>_sample(uv) reads uv (0..1, y down: row 0 at the top of the effect area) with the
// state's filter. A pass reads every state's current image and writes its target's other one.
// A state every writer of which is additive (add, scatter) has one image: the pass writing it
// reads it as an empty 1 x 1 black image (its size is s.size there).
#ifndef JFX_STATE_GLSL
#define JFX_STATE_GLSL

#include "effects/lib/jfx_common.glsl"

#define JFX_STATE(n) \
    uniform texture2D u_state_##n; \
    uniform sampler u_state_##n##_smp; \
    ivec2 n##_size(){ return textureSize(sampler2D(u_state_##n, u_state_##n##_smp), 0); } \
    vec4 n##_fetch(ivec2 p){ \
        return texelFetch(sampler2D(u_state_##n, u_state_##n##_smp), clamp(p, ivec2(0), n##_size() - 1), 0); \
    } \
    vec4 n##_sample(vec2 uv){ return texture(sampler2D(u_state_##n, u_state_##n##_smp), uv); }

// an injected splat (UIElement::effectSplat): area-local CSS px, px/s, linear RGBA
struct JfxSplat {
    vec2 pos;
    float radius;
    float strength;
    vec2 vel;
    vec4 color;
};

// a scatter quad's agent (scatter form): its placement texel and the fragment's offset
struct JfxPlaced {
    int index;          // the placement texel's index (y * width + x)
    ivec2 cell;         // the placement texel
    vec4 place;         // xy centre in target texels, zw half extents
    vec2 offset;        // this fragment from the centre, target texels
};

struct JfxStep {
    int pass;           // the pass index (JFX_PASS_<NAME>)
    int iteration;      // 0.. for a repeated pass (x<count>)
    ivec2 cell;         // the target texel
    vec2 uv;            // the texel's centre over the target, 0..1 (y down)
    vec2 size;          // the target in texels
    float dt;           // the step, seconds
    float time;         // the simulation time at the end of this step, seconds
    float steps;        // steps since the reset (this one included)
    bool warm;          // a silent @warmup step
    int stepInFrame;    // 0.. within this frame
    int stepsInFrame;
    vec2 pointer;       // area-local CSS px at this step
    vec2 pointerPrev;   // at the previous step
    vec2 pointerVelocity;   // px/s (raw)
    bool pointerKnown;  // a pointer position exists (else pointer = pointerPrev = 0)
    bool pointerInside;
    bool pointerDown;
    bool pressed;       // the primary button went down at this step
    bool released;
    int splatCount;     // jfx_splat(0 .. splatCount - 1)
    JfxContext c;       // the effect context at this texel (c.uv = uv)
};

#ifdef JFX_STEP_FORM
// written by the engine per pass (EffectStepBlock, 31 x vec4)
uniform u_fs_effectStep {
    vec4 target;        // xy target texels, zw 1/size
    vec4 info;          // x pass, y iteration, z steps since reset (this one included), w 1 during @warmup
    vec4 clock;         // x simulation time s, y dt s, z step index in this frame, w steps this frame
    vec4 pointer;       // xy area-local CSS px, z inside 0/1, w primary down 0/1
    vec4 pointerPrev;   // xy previous step's position, z 1 when the pointer is known, w 1 on the step of a press
    vec4 pointerVel;    // xy raw velocity px/s, z 1 on the step of a release, w 0
    vec4 splatInfo;     // x splat count (0..8)
    vec4 splatPos[8];   // xy CSS px, z radius px, w strength
    vec4 splatVel[8];   // xy px/s
    vec4 splatColor[8]; // linear RGBA
} jstep;

#ifdef JFX_SCATTER_FORM
// (effect_scatter.vert: the same order)
flat in vec4 v_scatterPlace;
flat in vec3 v_scatterCell;
#endif

JfxSplat jfx_splat(int i){
    JfxSplat s;
    s.pos = jstep.splatPos[i].xy;
    s.radius = jstep.splatPos[i].z;
    s.strength = jstep.splatPos[i].w;
    s.vel = jstep.splatVel[i].xy;
    s.color = jstep.splatColor[i];
    return s;
}

// the target texel's position in the effect area (CSS px): the state image covers the area
vec2 jfx_step_area_px(vec2 cellCentre){
    return cellCentre * jstep.target.zw * frame.size.xy;
}

JfxStep jfx_step_context(){
    JfxStep s;
    s.cell = ivec2(gl_FragCoord.xy);
    s.size = jstep.target.xy;
    s.uv = (vec2(s.cell) + 0.5) * jstep.target.zw;
    s.pass = int(jstep.info.x + 0.5);
    s.iteration = int(jstep.info.y + 0.5);
    s.steps = jstep.info.z;
    s.warm = jstep.info.w > 0.5;
    s.time = jstep.clock.x;
    s.dt = jstep.clock.y;
    s.stepInFrame = int(jstep.clock.z + 0.5);
    s.stepsInFrame = int(jstep.clock.w + 0.5);
    s.pointer = jstep.pointer.xy;
    s.pointerInside = jstep.pointer.z > 0.5;
    s.pointerDown = jstep.pointer.w > 0.5;
    s.pointerPrev = jstep.pointerPrev.xy;
    s.pointerKnown = jstep.pointerPrev.z > 0.5;
    s.pressed = jstep.pointerPrev.w > 0.5;
    s.pointerVelocity = jstep.pointerVel.xy;
    s.released = jstep.pointerVel.z > 0.5;
    s.splatCount = int(jstep.splatInfo.x + 0.5);
    // the draw-form inputs at this texel (jfx_common.glsl): the area uv, and in a filter use the
    // layer uv of that point
    v_uv = s.uv;
    v_color = vec4(1.0);
#ifdef JFX_FILTER_FORM
    vec3 p = vec3(s.uv * frame.size.xy, 1.0);
    v_texcoord = vec2(dot(frame.toLayerX.xyz, p), dot(frame.toLayerY.xyz, p)) * frame.layer.zw;
#endif
    s.c = jfx_context();
    return s;
}

#ifdef JFX_SCATTER_FORM
JfxPlaced jfx_placed(){
    JfxPlaced a;
    a.cell = ivec2(v_scatterCell.xy + 0.5);
    a.index = int(v_scatterCell.z + 0.5);
    a.place = v_scatterPlace;
    a.offset = gl_FragCoord.xy - v_scatterPlace.xy;
    return a;
}
#endif
#endif // JFX_STEP_FORM

#endif // JFX_STATE_GLSL
