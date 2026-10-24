// The extended PBR lobes of the built-in Lit (docs/specs/S11-materials-lighting.md): the
// u_fs_pbrExt block, the extension map slots and the hex tiling taps. Included by mesh.frag
// before includes/pbr.glsl; declares nothing without a PBR_* define, so every variant without
// one is the program it always was.
//
// The defines come from the material (MaterialPbr::getPbrVariant, the "builtin:lit#..."
// reference): a lobe's (PBR_CLEARCOAT, PBR_SHEEN, PBR_ANISOTROPY, PBR_IRIDESCENCE,
// PBR_TRANSMISSION, PBR_VOLUME, PBR_SPECULAR), hex tiling's (PBR_HEX_TILING) and one per map
// the variant binds (PBR_<LOBE>_MAP, PBR_CLEARCOAT_NORMAL_MAP). A variant binds at most three
// maps, u_pbrExtTexture0..2, taken in the maps' priority order: clear coat, specular,
// transmission, sheen, anisotropy, iridescence, clear coat normal. Each slot samples the UV
// set texCoordSets.set1.y, .z, .w names (with a second UV set).
//
// A fork of mesh.frag keeps the lobes by keeping these includes and hooks (docs/shaders).

#ifndef PBR_EXT_SLOTS_GLSL
#define PBR_EXT_SLOTS_GLSL

#if defined(PBR_CLEARCOAT) || defined(PBR_SHEEN) || defined(PBR_ANISOTROPY) || defined(PBR_IRIDESCENCE) || \
    defined(PBR_TRANSMISSION) || defined(PBR_SPECULAR) || defined(PBR_HEX_TILING)
    #define PBR_EXT
#endif

#ifdef PBR_EXT
    // std140, 8 vec4 (MaterialPbr::getUniformBlock)
    uniform u_fs_pbrExt {
        vec4 clearcoat;     // factor, roughness, normal scale, -
        vec4 sheen;         // rgb colour (linear), roughness
        vec4 anisotropy;    // strength, cos(rotation), sin(rotation), -
        vec4 iridescence;   // factor, ior, thickness min (nm), thickness max (nm)
        vec4 transmission;  // factor, volume thickness, attenuation distance (0: none), -
        vec4 attenuation;   // rgb attenuation colour (linear), -
        vec4 specular;      // rgb colour (linear), factor
        vec4 iorHex;        // ior, hex rotation, hex contrast, -
        #ifdef HAS_UV_SET2
            // the UV set of each texture (includes/pbr.glsl u_fs_texCoordSets, as floats: the
            // block stays all float), here so that a variant keeps to 8 fragment uniform blocks
            vec4 set0;
            vec4 set1;
        #endif
    } pbrExt;
#endif

// the slot of each map: the number of maps before it in priority order
#ifdef PBR_CLEARCOAT_MAP
    #define PBR_N_CLEARCOAT 1
#else
    #define PBR_N_CLEARCOAT 0
#endif
#ifdef PBR_SPECULAR_MAP
    #define PBR_N_SPECULAR 1
#else
    #define PBR_N_SPECULAR 0
#endif
#ifdef PBR_TRANSMISSION_MAP
    #define PBR_N_TRANSMISSION 1
#else
    #define PBR_N_TRANSMISSION 0
#endif
#ifdef PBR_SHEEN_MAP
    #define PBR_N_SHEEN 1
#else
    #define PBR_N_SHEEN 0
#endif
#ifdef PBR_ANISOTROPY_MAP
    #define PBR_N_ANISOTROPY 1
#else
    #define PBR_N_ANISOTROPY 0
#endif
#ifdef PBR_IRIDESCENCE_MAP
    #define PBR_N_IRIDESCENCE 1
#else
    #define PBR_N_IRIDESCENCE 0
#endif
#ifdef PBR_CLEARCOAT_NORMAL_MAP
    #define PBR_N_CLEARCOAT_NORMAL 1
#else
    #define PBR_N_CLEARCOAT_NORMAL 0
#endif

#define PBR_SLOT_CLEARCOAT 0
#define PBR_SLOT_SPECULAR (PBR_N_CLEARCOAT)
#define PBR_SLOT_TRANSMISSION (PBR_SLOT_SPECULAR + PBR_N_SPECULAR)
#define PBR_SLOT_SHEEN (PBR_SLOT_TRANSMISSION + PBR_N_TRANSMISSION)
#define PBR_SLOT_ANISOTROPY (PBR_SLOT_SHEEN + PBR_N_SHEEN)
#define PBR_SLOT_IRIDESCENCE (PBR_SLOT_ANISOTROPY + PBR_N_ANISOTROPY)
#define PBR_SLOT_CLEARCOAT_NORMAL (PBR_SLOT_IRIDESCENCE + PBR_N_IRIDESCENCE)
#define PBR_EXT_MAPS (PBR_SLOT_CLEARCOAT_NORMAL + PBR_N_CLEARCOAT_NORMAL)

#if PBR_EXT_MAPS > 3
    #error "a builtin:lit variant binds at most three extension maps"
#endif

#if PBR_EXT_MAPS > 0 && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
    #define PBR_EXT_MAP0
    uniform texture2D u_pbrExtTexture0;
    uniform sampler u_pbrExtTexture0_smp;
#endif
#if PBR_EXT_MAPS > 1 && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
    #define PBR_EXT_MAP1
    uniform texture2D u_pbrExtTexture1;
    uniform sampler u_pbrExtTexture1_smp;
#endif
#if PBR_EXT_MAPS > 2 && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
    #define PBR_EXT_MAP2
    uniform texture2D u_pbrExtTexture2;
    uniform sampler u_pbrExtTexture2_smp;
#endif

#if defined(PBR_HEX_TILING) && defined(HAS_UV_SET1)
    #include "includes/hex_tiling.glsl"

    // UV set 0's taps, set up once at the start of main (pbrHexSetup), and the weights every
    // hex-sampled texture blends them by (the base colour's luminance sets them)
    HexTaps pbrHexTaps;
    vec3 pbrHexWeights;

    void pbrHexSetup(){
        pbrHexTaps = hexSetup(v_uv1, dFdx(v_uv1), dFdy(v_uv1), pbrExt.iorHex.y);
        pbrHexWeights = hexWeights(pbrHexTaps.barycentrics, vec3(1.0), 0.0);
    }

    // the base colour: its taps' luminance sets the weights of every other map
    vec4 pbrHexBaseColor(vec2 uv, bool hex){
        if (!hex)
            return texture(sampler2D(u_baseColorTexture, u_baseColor_smp), uv);
        vec4 c0 = HEX_TAP0(u_baseColorTexture, u_baseColor_smp, pbrHexTaps);
        vec4 c1 = HEX_TAP1(u_baseColorTexture, u_baseColor_smp, pbrHexTaps);
        vec4 c2 = HEX_TAP2(u_baseColorTexture, u_baseColor_smp, pbrHexTaps);
        pbrHexWeights = hexWeights(pbrHexTaps.barycentrics,
                                   vec3(hexLuminance(c0.rgb), hexLuminance(c1.rgb), hexLuminance(c2.rgb)), pbrExt.iorHex.z);
        return c0 * pbrHexWeights.x + c1 * pbrHexWeights.y + c2 * pbrHexWeights.z;
    }

    // a map of UV set 0 hex sampled, of the other set as it is (hex: whether it is on set 0)
    #define PBR_HEX_TEXTURE(tex, smp, uv, hex) \
        ((hex) ? HEX_SAMPLE(tex, smp, pbrHexTaps, pbrHexWeights) : texture(sampler2D(tex, smp), uv))
    // a tangent-space normal map, -1..1
    #define PBR_HEX_NORMAL(tex, smp, uv, hex) \
        ((hex) ? HEX_SAMPLE_NORMAL(tex, smp, pbrHexTaps, pbrHexWeights) : texture(sampler2D(tex, smp), uv).rgb * 2.0 - 1.0)
    #define PBR_HEX_ACTIVE
#endif

#endif
