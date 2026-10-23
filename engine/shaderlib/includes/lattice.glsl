// Lattice deformation (free-form deformation): the vertex moved by the lattices bound to its
// mesh, docs/superpowers/specs/2026-10-23-lattice-deformer-design.md sections 3 and 5. The
// math is the CPU solver's (engine/core/lattice/LatticeSolver.cpp): a change to one is a
// change to both.
//
// Vertex stage (the default): with HAS_LATTICE (mesh property 'Ltc', depth and G-buffer
// 'Ltc') it declares the record texture, the u_vs_lattice block and
//
//   void latticeDeform(int stage, inout vec3 position, inout vec3 normal, inout vec3 tangent, inout float stretch)
//
// which runs the vertex through every lattice of its submesh, in order, at one point of the
// vertex pipeline: stage 0 on the bind pose before skinning (call it between morph targets
// and skinning, only under HAS_SKINNING), stage 1 after skinning and the instance matrix
// (every binding of a mesh without skinning is a stage 1 one). Position, normal and tangent
// are object space; the normal (cofactor of the Jacobian, never recomputed from faces) and
// the tangent come out normalised, the tangent's handedness (a_tangent.w) is unchanged;
// stretch is multiplied by each lattice's volume ratio (1 at rest, below 1 squashed, below
// 0 folded inside out). Without HAS_LATTICE nothing is declared: guard the calls with it.
// latticeDeformSmooth also moves a second normal (a_smoothNormal, includes/surface_vs.glsl)
// by the same Jacobian. The colour, depth and G-buffer stages call the same function, so the
// position is the same expression everywhere (invariant gl_Position, depth pre-pass).
//
// A vertex colour mask (a binding's mask channel) reads a_color when the variant has one
// (HAS_VERTEX_COLOR_VEC3 / _VEC4, a vec3 colour's alpha is 1): declare a_color before
// including. A mesh without vertex colours is not masked (as the CPU solver without a colour).
//
// The stretch reaches the fragment stage when both stages define LATTICE_STRETCH before
// including (an unread varying fails the cross-compile, so it is opt-in): the vertex stage
// writes out float v_latticeStretch (latticeWriteStretch), and a fragment stage that
// defines LATTICE_FS before including gets
//
//   float latticeStretch()   // the volume ratio, 1.0 without a lattice or LATTICE_STRETCH
//
// (library/lattice_stretch draws it as a heat map).
//
// The data is engine-internal (LatticeRender::prepare): per binding a record of RGBA32F
// texels in u_latticeTexture (LATTICE_TEXTURE_WIDTH wide, row-major) starting at
// latticeStarts[b]: 0-3 object -> normalized space columns, 4-7 its inverse, 8 (rx, ry, rz,
// interpolation 0 sharp 1 smooth 2 cubic), 9 (stage, weight, outside 0 extend 1 fade, fade
// distance), 10 (mask channel: -1 none, 0-3 rgba, 4-7 inverted), 11 reserved, then the
// padded grid of offsets (rx+2)(ry+2)(rz+2), x fastest.

#ifndef LATTICE_MAX_PER_MESH
    // lattice/LatticeTypes.h (the compiler defines them from there)
    #define LATTICE_MAX_PER_MESH 4
#endif
#ifndef LATTICE_TEXTURE_WIDTH
    #define LATTICE_TEXTURE_WIDTH 64
#endif
#ifndef LATTICE_RECORD_HEADER
    #define LATTICE_RECORD_HEADER 12
#endif

#ifdef LATTICE_FS

#if defined(HAS_LATTICE) && defined(LATTICE_STRETCH)
    in float v_latticeStretch;
#endif

float latticeStretch(){
    #if defined(HAS_LATTICE) && defined(LATTICE_STRETCH)
        return v_latticeStretch;
    #else
        return 1.0;
    #endif
}

#elif defined(HAS_LATTICE)

// @image_sample_type u_latticeTexture unfilterable_float
uniform texture2D u_latticeTexture;
// @sampler_type u_lattice_smp nonfiltering
uniform sampler u_lattice_smp;

// floats only (see the note on custom blocks in mesh.vert)
uniform u_vs_lattice {
    vec4 latticeParams; // x bindings, y bitmask of the bindings deforming this submesh, z 1 = stretch wanted
    vec4 latticeStarts; // first texel of each binding's record
};

#ifdef LATTICE_STRETCH
    out float v_latticeStretch;
#endif

vec4 latticeTexel(int index){
    return texelFetch(sampler2D(u_latticeTexture, u_lattice_smp), ivec2(index % LATTICE_TEXTURE_WIDTH, index / LATTICE_TEXTURE_WIDTH), 0);
}

// the vertex colour a mask channel reads
vec4 latticeVertexColor(){
    #if defined(HAS_VERTEX_COLOR_VEC3)
        return vec4(a_color, 1.0);
    #elif defined(HAS_VERTEX_COLOR_VEC4)
        return a_color;
    #else
        return vec4(1.0);
    #endif
}

// One axis of the offset field (spec 3.2): the first padded node the weights apply to, the
// weights and their derivatives along the normalized coordinate (0 where it is clamped)
void latticeAxis(float x, int r, int interpolation, out int first, out vec4 w, out vec4 dw){
    float xc = clamp(x, 0.0, 1.0);
    float g = xc * float(r - 1);
    int c = min(int(floor(g)), r - 2);
    float t = g - float(c);
    float scale = (x >= 0.0 && x <= 1.0) ? float(r - 1) : 0.0;
    if (interpolation == 2){
        // uniform cubic B-spline over nodes c-1 .. c+2
        float s = 1.0 - t;
        float t2 = t * t;
        float t3 = t2 * t;
        w = vec4(s * s * s, 3.0 * t3 - 6.0 * t2 + 4.0, -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0, t3) / 6.0;
        dw = vec4(-s * s, 3.0 * t2 - 4.0 * t, -3.0 * t2 + 2.0 * t + 1.0, t2) * 0.5;
        first = c - 1;
    }else if (interpolation == 1){
        float s = t * t * (3.0 - 2.0 * t);
        float ds = 6.0 * t * (1.0 - t);
        w = vec4(1.0 - s, s, 0.0, 0.0);
        dw = vec4(-ds, ds, 0.0, 0.0);
        first = c;
    }else{
        w = vec4(1.0 - t, t, 0.0, 0.0);
        dw = vec4(-1.0, 1.0, 0.0, 0.0);
        first = c;
    }
    dw *= scale;
}

// Every lattice of the submesh at this stage, in order (spec 3.4). The Jacobian goes to
// object space (A^-1 J A), so at rest it is the identity exactly and the position moves by
// A^-1 delta, which is Minv (M p + delta) without the round trip.
void latticeDeformSmooth(int stage, inout vec3 position, inout vec3 normal, inout vec3 smoothNormal, inout vec3 tangent, inout float stretch){
    int count = int(latticeParams.x + 0.5);
    int active = int(latticeParams.y + 0.5);
    bool wantStretch = latticeParams.z > 0.5;
    vec4 color = latticeVertexColor();

    for (int b = 0; b < LATTICE_MAX_PER_MESH; b++){
        if (b >= count)
            break;
        if ((active & (1 << b)) == 0)
            continue;

        int start = int(latticeStarts[b] + 0.5);
        vec4 binding = latticeTexel(start + 9); // stage, weight, outside, fade distance
        #ifdef HAS_SKINNING
            if (int(binding.x + 0.5) != stage)
                continue;
        #else
            // without skinning every binding deforms after the instance matrix
            if (stage != 1)
                continue;
        #endif

        float mask = 1.0;
        #if defined(HAS_VERTEX_COLOR_VEC3) || defined(HAS_VERTEX_COLOR_VEC4)
            int channel = int(floor(latticeTexel(start + 10).x + 0.5));
            if (channel >= 4){
                mask = 1.0 - color[channel - 4];
            }else if (channel >= 0){
                mask = color[channel];
            }
        #endif
        float weight = binding.y * mask;
        if (weight == 0.0)
            continue;

        mat4 toNormalized = mat4(latticeTexel(start), latticeTexel(start + 1), latticeTexel(start + 2), latticeTexel(start + 3));
        vec3 u = (toNormalized * vec4(position, 1.0)).xyz;

        // outside the cage (spec 3.3): the fade and its gradient
        float fade = 1.0;
        vec3 fadeGradient = vec3(0.0);
        if (int(binding.z + 0.5) == 1){
            vec3 outside = u - clamp(u, 0.0, 1.0);
            float d = length(outside);
            float fadeDistance = binding.w;
            if (fadeDistance <= 0.0){
                fade = (d > 0.0) ? 0.0 : 1.0;
            }else{
                fade = 1.0 - smoothstep(0.0, fadeDistance, d);
                if (d > 0.0 && d < fadeDistance){
                    float q = d / fadeDistance;
                    fadeGradient = -(6.0 * q * (1.0 - q) / fadeDistance) * outside / d;
                }
            }
        }
        if (fade <= 0.0)
            continue;

        vec4 grid = latticeTexel(start + 8); // rx, ry, rz, interpolation
        ivec3 r = ivec3(grid.xyz + 0.5);
        int interpolation = int(grid.w + 0.5);
        int fx, fy, fz;
        vec4 wx, wy, wz, dwx, dwy, dwz;
        latticeAxis(u.x, r.x, interpolation, fx, wx, dwx);
        latticeAxis(u.y, r.y, interpolation, fy, wy, dwy);
        latticeAxis(u.z, r.z, interpolation, fz, wz, dwz);
        int nodes = (interpolation == 2) ? 4 : 2;
        int strideY = r.x + 2;
        int strideZ = strideY * (r.y + 2);
        int gridStart = start + LATTICE_RECORD_HEADER + (fx + 1) + strideY * (fy + 1) + strideZ * (fz + 1);

        // the offset field D and its derivatives along x, y, z (spec 3.2)
        vec3 field = vec3(0.0);
        vec3 fieldX = vec3(0.0);
        vec3 fieldY = vec3(0.0);
        vec3 fieldZ = vec3(0.0);
        for (int k = 0; k < 4; k++){
            if (k >= nodes)
                break;
            for (int j = 0; j < 4; j++){
                if (j >= nodes)
                    break;
                float wyz = wy[j] * wz[k];
                float dyz = dwy[j] * wz[k];
                float ydz = wy[j] * dwz[k];
                int row = gridStart + strideY * j + strideZ * k;
                for (int i = 0; i < 4; i++){
                    if (i >= nodes)
                        break;
                    vec3 offset = latticeTexel(row + i).xyz;
                    field += (wx[i] * wyz) * offset;
                    fieldX += (dwx[i] * wyz) * offset;
                    fieldY += (wx[i] * dyz) * offset;
                    fieldZ += (wx[i] * ydz) * offset;
                }
            }
        }

        // J - I = w m (f dD/du + D (x) grad f)
        mat3 deformation = weight * mat3(fade * fieldX + field * fadeGradient.x,
                                         fade * fieldY + field * fadeGradient.y,
                                         fade * fieldZ + field * fadeGradient.z);
        // where the field is flat at zero (away from the moved points) the vertex is at rest
        if (field == vec3(0.0) && deformation[0] == vec3(0.0) && deformation[1] == vec3(0.0) && deformation[2] == vec3(0.0))
            continue;

        mat3 toObject = mat3(latticeTexel(start + 4).xyz, latticeTexel(start + 5).xyz, latticeTexel(start + 6).xyz);
        position += toObject * ((weight * fade) * field);
        // the Jacobian into object space
        mat3 jacobian = mat3(1.0) + toObject * deformation * mat3(toNormalized);
        // cofactor: (j2 x j3, j3 x j1, j1 x j2), safe when the cell is folded flat
        mat3 cofactor = mat3(cross(jacobian[1], jacobian[2]), cross(jacobian[2], jacobian[0]), cross(jacobian[0], jacobian[1]));

        vec3 movedNormal = cofactor * normal;
        if (dot(movedNormal, movedNormal) > 0.0)
            normal = normalize(movedNormal);
        vec3 movedSmoothNormal = cofactor * smoothNormal;
        if (dot(movedSmoothNormal, movedSmoothNormal) > 0.0)
            smoothNormal = normalize(movedSmoothNormal);
        vec3 movedTangent = jacobian * tangent;
        if (dot(movedTangent, movedTangent) > 0.0)
            tangent = normalize(movedTangent);
        if (wantStretch)
            stretch *= dot(jacobian[0], cofactor[0]);
    }
}

void latticeDeform(int stage, inout vec3 position, inout vec3 normal, inout vec3 tangent, inout float stretch){
    vec3 smoothNormal = normal;
    latticeDeformSmooth(stage, position, normal, smoothNormal, tangent, stretch);
}

// v_latticeStretch, with LATTICE_STRETCH (nothing without)
void latticeWriteStretch(float stretch){
    #ifdef LATTICE_STRETCH
        v_latticeStretch = stretch;
    #endif
}

#endif
