// Mirroring of the tangent frame, shared by the two mesh vertex paths (mesh.vert and
// includes/surface_vs.glsl) so they cannot drift apart. Included after the inputs it reads:
// pbrParams.modelMatrix, and the ones the including stage declares, i_matrix_col1..3 with
// HAS_INSTANCING, spriteParams.textureRect with HAS_TEXTURERECT and i_textureRect with
// HAS_INSTANCING (both with a UV set). Vertex stage only.
//
// The frame (T along u, B = cross(N, T) * a_tangent.w up the image) is computed from the
// un-mirrored inputs; then:
//   - a transform with a negative determinant (the model matrix's, times the instance
//     matrix's) turns the bitangent: T turned with the mirror, v did not (Unity's
//     unity_WorldTransformParams.w);
//   - a mirrored texture rect (a negative width or height, the sprite's times the
//     instance's) turns the axis it mirrors: u (or v) runs the other way.
// A zero determinant or rect size keeps the frame. For every non-mirrored draw both signs are
// 1 and the frame is bit for bit what it was without them.

#ifndef TANGENT_FRAME_GLSL
#define TANGENT_FRAME_GLSL

// -1 for a negative value, else 1 (a zero keeps the frame)
float tangentFrameSign(float value){
    return value < 0.0 ? -1.0 : 1.0;
}

vec2 tangentFrameSign(vec2 value){
    return vec2(tangentFrameSign(value.x), tangentFrameSign(value.y));
}

// the sign of the transform's determinant: the model matrix's, times the instance matrix's
float tangentFrameMirror(){
    float frameSign = tangentFrameSign(determinant(mat3(pbrParams.modelMatrix)));
    #ifdef HAS_INSTANCING
        frameSign *= tangentFrameSign(determinant(mat3(i_matrix_col1.xyz, i_matrix_col2.xyz, i_matrix_col3.xyz)));
    #endif
    return frameSign;
}

// the signs of the texture rect's size (x: u mirrored, y: v mirrored), the sprite's times
// the instance's; (1, 1) without a texture rect
vec2 tangentFrameRectSign(){
    vec2 rectSign = vec2(1.0);
    #if defined(HAS_UV_SET1) && defined(HAS_TEXTURERECT)
        rectSign *= tangentFrameSign(spriteParams.textureRect.zw);
    #endif
    #if defined(HAS_UV_SET1) && defined(HAS_INSTANCING)
        rectSign *= tangentFrameSign(i_textureRect.zw);
    #endif
    return rectSign;
}

// the world tangent and bitangent of a mesh with tangents, mirrored as the draw mirrors them
void mirrorTangentFrame(inout vec3 tangent, inout vec3 bitangent){
    vec2 rectSign = tangentFrameRectSign();
    tangent *= rectSign.x;
    bitangent *= tangentFrameMirror() * rectSign.y;
}

#endif
