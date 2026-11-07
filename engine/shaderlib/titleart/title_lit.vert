#version 450
precision highp float;

// Title Lit (docs/specs/S86-title-3d.md §6.4, ShaderType::TITLE_LIT): a 3D title layer's mesh.
// The 24-byte vertex is the layer-space position, the signed octahedral normal (SHORT2N) and the
// ids (USHORT4N: line span, char span, part, slot | role << 12). Per part the block holds its rig
// as a 3x4 matrix (identity for unrigged parts) and (no shear, z front, z back); the vertex
// stage applies the rig, the skew shear (x += skew.x (-z), normals by the inverse transpose)
// unless the part says no, the layer root and the camera. The layer-space position before the
// shear and the rig feeds the ramp, so gradients stick to the letters (Pachislop's
// project_vertex).

in vec3 a_position;
in vec2 a_normal;
in vec4 a_ids;
#ifdef TL_WRAP_UV
in vec2 a_wrap;
#endif

#define TL_MAX_PARTS 64

uniform u_vs_titleLit {
    mat4 viewProj;      // camera projection x view, the output crop and backend flip included
    mat4 model;         // layer space -> world (layer root and its groups)
    mat4 normalModel;   // inverse transpose of model's upper 3x3
    vec4 skew;          // x, y: shear per unit of -z
    vec4 parts[TL_MAX_PARTS * 4];   // per part: the rig's three rows, then (no shear, z front, z back, 0)
} tlv;

out vec3 v_world;
out vec3 v_normal;
out vec3 v_layerPos;
out vec3 v_layerNormal;
out vec2 v_zRange;
flat out vec4 v_ids;
#ifdef TL_WRAP_UV
out vec2 v_wrap;
#endif

vec3 tlOctDecode(vec2 f) {
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

void main() {
    vec4 ids = floor(a_ids * 65535.0 + 0.5);
    vec3 p = a_position;
    vec3 n = tlOctDecode(max(a_normal, vec2(-1.0)));
    v_layerPos = p;
    v_layerNormal = n;
    int part = min(int(ids.z), TL_MAX_PARTS - 1);
    vec4 info = tlv.parts[part * 4 + 3];
    v_zRange = info.yz;
#ifdef TL_RIGS
    vec4 r0 = tlv.parts[part * 4 + 0];
    vec4 r1 = tlv.parts[part * 4 + 1];
    vec4 r2 = tlv.parts[part * 4 + 2];
    vec4 hp = vec4(p, 1.0);
    p = vec3(dot(r0, hp), dot(r1, hp), dot(r2, hp));
    // a rig is rigid (rotation and uniform or axis scale): its rows' inverse transpose follows
    // from the cofactors of the 3x3
    vec3 c0 = vec3(r0.x, r1.x, r2.x), c1 = vec3(r0.y, r1.y, r2.y), c2 = vec3(r0.z, r1.z, r2.z);
    mat3 cof = mat3(cross(c1, c2), cross(c2, c0), cross(c0, c1));
    n = normalize(cof * n);
#endif
    // the shear moves the back of every solid by skew x its depth (the orb never shears)
    if (info.x < 0.5) {
        p.xy += tlv.skew.xy * (-p.z);
        // inverse transpose of [1 0 -sx; 0 1 -sy; 0 0 1]: n.z += sx n.x + sy n.y
        n = normalize(vec3(n.xy, n.z + tlv.skew.x * n.x + tlv.skew.y * n.y));
    }
    vec4 world = tlv.model * vec4(p, 1.0);
    v_world = world.xyz;
    v_normal = normalize(mat3(tlv.normalModel) * n);
    v_ids = ids;
#ifdef TL_WRAP_UV
    v_wrap = a_wrap;
#endif
    gl_Position = tlv.viewProj * world;
    #ifdef IS_VULKAN
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
