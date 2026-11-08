// The vertex stage of particle systems (shader type particles, docs/specs/S20-particles.md
// D6, R11, R12): the built-in particles.vert and every library particles shader include only
// this. Each instance is one particle of the 32-byte record (particles/ParticleRender.h) and
// the static quad corner; PARTICLE_MESH draws a mesh per instance instead, PARTICLE_TRAIL a
// trail or ribbon strip (one vertex per side of each point, per vertex, no instancing).
//
// Outputs (includes/particles_fs.glsl reads them, in this order):
//   v_uv          the texture coordinate of the frame (sprite sheet rect applied)
//   v_color       the particle's colour, linear, straight alpha
//   v_uvNext      FRAME_BLEND: the next frame's coordinate; v_frameBlend its weight
//   v_light       LIT: the light the vertex receives (rgb), SIXWAY: the other lights
//   v_basis*      SIXWAY: the sprite's right, up and normal (world), for the main light per pixel
//   v_emissive    the emission intensity
//   v_custom      CUSTOM_DATA: the Custom Data module's four values
//   v_worldPosition RECEIVE_SHADOWS: the vertex in world space
//
// Render modes (particlesParams.mode.x, the Renderer module's): 0 Billboard (the camera
// plane), 1 Facing Camera Position, 2 Stretched (STRETCH), 3 Horizontal, 4 Vertical, 5 World
// oriented, 6 Local oriented, 7 Mesh (PARTICLE_MESH), 9 Velocity aligned (8, None, draws no
// particles). Rotation is clockwise as seen.

#define FRAME_VS
// a pixel particle variant (S49, 'Pxl') reads the pass's pixel-snap grid whenever a Pixel Perfect
// Camera gives one: its frame block declares the grid as the 'Pxs' variant's does
#if defined(PARTICLE_PIXEL) && !defined(PIXEL_SNAP)
    #define PIXEL_SNAP
#endif
#include "includes/frame.glsl"
#include "includes/srgb.glsl"
// a pixel-perfect camera's particles (S41, 'Pxs'): each particle's centre on the pixel grid, and a
// quad turned by a multiple of 90 degrees with every corner on it (S49's Pixel Art module snaps
// the lower-left corner of its pixel box with the same pixelSnapRound)
#ifdef PIXEL_SNAP
    #include "includes/pixel_snap.glsl"
#endif

uniform u_vs_particlesParams {
    mat4 modelMatrix;     // simulation space to world (the emitter's transform, or identity for World simulation)
    vec4 mode;            // x render mode, y vertical lock (0 world, 1 emitter), z frames in the sheet, w sheet (0 none, 1 grid, 2 sprites)
    vec4 sheet;           // x tiles X, y tiles Y, z mesh align to velocity (1), w trail texture tiles
    vec4 stretch;         // x camera scale, y speed scale, z length scale, w freeze in view (1)
    vec4 pivot;           // xyz the pivot, in particle sizes; w 1 when the trail is in world space
    vec4 screenSize;      // x min, y max screen size (fractions of the view height), z perspective (1) or ortho (0), w view height of one unit at depth 1 (2 tan(fov/2), ortho: the view height)
    vec4 cameraVelocity;  // xyz the camera's velocity (world, per second)
    vec4 tiling;          // the texture's UVs: xy scale, zw offset (the Renderer's Tiling and Offset)
    #if defined(LIT) || defined(SIXWAY)
        vec4 lightPosition[MAX_PARTICLE_LIGHTS];  // xyz world position, w range (0: no light)
        vec4 lightColor[MAX_PARTICLE_LIGHTS];     // rgb colour times intensity (linear)
    #endif
    #ifdef HAS_TEXTURE
        vec4 frameRects[64];  // Sprites: each frame's rect (xy origin, zw size, UV)
    #endif
    #ifdef PARTICLE_PIXEL
        // pixel particles (S49): the box on the grid
        vec4 pixel;           // x the shape (-1: a billboard of the Pixel Art module), y Snap to Pixel Grid, z Sizes in Pixels, w Integer Size
        vec4 pixelGrid;       // x world units per base pixel (orthographic, no Pixel Perfect Camera: 1 / PPU), y rotation (0 Steps90, 1 Rotate On Grid), z perspective (1), w Shard / Chunk rotation steps
        vec4 pixelStreak;     // x Length Scale, y Max Length, z Width
    #endif
    #ifdef PARTICLE_ANALYTIC
        // Analytic (S49): particles::AnalyticBlock, member for member
        vec4 analyticSlots;
        vec4 analyticTiming;
        vec4 analyticFrame;
        vec4 analyticKeys[6];   // 16-bit halves (a block of one type flattens on GL): analyticKey()
        vec4 analyticStart;
        vec4 analyticStart2;
        vec4 analyticColor0;
        vec4 analyticColor1;
        vec4 analyticMotion;
        vec4 analyticVelocity;
        vec4 analyticShape;
        vec4 analyticShape2;
        vec4 analyticShape3;
        vec4 analyticShape4;
        vec4 analyticShapeXform[3];
        vec4 analyticShapeRot[3];
        vec4 analyticSheet;
        vec4 analyticWobble;
        vec4 analyticPixel;
        vec4 analyticRamp;
        vec4 analyticRemap;
        vec4 analyticExtra;
        vec4 analyticBursts[4];
        vec4 analyticBurstSlots[4];
        vec4 analyticSizeCurve[8];
        vec4 analyticFadeCurve[8];
        vec4 analyticColorTable[32];
    #endif
} particlesParams;

#ifdef PARTICLE_MESH
    in vec3 a_position;
    in vec3 a_normal;
    in vec2 a_texcoord1;
    in vec4 i_particleRotation;     // the mesh's rotation (quaternion)
#elif !defined(PARTICLE_TRAIL)
    in vec2 a_corner;               // the quad corner, -0.5 .. 0.5
#endif
#ifdef PARTICLE_ANALYTIC
    // Analytic (S49 R15-R19): no record; each instance is a slot, its particle computed from the
    // slot and the time into the values the record would hold (includes/particles_analytic_vs.glsl)
    in float i_particleSlot;
    vec3 i_particlePosition;
    vec4 i_particleColor;
    vec2 i_particleSize;
    vec2 i_particleRotFrame;
    vec4 i_particleVelocity;
    vec4 i_particlePixel;
#else
in vec3 i_particlePosition;         // trails: the point
in vec4 i_particleColor;            // rgb sRGB-encoded (gamma 2.2), a linear
#ifndef PARTICLE_TRAIL
    in vec2 i_particleSize;
#endif
in vec2 i_particleRotFrame;         // x rotation / 2pi, y frame / frame count (blend in the fraction); trails: x u, y side
in vec4 i_particleVelocity;         // xyz velocity (simulation space), w emission (mesh: the size z); trails: xyz tangent, w half width
#ifdef CUSTOM_DATA
    in vec4 i_particleCustom;
#endif
#ifdef PARTICLE_PIXEL
    in vec4 i_particlePixel;        // UBYTE4N: x the ramp step (1: none), y normalized age, z phase, w flags (S49 R10)
#endif
#endif

out vec2 v_uv;
out vec4 v_color;
#ifdef FRAME_BLEND
    out vec2 v_uvNext;
    out float v_frameBlend;
#endif
#if defined(LIT) || defined(SIXWAY)
    out vec3 v_light;
#endif
#if defined(LIT) && !defined(SIXWAY)
    out vec3 v_mainLight;
#endif
#ifdef SIXWAY
    out vec3 v_basisRight;
    out vec3 v_basisUp;
    out vec3 v_basisNormal;
#endif
out float v_emissive;
#ifdef CUSTOM_DATA
    out vec4 v_custom;
#endif
#ifdef RECEIVE_SHADOWS
    out vec3 v_worldPosition;
#endif
#ifdef PARTICLE_PIXEL
    flat out vec4 v_pixelBox;
    flat out vec4 v_pixelLine;
    flat out vec4 v_pixelData;
    out vec2 v_pixelLocal;
    out vec2 v_pixelGrid;
#endif

vec3 particleSafeNormalize(vec3 v, vec3 fallback){
    float len = length(v);
    return len > 1e-6 ? v / len : fallback;
}

// the rect of a frame of the sheet (xy origin, zw size, UV with v down)
vec4 particleFrameRect(float frame){
    #ifdef HAS_TEXTURE
        if (particlesParams.mode.w > 1.5){
            int index = int(clamp(frame, 0.0, particlesParams.mode.z - 1.0));
            return particlesParams.frameRects[index];
        }
    #endif
    if (particlesParams.mode.w > 0.5){
        float tilesX = particlesParams.sheet.x;
        float tilesY = particlesParams.sheet.y;
        float column = mod(frame, tilesX);
        float row = floor(frame / tilesX);
        return vec4(column / tilesX, row / tilesY, 1.0 / tilesX, 1.0 / tilesY);
    }
    return vec4(0.0, 0.0, 1.0, 1.0);
}

// the frame coordinates of a texture coordinate inside the particle (0..1, v down)
void particleFrameUVs(vec2 local, float frame01){
    float frames = max(particlesParams.mode.z, 1.0);
    float frame = frame01 * frames;
    float first = floor(frame);
    vec4 rect = particleFrameRect(first);
    v_uv = (rect.xy + local * rect.zw) * particlesParams.tiling.xy + particlesParams.tiling.zw;
    #ifdef FRAME_BLEND
        float next = mod(first + 1.0, frames);
        vec4 nextRect = particleFrameRect(next);
        v_uvNext = (nextRect.xy + local * nextRect.zw) * particlesParams.tiling.xy + particlesParams.tiling.zw;
        v_frameBlend = frame - first;
    #endif
}

// the main directional light a vertex with this world normal receives (LIT), with a soft wrap:
// thin particles let light through
vec3 particleMainLight(vec3 normal){
    float ndl = (dot(normal, frame.mainLightDirection.xyz) + 0.5) / 1.5;
    return frame.mainLightColor.rgb * frame.mainLightDirection.w * max(ndl, 0.0);
}

// the rest of the light a vertex receives (LIT): the ambient and the nearest point lights
vec3 particleVertexLight(vec3 position, vec3 normal){
    vec3 light = frame.ambient.rgb;
    #ifdef LIT
        for (int i = 0; i < MAX_PARTICLE_LIGHTS; i++){
            vec4 lp = particlesParams.lightPosition[i];
            if (lp.w <= 0.0)
                continue;
            vec3 toLight = lp.xyz - position;
            float distanceSq = dot(toLight, toLight);
            float window = clamp(1.0 - pow(distanceSq / (lp.w * lp.w), 2.0), 0.0, 1.0);
            float attenuation = window * window / max(distanceSq, 0.0001);
            float ndl = (dot(normal, toLight * inversesqrt(max(distanceSq, 1e-8))) + 0.5) / 1.5;
            light += particlesParams.lightColor[i].rgb * attenuation * max(ndl, 0.0);
        }
    #endif
    return light;
}

// the other lights of SIXWAY (per vertex): their colour (the main light is per pixel)
vec3 particleOtherLights(vec3 position){
    vec3 light = vec3(0.0);
    #ifdef SIXWAY
        for (int i = 0; i < MAX_PARTICLE_LIGHTS; i++){
            vec4 lp = particlesParams.lightPosition[i];
            if (lp.w <= 0.0)
                continue;
            vec3 toLight = lp.xyz - position;
            float distanceSq = dot(toLight, toLight);
            float window = clamp(1.0 - pow(distanceSq / (lp.w * lp.w), 2.0), 0.0, 1.0);
            light += particlesParams.lightColor[i].rgb * window * window / max(distanceSq, 0.0001);
        }
    #endif
    return light;
}

// a size clamped to the Renderer's min and max screen size at this view depth
vec2 particleScreenClamp(vec2 size, vec3 worldCenter){
    float height = particlesParams.screenSize.w;
    if (particlesParams.screenSize.z > 0.5){
        float depth = max(-(frame.viewMatrix * vec4(worldCenter, 1.0)).z, 0.0001);
        height *= depth;
    }
    float extent = max(abs(size.x), abs(size.y));
    if (extent <= 0.0 || height <= 0.0)
        return size;
    float share = extent / height;
    float clamped = clamp(share, particlesParams.screenSize.x, particlesParams.screenSize.y);
    return size * (clamped / share);
}

vec3 rotateByQuaternion(vec4 q, vec3 v){
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

#ifdef PARTICLE_ANALYTIC
    #include "includes/particles_analytic_vs.glsl"
#endif

#ifdef PARTICLE_PIXEL
// The base-pixel grid of the box (S49 R2): a Pixel Perfect Camera's (S41's terms, frame.pixelSnap),
// else 1 / PPU world units in view space anchored at its origin (orthographic), else the
// screen's pixels (perspective: only the projected centre snaps)
vec2 pixelGridFromWorld(vec3 world){
    vec4 clip = frame.viewProjectionMatrix * vec4(world, 1.0);
    if (frame.pixelSnap.x > 0.0)
        return pixelSnapToPixels(clip);
    if (particlesParams.pixelGrid.z < 0.5)
        return (frame.viewMatrix * vec4(world, 1.0)).xy / particlesParams.pixelGrid.x;
    return (clip.xy / clip.w * 0.5 + 0.5) * frame.resolution.xy;
}

vec4 pixelGridToClip(vec2 p, vec3 worldCenter, vec4 clipCenter){
    if (frame.pixelSnap.x > 0.0)
        return pixelSnapFromPixels(p, clipCenter);
    if (particlesParams.pixelGrid.z < 0.5){
        vec4 view = frame.viewMatrix * vec4(worldCenter, 1.0);
        return frame.projectionMatrix * vec4(p * particlesParams.pixelGrid.x, view.z, 1.0);
    }
    vec2 ndc = p / frame.resolution.xy * 2.0 - 1.0;
    return vec4(ndc * clipCenter.w, clipCenter.zw);
}

// S41's rule on the box's lower-left corner (D4): a box of n pixels covers whole pixels
vec2 pixelCorner(vec2 centre, vec2 box, bool snap){
    vec2 c = centre - box * 0.5;
    return snap ? vec2(pixelSnapRound(c.x), pixelSnapRound(c.y)) : c;
}

// a unit axis held to 1/65536, so the CPU twin and every backend round the same products
vec2 pixelAxis(float angle){
    return floor(vec2(cos(angle), sin(angle)) * 65536.0 + 0.5) / 65536.0;
}

// The box of a shape (R9's table) on the grid, and Streak's and Shard's line in it
// (ParticlePixel.cpp pixelLayout is the CPU twin): shape -1 is a billboard's n x n
void pixelLayout(int shape, vec2 centre, vec2 n, vec2 velocityPx, float rotation, bool snap,
                 out vec2 box, out vec2 corner, out vec4 pxLine, out float shapeAngle){
    pxLine = vec4(0.0);
    shapeAngle = 0.0;
    if (shape == 4){
        // Streak: from the head (the particle) back along the velocity seen, one pixel a step
        float speed = length(velocityPx);
        float len = clamp(1.0 + speed * particlesParams.pixelStreak.x, 1.0, particlesParams.pixelStreak.y);
        vec2 dir = speed > 1e-6 ? velocityPx / speed : vec2(0.0);
        vec2 d = floor(-dir * (len - 1.0) + 0.5);
        vec2 head = pixelCorner(centre, vec2(1.0), snap);
        int width = int(particlesParams.pixelStreak.z + 0.5);
        bool xMajor = abs(d.x) >= abs(d.y);
        float low = float((width - 1) / 2);
        box = abs(d) + 1.0 + (xMajor ? vec2(0.0, float(width - 1)) : vec2(float(width - 1), 0.0));
        corner = head + min(d, vec2(0.0)) - (xMajor ? vec2(0.0, low) : vec2(low, 0.0));
        pxLine = vec4(head - corner, head + d - corner);
        return;
    }
    float size = n.x;
    if (shape == 0)
        box = ceil(n);
    else if (shape == 1)
        box = vec2(floor(size) + 1.0 - mod(floor(size), 2.0));   // odd: an even size rounds up
    else if (shape == 5)
        box = vec2(ceil(1.6 * size));
    else if (shape == 6)
        box = vec2(ceil(size) + 2.0);
    else if (shape == 7)
        box = vec2(ceil(1.3 * size) + 2.0);
    else if (shape > 0)
        box = vec2(ceil(size));
    else
        box = n;
    corner = pixelCorner(centre, box, snap);
    if (shape == 6 || shape == 7){
        // the particle's rotation (clockwise as seen) to rotationSteps a turn, counter-clockwise here
        float steps = particlesParams.pixelGrid.w;
        shapeAngle = -floor(rotation / 6.28318530718 * steps + 0.5) * 6.28318530718 / steps;
        if (shape == 6){
            vec2 dir = pixelAxis(shapeAngle);
            float halfLength = (size - 1.0) * 0.5;
            vec2 c = box * 0.5;
            pxLine = vec4(floor(c - dir * halfLength), floor(c + dir * halfLength));
        }
    }
}
#endif

void main() {
    #ifdef PARTICLE_ANALYTIC
        // a dead slot draws nothing: its quad collapses outside the clip volume
        if (!analyticParticle(i_particleSlot)){
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
    #endif
    vec4 linearColor = vec4(sRGBToLinear(i_particleColor.rgb), i_particleColor.a);
    v_color = linearColor;
    v_emissive = i_particleVelocity.w;
    #ifdef CUSTOM_DATA
        v_custom = i_particleCustom;
    #endif

    vec3 cameraRight = vec3(frame.viewMatrix[0][0], frame.viewMatrix[1][0], frame.viewMatrix[2][0]);
    vec3 cameraUp = vec3(frame.viewMatrix[0][1], frame.viewMatrix[1][1], frame.viewMatrix[2][1]);
    vec3 cameraBack = vec3(frame.viewMatrix[0][2], frame.viewMatrix[1][2], frame.viewMatrix[2][2]);

    vec3 worldPosition;
    vec3 normal;
    vec3 right = cameraRight;
    vec3 up = cameraUp;
    #ifdef PIXEL_SNAP
        vec3 snapCenter = vec3(0.0);
        bool snapCorners = false;
    #endif

#if defined(PARTICLE_TRAIL)
    // a trail point: the centre pushed out sideways, across the tangent and the view
    vec3 center = i_particlePosition;
    vec3 tangent = i_particleVelocity.xyz;
    if (particlesParams.pivot.w < 0.5){
        center = (particlesParams.modelMatrix * vec4(center, 1.0)).xyz;
        tangent = mat3(particlesParams.modelMatrix) * tangent;
    }
    vec3 toCamera = frame.cameraPosition.w > 0.5 ? cameraBack : particleSafeNormalize(frame.cameraPosition.xyz - center, cameraBack);
    vec3 side = particleSafeNormalize(cross(tangent, toCamera), cameraRight);
    float sideSign = i_particleRotFrame.y > 0.5 ? 1.0 : -1.0;
    worldPosition = center + side * (sideSign * i_particleVelocity.w);
    normal = toCamera;
    right = side;
    up = particleSafeNormalize(tangent, cameraUp);
    // u along the trail (Tile repeats it), v across
    float u = i_particleRotFrame.x * max(particlesParams.sheet.w, 1.0);
    v_uv = vec2(u, i_particleRotFrame.y);
    #ifdef FRAME_BLEND
        v_uvNext = v_uv;
        v_frameBlend = 0.0;
    #endif
    v_emissive = 0.0;
#elif defined(PARTICLE_MESH)
    vec3 size = vec3(i_particleSize, i_particleVelocity.w);
    v_emissive = 0.0;
    vec3 local = (a_position + particlesParams.pivot.xyz) * size;
    vec3 localNormal = a_normal;
    vec4 rotation = i_particleRotation;
    float rotationLength = length(rotation);
    rotation = rotationLength > 1e-6 ? rotation / rotationLength : vec4(0.0, 0.0, 0.0, 1.0);
    local = rotateByQuaternion(rotation, local);
    localNormal = rotateByQuaternion(rotation, localNormal);
    if (particlesParams.sheet.z > 0.5){
        // Align to Velocity: the mesh's +Z along the velocity
        vec3 forward = particleSafeNormalize(i_particleVelocity.xyz, vec3(0.0, 0.0, 1.0));
        vec3 helper = abs(forward.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 axisX = normalize(cross(helper, forward));
        vec3 axisY = cross(forward, axisX);
        mat3 align = mat3(axisX, axisY, forward);
        local = align * local;
        localNormal = align * localNormal;
    }
    worldPosition = (particlesParams.modelMatrix * vec4(i_particlePosition + local, 1.0)).xyz;
    #ifdef PIXEL_SNAP
        snapCenter = (particlesParams.modelMatrix * vec4(i_particlePosition, 1.0)).xyz;
    #endif
    normal = particleSafeNormalize(mat3(particlesParams.modelMatrix) * localNormal, cameraBack);
    right = particleSafeNormalize(mat3(particlesParams.modelMatrix) * rotateByQuaternion(rotation, vec3(1.0, 0.0, 0.0)), cameraRight);
    up = particleSafeNormalize(mat3(particlesParams.modelMatrix) * rotateByQuaternion(rotation, vec3(0.0, 1.0, 0.0)), cameraUp);
    particleFrameUVs(a_texcoord1, i_particleRotFrame.y);
#elif defined(PARTICLE_PIXEL)
    // the pixel box on the grid (S49 R2, R3, R9): sizes in base pixels, whole-pixel boxes, the
    // lower-left corner snapped; a shape fills it per pixel, a billboard maps its texture on it
    vec3 center = (particlesParams.modelMatrix * vec4(i_particlePosition, 1.0)).xyz;
    vec4 clipCenter = frame.viewProjectionMatrix * vec4(center, 1.0);
    vec3 toCamera = frame.cameraPosition.w > 0.5 ? cameraBack : particleSafeNormalize(frame.cameraPosition.xyz - center, cameraBack);
    vec2 centrePx = pixelGridFromWorld(center);
    #ifdef PARTICLE_ANALYTIC
        centrePx += analyticWobblePx;   // Wobble, before the snap (the Simulated pack adds it to the position)
    #endif
    float probe = particlesParams.pixelGrid.z > 0.5 ? max(clipCenter.w, 1e-4) * 0.01 : 1.0;
    float pxPerWorld = length(pixelGridFromWorld(center + cameraRight * probe) - centrePx) / probe;
    vec2 n = abs(i_particleSize);
    if (particlesParams.pixel.z < 0.5)
        n *= pxPerWorld;
    n = particlesParams.pixel.w > 0.5 ? max(vec2(1.0), floor(n + 0.5)) : max(vec2(1.0), n);
    vec3 worldVelocity = mat3(particlesParams.modelMatrix) * i_particleVelocity.xyz;
    float worldSpeed = length(worldVelocity);
    vec2 velocityPx = worldSpeed > 1e-6 ? (pixelGridFromWorld(center + worldVelocity * (probe / worldSpeed)) - centrePx) * (worldSpeed / probe) : vec2(0.0);
    int shape = int(floor(particlesParams.pixel.x + 0.5));
    bool snap = particlesParams.pixel.y > 0.5;
    float rotation = i_particleRotFrame.x * 6.28318530718;
    int mode = int(particlesParams.mode.x + 0.5);
    vec2 box, corner;
    vec4 pxLine;
    float shapeAngle;
    vec2 gridPosition;
    vec2 uvLocal = a_corner + 0.5;
    if (shape < 0 && (mode == 2 || mode == 9 || (particlesParams.pixelGrid.y > 0.5 && abs(sin(rotation * 2.0)) > 1e-4))){
        // a turned billboard (along the velocity seen; or Rotate On Grid): the quad keeps its angle
        // and its pivot snaps (S41's policy)
        vec2 upPx = length(velocityPx) > 1e-6 ? normalize(velocityPx) : vec2(0.0, 1.0);
        if (mode != 2 && mode != 9)
            upPx = vec2(sin(rotation), cos(rotation));
        vec2 rightPx = vec2(upPx.y, -upPx.x);
        vec2 extent = n;
        float along = a_corner.y;
        if (mode == 2){
            extent.y = n.y * particlesParams.stretch.z + length(velocityPx) * particlesParams.stretch.y;
            along -= 0.5;   // the head at the particle
        }
        vec2 pivot = snap ? vec2(pixelSnapRound(centrePx.x), pixelSnapRound(centrePx.y)) : centrePx;
        box = extent;
        corner = pivot - extent * 0.5;
        pxLine = vec4(0.0);
        shapeAngle = 0.0;
        gridPosition = pivot + rightPx * (a_corner.x * extent.x) + upPx * (along * extent.y);
    }else{
        if (shape < 0){
            // Steps90: the rotation rounded to a quarter turn; the box snaps unturned (an odd
            // quarter swaps its sides) and the texture turns on it
            float quarter = mod(floor(rotation / 1.57079632679 + 0.5), 4.0);
            if (mod(quarter, 2.0) > 0.5)
                n = n.yx;
            if (quarter > 0.5 && quarter < 1.5)
                uvLocal = vec2(uvLocal.y, 1.0 - uvLocal.x);
            else if (quarter > 1.5 && quarter < 2.5)
                uvLocal = 1.0 - uvLocal;
            else if (quarter > 2.5)
                uvLocal = vec2(1.0 - uvLocal.y, uvLocal.x);
        }
        pixelLayout(shape, centrePx, n, velocityPx, rotation, snap, box, corner, pxLine, shapeAngle);
        gridPosition = corner + (a_corner + 0.5) * box;
    }
    v_pixelLocal = (a_corner + 0.5) * box;
    v_pixelGrid = gridPosition;
    v_pixelBox = vec4(box, n.x, shapeAngle);
    v_pixelLine = pxLine;
    float rampStep = floor(i_particlePixel.x * 255.0 + 0.5);
    // the alpha goes to the fragment's fade (the dither compares it); the colour carries 1
    v_pixelData = vec4(rampStep > 254.5 ? -1.0 : rampStep, i_particlePixel.y, i_particleColor.a, i_particlePixel.w * 255.0);
    v_color.a = 1.0;
    // mirrored sizes mirror the texture
    if (i_particleSize.x < 0.0) uvLocal.x = 1.0 - uvLocal.x;
    if (i_particleSize.y < 0.0) uvLocal.y = 1.0 - uvLocal.y;
    worldPosition = center;
    normal = toCamera;
    vec4 pixelClip = pixelGridToClip(gridPosition, center, clipCenter);
    particleFrameUVs(vec2(uvLocal.x, 1.0 - uvLocal.y), i_particleRotFrame.y);
#else
    vec3 center = (particlesParams.modelMatrix * vec4(i_particlePosition, 1.0)).xyz;
    vec2 size = particleScreenClamp(i_particleSize, center);
    int mode = int(particlesParams.mode.x + 0.5);
    vec3 toCamera = frame.cameraPosition.w > 0.5 ? cameraBack : particleSafeNormalize(frame.cameraPosition.xyz - center, cameraBack);
    float angle = i_particleRotFrame.x * 6.28318530718;

    if (mode == 1){
        // facing the camera position: no smear at wide angles
        right = particleSafeNormalize(cross(cameraUp, toCamera), cameraRight);
        up = cross(toCamera, right);
    }else if (mode == 3){
        // horizontal: flat on the world XZ plane, facing up
        right = vec3(1.0, 0.0, 0.0);
        up = vec3(0.0, 0.0, -1.0);
    }else if (mode == 4){
        // vertical: upright (world or emitter Y), facing the camera around it
        vec3 axis = particlesParams.mode.y > 0.5 ? particleSafeNormalize(mat3(particlesParams.modelMatrix) * vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0))
                                                 : vec3(0.0, 1.0, 0.0);
        right = particleSafeNormalize(cross(axis, toCamera), cameraRight);
        up = axis;
    }else if (mode == 5){
        // world oriented: the world XY plane
        right = vec3(1.0, 0.0, 0.0);
        up = vec3(0.0, 1.0, 0.0);
    }else if (mode == 6){
        // local oriented: the emitter's XY plane
        right = particleSafeNormalize(mat3(particlesParams.modelMatrix) * vec3(1.0, 0.0, 0.0), cameraRight);
        up = particleSafeNormalize(mat3(particlesParams.modelMatrix) * vec3(0.0, 1.0, 0.0), cameraUp);
    }else if (mode == 9){
        // velocity aligned: the quad across the velocity (its normal along it), up turned from
        // the world's Y (Unity's Velocity render alignment)
        vec3 forward = particleSafeNormalize(mat3(particlesParams.modelMatrix) * i_particleVelocity.xyz, toCamera);
        vec3 helper = abs(forward.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        right = particleSafeNormalize(cross(helper, forward), cameraRight);
        up = cross(forward, right);
    }

    #ifdef STRETCH
        // stretched: along the velocity as seen (the camera's own motion scaled in), the length
        // the size times the length scale plus the speed times the speed scale
        vec3 velocity = mat3(particlesParams.modelMatrix) * i_particleVelocity.xyz - particlesParams.cameraVelocity.xyz * particlesParams.stretch.x;
        vec3 seen = velocity - dot(velocity, toCamera) * toCamera;
        if (particlesParams.stretch.w > 0.5)
            seen = velocity - dot(velocity, cameraBack) * cameraBack;
        float speed = length(velocity);
        vec3 axis = particleSafeNormalize(seen, cameraUp);
        up = axis;
        right = particleSafeNormalize(cross(axis, toCamera), cameraRight);
        size.y = size.y * particlesParams.stretch.z + speed * particlesParams.stretch.y;
        angle = 0.0;
    #endif

    // the corner, rotated clockwise as seen, around the pivot
    vec2 corner = (a_corner + particlesParams.pivot.xy) * size;
    #ifdef STRETCH
        // the head at the particle: the quad trails behind it
        corner.y -= size.y * 0.5;
    #endif
    float c = cos(angle);
    float s = sin(angle);
    #ifdef PIXEL_SNAP
        // a quarter turn (on a view without roll) is drawn exactly on the grid: the step's axis
        // (pixelSnapRotationStep, the rotated x axis is (c, -s)), and its corners snap one by one
        snapCorners = abs(c * s) < 1e-4 && frame.pixelSnapOffset.z > 0.5;
        #ifdef STRETCH
            snapCorners = false;
        #endif
        if (snapCorners){
            vec2 quarter = pixelSnapRotationStep(vec2(c, -s));
            c = quarter.x;
            s = -quarter.y;
        }
        snapCenter = center;
    #endif
    vec2 rotated = vec2(c * corner.x + s * corner.y, -s * corner.x + c * corner.y);
    worldPosition = center + right * rotated.x + up * rotated.y + toCamera * (particlesParams.pivot.z * max(abs(size.x), abs(size.y)));
    // a rounded normal: lit sprites read as volumes
    normal = particleSafeNormalize(toCamera + (right * a_corner.x + up * a_corner.y), toCamera);
    vec3 basisRight = right * c - up * s;
    vec3 basisUp = right * s + up * c;
    right = basisRight;
    up = basisUp;
    particleFrameUVs(vec2(a_corner.x + 0.5, 0.5 - a_corner.y), i_particleRotFrame.y);
#endif

    #ifdef SIXWAY
        v_basisRight = right;
        v_basisUp = up;
        v_basisNormal = cross(right, up);
        v_light = frame.ambient.rgb + particleOtherLights(worldPosition);
    #elif defined(LIT)
        v_light = particleVertexLight(worldPosition, normal);
        v_mainLight = particleMainLight(normal);
    #endif
    #ifdef RECEIVE_SHADOWS
        v_worldPosition = worldPosition;
    #endif

    #if defined(PARTICLE_PIXEL) && !defined(PARTICLE_MESH) && !defined(PARTICLE_TRAIL)
        gl_Position = pixelClip;
    #else
        gl_Position = frame.viewProjectionMatrix * vec4(worldPosition, 1.0);
    #endif
    #if defined(PIXEL_SNAP) && !defined(PARTICLE_TRAIL) && !defined(PARTICLE_PIXEL)
        // a pass without a Pixel Perfect Camera has no grid (pixelSnap zero): nothing snaps
        if (frame.pixelSnap.x > 0.0){
            gl_Position = snapCorners ? pixelSnapClip(gl_Position)
                                      : gl_Position + pixelSnapCenterDelta(frame.viewProjectionMatrix * vec4(snapCenter, 1.0));
        }
    #endif

    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
