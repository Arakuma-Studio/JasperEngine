// The material's own base colour for library mesh shaders that draw what the engine's
// Material holds instead of @property / @texture values: Material::baseColorFactor
// (Mesh::setColor, a Sprite's colour) and Material::baseColorTexture (a Sprite's, Tilemap's
// or MeshPolygon's texture), sampled at the first UV set (@require uv1). Declares
// u_fs_pbrParams in the layout the engine uploads for unlit variants (with the alphaCutoff
// of ALPHA_MASK) and u_baseColorTexture, which the engine binds by name. Lighting mode
// none (MATERIAL_UNLIT) only. Fragment stage.
// BASE_COLOR_TEXTURE_ONLY (defined before the include): only the texture, for a shader that
// never reads the colour factor or the alpha mode (no u_fs_pbrParams: an unread block).

#ifndef BASE_COLOR_FS_GLSL
#define BASE_COLOR_FS_GLSL

#ifndef BASE_COLOR_TEXTURE_ONLY
uniform u_fs_pbrParams {
    vec4 baseColorFactor; // linear
    #ifdef ALPHA_MASK
        // the CPU Material prefix: metallic and roughness sit before alphaCutoff
        float alphaMaskMetallicPadding;
        float alphaMaskRoughnessPadding;
        float alphaCutoff;
    #endif
} pbrParams;
#endif

#ifdef HAS_UV_SET1
    uniform texture2D u_baseColorTexture;
    uniform sampler u_baseColor_smp;

    // the base colour texture as stored (sRGB-encoded colour, alpha)
    vec4 sampleBaseColorTexture(vec2 uv){
        return texture(sampler2D(u_baseColorTexture, u_baseColor_smp), uv);
    }

    // size of the base colour texture in texels
    vec2 baseColorTextureSize(){
        return vec2(textureSize(sampler2D(u_baseColorTexture, u_baseColor_smp), 0));
    }
#endif

#ifndef BASE_COLOR_TEXTURE_ONLY
// glTF alpha modes of the material: MASK discards below the cutoff, MASK and OPAQUE
// make what survives opaque
vec4 applyMaterialAlphaMode(vec4 color){
    #ifdef ALPHA_MASK
        if (color.a < pbrParams.alphaCutoff){
            discard;
        }
    #endif
    #if defined(ALPHA_MASK) || defined(ALPHA_OPAQUE)
        color.a = 1.0;
    #endif
    return color;
}
#endif

#endif
