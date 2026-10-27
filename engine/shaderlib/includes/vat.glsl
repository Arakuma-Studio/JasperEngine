// Vertex Animation (VAT): a mesh played from a baked animation texture by its Vertex Animator,
// docs/superpowers/specs/2026-10-27-vertex-animation-design.md sections 2.2, 3.4 and 3.5. The
// frame math is the C++ one of engine/core/animation/VertexAnimation.h (vatSample,
// vatSampleNormalized, vatWeightB, vatUnpackInstance): a change to one is a change to both
// (a GPU readback test compares them).
//
// With HAS_VAT (mesh property 'Vat' or 'Vab', depth 'Vat'/'Vab', G-buffer 'Vat'/'Vab'; the
// variant has no skinning nor morph targets) it declares the payload texture, the u_vs_vat block
// and the attributes it reads, and
//
//   void vatEvaluate()                 // first thing in main(): samples this vertex's animation
//   vec3 vatPosition(vec3 position)    // the rest position (a_position) animated, object space
//   vec3 vatNormal(vec3 normal)        // the rest normal (a_normal) animated (callers normalise)
//   vec3 vatTangent(vec3 tangent)      // the rest tangent (a_tangent.xyz) animated (callers normalise)
//   vec3 vatSmoothNormal(vec3 smoothNormal, vec3 normal)  // a_smoothNormal turned with the vertex
//
// which take the place of the morph targets and the skinning at the start of the vertex path,
// so lattices (stage 1), the instance matrix, the instance fade and the SURFACE_DISPLACE hooks
// apply after them. Without HAS_VAT they return their argument and vatEvaluate does nothing, so
// every vertex entry point calls them unconditionally; the colour, depth and G-buffer stages use
// the same expressions (invariant gl_Position, depth pre-pass, shadows).
//
// Modes:
// - Vertices (HAS_VAT): per vertex (a_vatIndex, the glTF _VAT_INDEX attribute) and frame the
//   position offset from the rest position (absolute when the clip has "rest": "none") and,
//   when baked, a tangent-frame quaternion taking +X, +Y, +Z to tangent, bitangent, normal. The
//   tangent's sign stays a_tangent.w.
// - Bones (HAS_VAT_BONES): per bone and frame a rotation quaternion and a translation + uniform
//   scale, the skin matrix T * R * S of the influences in a_boneIds / a_boneWeights (normAdjust
//   as includes/skinning.glsl), blended and applied as linear blend skinning. Quaternions are
//   nlerped, translation and scale lerped, so a fast-spinning piece never shrinks between frames.
//
// Two frames are mixed per track (alpha), two tracks A and B (a crossfade or a fixed blend with
// B phase-synced to A) by weightB. A clip index outside the table (-1) is the rest pose. The
// playback state is per instance (i_vat0, i_vat1, in their own instance buffer) with
// HAS_INSTANCING, else the entity's (vatState0, vatState1); both are vat::vatPackInstance:
//   v0 = (a.clip, a.start, a.speed, b.clip)
//   v1 = (b.start, b.speed, fadeStart, blend >= 0 ? -(1 + blend) : fadeEnd)
//
// The payload is one RGBA16F/RGBA32F texture read with texelFetch (never filtered), texel t at
// (t % width, t / width): Vertices t = (frame * vertices + vertex) * texelsPerSample + k (k 0
// offset, 1 tangent frame), Bones t = (frame * bones + bone) * 2 + k (k 0 rotation, 1
// translation + scale), frame = clip start + local frame. Everything is highp (the vertex stage's
// default precision on GLES 3 / WebGL2).

#ifndef VAT_MAX_CLIPS
    // animation/VertexAnimation.h (the compiler defines it from there)
    #define VAT_MAX_CLIPS 64
#endif

#ifdef HAS_VAT

#ifdef HAS_VAT_BONES
    in vec4 a_boneWeights;
    in vec4 a_boneIds;
#else
    in float a_vatIndex;
#endif

#ifdef HAS_INSTANCING
    in vec4 i_vat0;
    in vec4 i_vat1;
#endif

// @image_sample_type u_vatTexture unfilterable_float
uniform texture2D u_vatTexture;
// @sampler_type u_vat_smp nonfiltering
uniform sampler u_vat_smp;

// floats only (see the note on custom blocks in mesh.vert)
uniform u_vs_vat {
    vec4 vatTime;       // x now (seconds, the Vertex Animator's clock), y bone index scale, z bone weight scale (normAdjust), w clips in the table
    vec4 vatLayout;     // x texels per row, y vertices (Vertices) or bones (Bones) per frame, z texels per sample, w 1 = a tangent frame per sample
    vec4 vatState0;     // the entity's tracks (vat::vatPackInstance), read without HAS_INSTANCING
    vec4 vatState1;
    vec4 vatClips[VAT_MAX_CLIPS]; // start, frames, fps, flags: wrap (0 loop, 1 once or clamp forever, 2 ping-pong) + 4 interpolate + 8 rest none
};

// one track's two frames (vat::VatFrameSample): global frames and the weight of frame1
struct VatFrames {
    int frame0;
    int frame1;
    float alpha;
    float normalized;
};

// vat::VatTrackPair
struct VatTracks {
    int clipA;
    float startA;
    float speedA;
    int clipB;
    float startB;
    float speedB;
    float fadeStart;
    float fadeEnd;
    float blend;
};

vec4 vatTexel(int index){
    int width = int(vatLayout.x + 0.5);
    return texelFetch(sampler2D(u_vatTexture, u_vat_smp), ivec2(index % width, index / width), 0);
}

int vatClipFlags(vec4 clip){
    return int(clip.w + 0.5);
}

// the frames at a local position x (in samples since the clip's first frame, not wrapped yet)
// of a clip (VertexAnimation.cpp's sampleAt, which vat::vatSample and vatSampleNormalized share)
VatFrames vatFramesAt(vec4 clip, float x){
    VatFrames f;
    int flags = vatClipFlags(clip);
    int wrap = flags & 3;
    float frames = max(floor(clip.y + 0.5), 1.0);
    float last = frames - 1.0;
    float f0;
    float f1;
    if (wrap == 0){
        x = x - frames * floor(x / frames);
        // float rounding of a tiny negative x lands on `frames`: the same phase as 0, kept
        // continuous by f0 = last with alpha 1 (= the first frame)
        x = clamp(x, 0.0, frames);
        f0 = min(floor(x), last);
        f1 = (f0 + 1.0 == frames) ? 0.0 : f0 + 1.0;
        f.normalized = x / frames;
    }else{
        if (wrap == 2){
            float p = 2.0 * last;
            float y = (p > 0.0) ? x - p * floor(x / p) : 0.0;
            x = (y <= last) ? y : p - y;
        }
        x = clamp(x, 0.0, last);
        f0 = floor(x);
        f1 = min(f0 + 1.0, last);
        f.normalized = (last > 0.0) ? x / last : 0.0;
    }
    f.alpha = ((flags & 4) != 0 && frames > 1.0) ? x - f0 : 0.0;
    int start = int(clip.x + 0.5);
    f.frame0 = start + int(f0);
    f.frame1 = start + int(f1);
    return f;
}

// vat::vatSample: the clip at local time tau (seconds, (now - start) * speed)
VatFrames vatSample(vec4 clip, float tau){
    return vatFramesAt(clip, tau * clip.z);
}

// vat::vatSampleNormalized: the clip at a phase (0..1) instead of a time (phase-synced B):
// x = phase * frames for a loop clip, phase * (frames - 1) for the others
VatFrames vatSampleNormalized(vec4 clip, float normalized){
    float frames = max(floor(clip.y + 0.5), 1.0);
    bool loop = (vatClipFlags(clip) & 3) == 0;
    return vatFramesAt(clip, normalized * (loop ? frames : frames - 1.0));
}

// vat::vatUnpackInstance (v1.w < 0 only in blend mode: vatPackInstance writes max(fadeEnd, 0))
VatTracks vatUnpackInstance(vec4 v0, vec4 v1){
    VatTracks t;
    t.clipA = int(floor(v0.x + 0.5));
    t.startA = v0.y;
    t.speedA = v0.z;
    t.clipB = int(floor(v0.w + 0.5));
    t.startB = v1.x;
    t.speedB = v1.y;
    t.fadeStart = v1.z;
    if (v1.w < 0.0){
        t.blend = -v1.w - 1.0;
        t.fadeEnd = 0.0;
    }else{
        t.blend = -1.0;
        t.fadeEnd = v1.w;
    }
    return t;
}

bool vatValidClip(int clip){
    return clip >= 0 && clip < int(vatTime.w + 0.5);
}

// vat::vatWeightB
float vatWeightB(VatTracks t, float now){
    if (!vatValidClip(t.clipB))
        return 0.0;
    if (t.blend >= 0.0)
        return clamp(t.blend, 0.0, 1.0);
    if (t.fadeEnd > t.fadeStart)
        return clamp((now - t.fadeStart) / (t.fadeEnd - t.fadeStart), 0.0, 1.0);
    return 1.0;
}

// q1 on q0's side (both describe the same rotation), mixed, normalised
vec4 vatNlerp(vec4 q0, vec4 q1, float t){
    if (dot(q0, q1) < 0.0)
        q1 = -q1;
    return normalize(mix(q0, q1, t));
}

vec3 vatRotate(vec4 q, vec3 v){
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// the two tracks: sampled (false for a track at rest, B unused or A fully faded out), their
// clips and frames, and B's weight
bool vatTrackA;
bool vatTrackB;
int vatClipA;
int vatClipB;
VatFrames vatFramesA;
VatFrames vatFramesB;
float vatWeight;
// vatEvaluate ran (a fork that never calls it draws the rest pose)
bool vatReady = false;

void vatEvaluateTracks(){
    #ifdef HAS_INSTANCING
        VatTracks t = vatUnpackInstance(i_vat0, i_vat1);
    #else
        VatTracks t = vatUnpackInstance(vatState0, vatState1);
    #endif
    float now = vatTime.x;
    vatClipA = t.clipA;
    vatClipB = t.clipB;
    vatWeight = vatWeightB(t, now);
    vatTrackA = vatValidClip(t.clipA) && vatWeight < 1.0;
    vatTrackB = vatWeight > 0.0;
    if (vatValidClip(t.clipA))
        vatFramesA = vatSample(vatClips[t.clipA], (now - t.startA) * t.speedA);
    else
        vatFramesA = VatFrames(0, 0, 0.0, 0.0);
    if (vatTrackB){
        if (t.blend >= 0.0)
            vatFramesB = vatSampleNormalized(vatClips[t.clipB], vatFramesA.normalized);
        else
            vatFramesB = vatSample(vatClips[t.clipB], (now - t.startB) * t.speedB);
    }else{
        vatFramesB = VatFrames(0, 0, 0.0, 0.0);
    }
}

#ifdef HAS_VAT_BONES

mat4 vatSkin;

// one bone of one track: rotation (nlerp of the two frames) and translation + scale (lerp)
void vatBoneTrack(VatFrames f, int bone, out vec4 rotation, out vec4 translationScale){
    int bones = int(vatLayout.y + 0.5);
    int t0 = (f.frame0 * bones + bone) * 2;
    int t1 = (f.frame1 * bones + bone) * 2;
    rotation = vatNlerp(vatTexel(t0), vatTexel(t1), f.alpha);
    translationScale = mix(vatTexel(t0 + 1), vatTexel(t1 + 1), f.alpha);
}

// T(translation) * R(rotation) * S(scale)
mat4 vatBoneMatrix(vec4 q, vec4 ts){
    float x = q.x, y = q.y, z = q.z, w = q.w;
    mat3 r = mat3(
        1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y),
        2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x),
        2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y)) * ts.w;
    return mat4(vec4(r[0], 0.0), vec4(r[1], 0.0), vec4(r[2], 0.0), vec4(ts.xyz, 1.0));
}

// one influence of the vertex added to vatSkin (skipped at weight 0: a rigid piece reads one bone)
void vatInfluence(float boneId, float boneWeight, inout float weightSum){
    float weight = boneWeight * vatTime.z;
    if (weight == 0.0)
        return;
    int bone = int(floor(boneId * vatTime.y + 0.5));
    // the rest pose is the identity: no rotation, no translation, scale 1
    vec4 rotation = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 translationScale = vec4(0.0, 0.0, 0.0, 1.0);
    if (vatTrackA)
        vatBoneTrack(vatFramesA, bone, rotation, translationScale);
    if (vatTrackB){
        vec4 rotationB;
        vec4 translationScaleB;
        vatBoneTrack(vatFramesB, bone, rotationB, translationScaleB);
        rotation = vatNlerp(rotation, rotationB, vatWeight);
        translationScale = mix(translationScale, translationScaleB, vatWeight);
    }
    vatSkin += vatBoneMatrix(rotation, translationScale) * weight;
    weightSum += weight;
}

void vatEvaluate(){
    vatEvaluateTracks();
    vatSkin = mat4(0.0);
    float weightSum = 0.0;
    vatInfluence(a_boneIds.x, a_boneWeights.x, weightSum);
    vatInfluence(a_boneIds.y, a_boneWeights.y, weightSum);
    vatInfluence(a_boneIds.z, a_boneWeights.z, weightSum);
    vatInfluence(a_boneIds.w, a_boneWeights.w, weightSum);
    // a vertex without influences stays where it is
    if (weightSum == 0.0)
        vatSkin = mat4(1.0);
    vatReady = true;
}

vec3 vatPosition(vec3 position){
    if (!vatReady)
        return position;
    // as getSkinPosition (includes/skinning.glsl): the weights need not sum to one
    vec4 skinned = vatSkin * vec4(position, 1.0);
    return skinned.xyz / skinned.w;
}

vec3 vatNormal(vec3 normal){
    return vatReady ? mat3(vatSkin) * normal : normal;
}

vec3 vatTangent(vec3 tangent){
    return vatReady ? mat3(vatSkin) * tangent : tangent;
}

vec3 vatSmoothNormal(vec3 smoothNormal, vec3 normal){
    return vatReady ? mat3(vatSkin) * smoothNormal : smoothNormal;
}

#else // Vertices

vec3 vatOffset;         // the tracks' offsets mixed
float vatRestWeight;    // how much of the rest position they keep (0 for absolute positions)
// the first texel of this vertex's two samples in each track
int vatTexelsA0;
int vatTexelsA1;
int vatTexelsB0;
int vatTexelsB1;
// the tracks' tangent frames, read on the first vatNormal / vatTangent (a depth pass reads none)
bool vatFramesRead = false;
vec4 vatFrameA;
vec4 vatFrameB;

// one track: the first texels of this vertex's two samples and its offset mixed between them
vec3 vatVertexTrack(VatFrames f, out int t0, out int t1){
    int vertices = int(vatLayout.y + 0.5);
    int texels = int(vatLayout.z + 0.5);
    int vertex = int(floor(a_vatIndex + 0.5));
    t0 = (f.frame0 * vertices + vertex) * texels;
    t1 = (f.frame1 * vertices + vertex) * texels;
    return mix(vatTexel(t0).xyz, vatTexel(t1).xyz, f.alpha);
}

void vatEvaluate(){
    vatEvaluateTracks();
    vec3 offsetA = vec3(0.0);
    float restA = 1.0;
    if (vatTrackA){
        offsetA = vatVertexTrack(vatFramesA, vatTexelsA0, vatTexelsA1);
        restA = ((vatClipFlags(vatClips[vatClipA]) & 8) != 0) ? 0.0 : 1.0;
    }
    vec3 offsetB = vec3(0.0);
    float restB = 1.0;
    if (vatTrackB){
        offsetB = vatVertexTrack(vatFramesB, vatTexelsB0, vatTexelsB1);
        restB = ((vatClipFlags(vatClips[vatClipB]) & 8) != 0) ? 0.0 : 1.0;
    }
    vatOffset = mix(offsetA, offsetB, vatWeight);
    vatRestWeight = mix(restA, restB, vatWeight);
    vatReady = true;
}

vec3 vatPosition(vec3 position){
    return vatReady ? position * vatRestWeight + vatOffset : position;
}

// a direction of the tangent frame: the rest one for a track at rest or without a frame
vec3 vatFrameAxis(vec3 rest, vec3 axis){
    if (!vatReady || vatLayout.w < 0.5)
        return rest;
    if (!vatFramesRead){
        if (vatTrackA)
            vatFrameA = vatNlerp(vatTexel(vatTexelsA0 + 1), vatTexel(vatTexelsA1 + 1), vatFramesA.alpha);
        if (vatTrackB)
            vatFrameB = vatNlerp(vatTexel(vatTexelsB0 + 1), vatTexel(vatTexelsB1 + 1), vatFramesB.alpha);
        vatFramesRead = true;
    }
    vec3 a = vatTrackA ? vatRotate(vatFrameA, axis) : rest;
    vec3 b = vatTrackB ? vatRotate(vatFrameB, axis) : rest;
    return mix(a, b, vatWeight);
}

vec3 vatNormal(vec3 normal){
    return vatFrameAxis(normal, vec3(0.0, 0.0, 1.0));
}

vec3 vatTangent(vec3 tangent){
    return vatFrameAxis(tangent, vec3(1.0, 0.0, 0.0));
}

// turned by the shortest rotation from the rest normal to the animated one
vec3 vatSmoothNormal(vec3 smoothNormal, vec3 normal){
    vec3 from = normalize(normal);
    vec3 to = normalize(vatNormal(normal));
    float c = dot(from, to);
    if (c < -0.9999)
        return -smoothNormal;
    vec3 axis = cross(from, to);
    return smoothNormal * c + cross(axis, smoothNormal) + axis * (dot(axis, smoothNormal) / (1.0 + c));
}

#endif // HAS_VAT_BONES

#else // !HAS_VAT

void vatEvaluate(){
}

vec3 vatPosition(vec3 position){
    return position;
}

vec3 vatNormal(vec3 normal){
    return normal;
}

vec3 vatTangent(vec3 tangent){
    return tangent;
}

vec3 vatSmoothNormal(vec3 smoothNormal, vec3 normal){
    return smoothNormal;
}

#endif // HAS_VAT
