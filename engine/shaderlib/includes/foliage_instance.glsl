// The compact instance (T5 F3, R36): 32 bytes, in terrain-local space. i_matrix_col1..4, i_color and
// i_textureRect become file-scope variables filled by compactInstanceSetup(), the first line of a
// vertex stage's main, so the code after it and every library shader work unchanged. Only FLOAT4,
// SHORT4N and UBYTE4N inputs (all five backends map them), no gl_InstanceID.
//
// Include where the stage declares its instance inputs; those stay under !HAS_COMPACT_INSTANCE.

#ifndef FOLIAGE_INSTANCE_GLSL
#define FOLIAGE_INSTANCE_GLSL

#ifdef HAS_COMPACT_INSTANCE

in vec4 i_packed0;  // xyz position, w the uniform (width) scale
in vec4 i_packed1;  // snorm quaternion, x y z w
in vec4 i_packed2;  // unorm linear tint
in vec4 i_packed3;  // unorm: height ratio (byte / 128), wind phase, wind bend, density rank

vec4 i_matrix_col1;
vec4 i_matrix_col2;
vec4 i_matrix_col3;
vec4 i_matrix_col4;
vec4 i_color;
vec4 i_textureRect;

void compactInstanceSetup(){
    // SHORT4N keeps a quaternion to 3e-5 per component: unit again, or a tree scales with its yaw
    vec4 q = normalize(i_packed1);
    float width = i_packed0.w;
    float tall = width * i_packed3.x * (255.0 / 128.0);

    float xx = q.x * q.x;
    float yy = q.y * q.y;
    float zz = q.z * q.z;
    float xy = q.x * q.y;
    float xz = q.x * q.z;
    float yz = q.y * q.z;
    float wx = q.w * q.x;
    float wy = q.w * q.y;
    float wz = q.w * q.z;

    i_matrix_col1 = vec4((1.0 - 2.0 * (yy + zz)) * width, 2.0 * (xy + wz) * width, 2.0 * (xz - wy) * width, 0.0);
    i_matrix_col2 = vec4(2.0 * (xy - wz) * tall, (1.0 - 2.0 * (xx + zz)) * tall, 2.0 * (yz + wx) * tall, 0.0);
    i_matrix_col3 = vec4(2.0 * (xz + wy) * width, 2.0 * (yz - wx) * width, (1.0 - 2.0 * (xx + yy)) * width, 0.0);
    i_matrix_col4 = vec4(i_packed0.xyz, 1.0);
    i_color = i_packed2;
    i_textureRect = vec4(0.0, 0.0, 1.0, 1.0);
}

#endif

#endif
