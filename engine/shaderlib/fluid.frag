#version 450
// the defaults Vulkan GLSL has anyway, stated: Pavel's kernels are highp (fp32) wherever
// precision matters
precision highp float;
precision highp int;

// The fluid simulation (FluidSimulator; Pavel Dobryakov's WebGL-Fluid-Simulation, MIT): one
// program for the solver passes and one for the look passes (FLUID_LOOK), the pass chosen by
// fluid.target.z. The kernels are the shared ones of effects/lib/fam_fluid_solver.glsl and
// fam_fluid_look.glsl (the HTML UI `fluid` effect runs the same). Every pass but the screen
// composite writes its whole target and addresses it by gl_FragCoord: uv = (x, row) / size,
// row 0 being uv.y = 0 on every backend, so ping-pong passes never flip. The simulation's
// images keep the fluid's bottom row at uv.y = 0; the display writes the output top row first
// (the engine's texture convention, framebuffer textures are top-left origin everywhere).
//
// FLUID_MANUAL_FILTERING: the device cannot filter half floats linearly, the advection
// interpolates itself (the solver program only).

#ifdef FLUID_LOOK
    in vec2 v_clip;
#endif

out vec4 frag_color;

uniform u_fs_fluid {
    vec4 target;    // xy 1 / target size, z the pass, w the composite's vertical flip (0 or 1)
    vec4 texel;     // xy the texel step the pass reads with, zw the source's texel (advection)
    vec4 params;    // x dt, y the pass's value, z aspect (width / height), w splat radius
    vec4 vector;    // splat: xy point; bloom prefilter: xyz curve, w threshold
    vec4 color;     // splat: rgb value; display: rgb back colour, w 1 transparent
    vec4 flags;     // display: x shading, y bloom, z sunrays (0 or 1)
} fluid;

uniform texture2D u_fluidTex0;
uniform sampler u_fluidTex0_smp;
uniform texture2D u_fluidTex1;
uniform sampler u_fluidTex1_smp;
#ifdef FLUID_LOOK
    uniform texture2D u_fluidTex2;
    uniform sampler u_fluidTex2_smp;
#endif

vec4 tex0(vec2 uv){
    return texture(sampler2D(u_fluidTex0, u_fluidTex0_smp), uv);
}

vec4 tex1(vec2 uv){
    return texture(sampler2D(u_fluidTex1, u_fluidTex1_smp), uv);
}

#ifdef FLUID_LOOK

    vec4 tex2(vec2 uv){
        return texture(sampler2D(u_fluidTex2, u_fluidTex2_smp), uv);
    }

    // tex0: the image the pass reads; tex1: the level added (bloom up) or the bloom (display);
    // tex2: the sunrays (display)
    #define FLUID_LOOK_SOURCE(uv) tex0(uv)
    #include "effects/lib/fam_fluid_look.glsl"

    // FluidSimulator's LookPass
    const int PASS_BLOOM_PREFILTER = 0;
    const int PASS_BLOOM_BLUR = 1;
    const int PASS_BLOOM_UP = 2;
    const int PASS_BLOOM_FINAL = 3;
    const int PASS_SUNRAYS_MASK = 4;
    const int PASS_SUNRAYS = 5;
    const int PASS_BLUR = 6;
    const int PASS_DISPLAY = 7;
    const int PASS_COMPOSITE = 8;

#else

    // tex0: the velocity (curl, vorticity, divergence, gradient, advection) or the divergence
    // (Jacobi); tex1: the curl (vorticity), the pressure (clear, Jacobi, gradient) or the field
    // advected, splatted or copied
    #define FLUID_VELOCITY(uv) tex0(uv).xy
    #define FLUID_DIVERGENCE(uv) tex0(uv).x
    #define FLUID_CURL(uv) tex1(uv).x
    #define FLUID_PRESSURE(uv) tex1(uv).x
    #define FLUID_SOURCE(uv) tex1(uv)
    #include "effects/lib/fam_fluid_solver.glsl"

    // FluidSimulator's SolverPass
    const int PASS_CURL = 0;
    const int PASS_VORTICITY = 1;
    const int PASS_DIVERGENCE = 2;
    const int PASS_PRESSURE_CLEAR = 3;
    const int PASS_JACOBI = 4;
    const int PASS_GRADIENT = 5;
    const int PASS_ADVECT = 6;
    const int PASS_SPLAT = 7;
    const int PASS_COPY = 8;

#endif

void main(){
    vec2 uv = gl_FragCoord.xy * fluid.target.xy;
    int pass = int(fluid.target.z + 0.5);
    vec2 texel = fluid.texel.xy;

#ifdef FLUID_LOOK
    if (pass == PASS_BLOOM_PREFILTER){
        vec3 c = tex0(uv).rgb;
        frag_color = vec4(fluidBloomPrefilter(c, fluid.vector.xyz, fluid.vector.w), 0.0);
    }else if (pass == PASS_BLOOM_BLUR){
        frag_color = fluidBloomBlur(uv, texel);
    }else if (pass == PASS_BLOOM_UP){
        // his additive (ONE, ONE) upsample into the level, as a sum into the level's copy
        frag_color = fluidBloomBlur(uv, texel) + tex1(uv);
    }else if (pass == PASS_BLOOM_FINAL){
        frag_color = fluidBloomBlur(uv, texel) * fluid.params.y;
    }else if (pass == PASS_SUNRAYS_MASK){
        frag_color = fluidSunraysMask(tex0(uv));
    }else if (pass == PASS_SUNRAYS){
        frag_color = vec4(fluidSunrays(uv, fluid.params.y), 0.0, 0.0, 1.0);
    }else if (pass == PASS_BLUR){
        frag_color = fluidBlur(uv, texel);
    }else if (pass == PASS_DISPLAY){
        // the output's first row is the fluid's top
        vec2 p = vec2(uv.x, 1.0 - uv.y);
        vec3 c = tex0(p).rgb;
        if (fluid.flags.x > 0.5){
            vec3 l = tex0(p - vec2(texel.x, 0.0)).rgb;
            vec3 r = tex0(p + vec2(texel.x, 0.0)).rgb;
            vec3 t = tex0(p + vec2(0.0, texel.y)).rgb;
            vec3 b = tex0(p - vec2(0.0, texel.y)).rgb;
            c *= fluidShadingDiffuse(l, r, t, b, texel);
        }
        bool useBloom = fluid.flags.y > 0.5;
        bool useSunrays = fluid.flags.z > 0.5;
        vec3 bloom = useBloom ? tex1(p).rgb : vec3(0.0);
        float sunrays = useSunrays ? tex2(p).r : 1.0;
        vec4 display = fluidDisplayFinish(c, bloom, sunrays, fluidDitherNoise(gl_FragCoord.xy), useBloom, useSunrays);
        // an 8-bit target clamps; opaque: over the back colour (his ONE, ONE_MINUS_SRC_ALPHA
        // blend over drawColor), transparent: premultiplied as it is
        display = clamp(display, 0.0, 1.0);
        if (fluid.color.w < 0.5)
            display = vec4(display.rgb + fluid.color.rgb * (1.0 - display.a), 1.0);
        frag_color = display;
    }else{
        // PASS_COMPOSITE: the output over a view (premultiplied blend), its top row at the
        // view's top (clip y +1), or at clip y -1 into a target drawn Y-flipped (GL offscreen)
        vec2 c = v_clip * 0.5 + 0.5;
        vec2 p = vec2(c.x, fluid.target.w > 0.5 ? c.y : 1.0 - c.y);
        frag_color = tex0(p);
    }
#else
    if (pass == PASS_CURL){
        frag_color = vec4(fluidCurl(uv, texel), 0.0, 0.0, 1.0);
    }else if (pass == PASS_VORTICITY){
        frag_color = vec4(fluidVorticity(uv, texel, fluid.params.y, fluid.params.x), 0.0, 1.0);
    }else if (pass == PASS_DIVERGENCE){
        frag_color = vec4(fluidDivergence(uv, texel), 0.0, 0.0, 1.0);
    }else if (pass == PASS_PRESSURE_CLEAR){
        frag_color = vec4(fluidPressureClear(uv, fluid.params.y), 0.0, 0.0, 1.0);
    }else if (pass == PASS_JACOBI){
        frag_color = vec4(fluidJacobi(uv, texel), 0.0, 0.0, 1.0);
    }else if (pass == PASS_GRADIENT){
        frag_color = vec4(fluidGradientSubtract(uv, texel, fluid.params.y), 0.0, 1.0);
    }else if (pass == PASS_ADVECT){
        frag_color = fluidAdvect(uv, texel, fluid.texel.zw, fluid.params.x, fluid.params.y);
    }else if (pass == PASS_SPLAT){
        frag_color = fluidSplat(uv, fluid.vector.xy, fluid.color.rgb, fluid.params.w, fluid.params.z);
    }else{
        // PASS_COPY: a resize keeps the field, resampled (his resizeFBO)
        frag_color = tex1(uv);
    }
#endif
}
