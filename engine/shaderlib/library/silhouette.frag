#version 450

// @shader mesh
// @name "Silhouette"
// @category "Outline and stencil"
// @description "See-through silhouette: draws only the parts of the object hidden behind other objects (depth test greater, no depth write), in a flat colour or a fresnel glow, alpha blended. Use it as a material's extra pass, order Before (See-through silhouette preset), and draw the object after its occluders (the preset gives it sort priority 1). Each pixel blends once: stencil bit 0x02 is set where it draws (overlapping hidden parts do not stack). No shadows."
// @require normals position feature=SILHOUETTE_MODE_FRESNEL
// @state blend=alpha ztest=greater zwrite=off cull=back shadows=off
// @state stencil=notequal ref=2 readmask=2 writemask=2 pass=replace
// @keywords SILHOUETTE_MODE "Shading" FLAT FRESNEL
// @property color hdrcolor (0.25, 0.6, 1, 0.6) "Colour (alpha: opacity)"
// @property fresnelPower range(0.25, 8) 2 "Fresnel power" feature=SILHOUETTE_MODE_FRESNEL
// @property fresnelFill range(0, 1) 0.15 "Fresnel fill (opacity facing the view)" feature=SILHOUETTE_MODE_FRESNEL

// Drawn Before the main draw, the object's own depth is not in the buffer yet, so only
// what drew earlier (its occluders) can make the test pass; drawn After, the object's own
// concave parts hidden behind itself would show too. FRESNEL: opacity
// mix(fill, 1, pow(1 - |n.v|, power)) times the colour's alpha, an X-ray rim. The stencil
// bit stays set until the camera pass ends, so two silhouettes overlapping on screen
// blend once too.

#ifdef SILHOUETTE_MODE_FRESNEL
    #define FRAME_FS
    #include "includes/frame.glsl"

    in vec3 v_position;
    in vec3 v_normal;
#endif

uniform u_fs_customParams {
    vec4 color;
    #ifdef SILHOUETTE_MODE_FRESNEL
        float fresnelPower;
        float fresnelFill;
    #endif
} customParams;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main(){
    vec4 color = customParams.color;
    #ifdef SILHOUETTE_MODE_FRESNEL
        // towards the camera: from the position in perspective, the view axis in orthographic
        vec3 toCamera = mix(frame.cameraPosition.xyz - v_position, -frame.cameraDirection.xyz, frame.cameraPosition.w);
        float facing = abs(dot(normalize(v_normal), normalize(toCamera)));
        color.a *= mix(customParams.fresnelFill, 1.0, pow(max(1.0 - facing, 0.0), customParams.fresnelPower));
    #endif
    g_finalColor = finalizeStraightColor(color);
}
