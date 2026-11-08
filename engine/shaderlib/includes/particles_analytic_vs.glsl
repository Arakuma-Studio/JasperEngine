// Analytic particles (docs/specs/S49-pixel-particles.md R15-R19, D2, D3): the vertex part of the
// PARTICLE_ANALYTIC variant ('Ana'). Each instance is a slot (i_particleSlot, a shared static
// stream); its particle at the frame's time is a pure function of the slot, the time and the
// analytic* members of u_vs_particlesParams (particles::AnalyticBlock, byte for byte), worked
// out into the values the 32-byte record would hold, so the rest of includes/particles_vs.glsl
// and the fragment stage run unchanged. Nothing is read back and nothing is kept between
// frames. The CPU twin is engine/core/particles/AnalyticParticles.cpp (evaluate): the same
// float32 formulas in the same order, with S20's stateless hash (highp uint) and its keys.
//
// The motion under gravity and linear drag (fly, flyV) and the seamless cycle follow pixelfx's
// src/core/particles.js, MIT License, Copyright (c) 2026 Patch. See NOTICE.md.

#ifndef PARTICLES_ANALYTIC_VS_GLSL
#define PARTICLES_ANALYTIC_VS_GLSL

const float ANALYTIC_TWO_PI = 6.28318530718;

// the wobble the particle sways by, base pixels (the pixel box adds it before it snaps)
vec2 analyticWobblePx = vec2(0.0);

// S20's hashRandom (ParticleKernels.h): a stateless value per key and particle
uint analyticHash(uint key, uint particle){
    uint x = key ^ (particle * 0x9e3779b9u);
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float analyticRandom(uint key, uint particle){
    return float(analyticHash(key, particle) & 0xffffffu) * (1.0 / 16777216.0);
}

float analyticEncode(float linear){
    return linear <= 0.0 ? 0.0 : pow(min(linear, 1.0), 1.0 / 2.2);
}

float analyticDecode(float srgb){
    return srgb <= 0.0 ? 0.0 : pow(srgb, 2.2);
}

vec3 analyticUnitSphere(float u, float v){
    float cy = u * 2.0 - 1.0;
    float r = sqrt(max(1.0 - cy * cy, 0.0));
    float a = v * ANALYTIC_TWO_PI;
    return vec3(r * cos(a), cy, r * sin(a));
}

float analyticCbrt(float x){
    return x <= 0.0 ? 0.0 : pow(x, 1.0 / 3.0);
}

// 32 samples over normalized age, linearly interpolated (fade: 1)
float analyticCurve(float t, bool fade){
    float x = clamp(t, 0.0, 1.0) * 31.0;
    int i = min(int(x), 30);
    float f = x - float(i);
    vec4 qa = fade ? particlesParams.analyticFadeCurve[i / 4] : particlesParams.analyticSizeCurve[i / 4];
    vec4 qb = fade ? particlesParams.analyticFadeCurve[(i + 1) / 4] : particlesParams.analyticSizeCurve[(i + 1) / 4];
    float a = qa[i - (i / 4) * 4];
    float b = qb[(i + 1) - ((i + 1) / 4) * 4];
    return a + (b - a) * f;
}

// pixelfx fly / flyV: after t under g with drag k (the series below k t = 1e-3)
void analyticFly(vec3 p0, vec3 v0, vec3 g, float k, float t, out vec3 position, out vec3 velocity){
    float f, h, e;
    float kt = k * t;
    if (kt < 1.0e-3){
        f = t - k * t * t * 0.5 + k * k * t * t * t * (1.0 / 6.0);
        h = t * t * 0.5 - k * t * t * t * (1.0 / 6.0) + k * k * t * t * t * t * (1.0 / 24.0);
        e = 1.0 - kt + kt * kt * 0.5;
    }else{
        e = exp(-kt);
        f = (1.0 - e) / k;
        h = (t - f) / k;
    }
    position = p0 + v0 * f + g * h;
    velocity = v0 * e + g * f;
}

// the shape's start position and direction (S20's spawnShape for Analytic's shapes)
void analyticShape(uint key, uint index, out vec3 p, out vec3 d){
    #define RND(stream) analyticRandom(key ^ (uint(stream) * 0x27d4eb2fu), index)
    int kind = int(particlesParams.analyticShape.x + 0.5);
    float radius = particlesParams.analyticShape.y;
    float inner = particlesParams.analyticShape.z;
    float arc = particlesParams.analyticShape.w;
    float tanAngle = particlesParams.analyticShape2.x;
    float len = particlesParams.analyticShape2.y;
    float donut = particlesParams.analyticShape2.z;
    int emitFrom = int(particlesParams.analyticShape2.w + 0.5);
    p = vec3(0.0);
    d = vec3(0.0, 1.0, 0.0);
    float angle = RND(1) * arc;
    float sa = sin(angle);
    float ca = cos(angle);
    if (kind == 1 || kind == 2){
        // Sphere, Hemisphere
        float u = RND(2);
        float cy = kind == 1 ? u * 2.0 - 1.0 : u;
        float rr = sqrt(max(1.0 - cy * cy, 0.0));
        d = vec3(rr * ca, cy, rr * sa);
        float inner3 = inner * inner * inner;
        float r = analyticCbrt((1.0 - inner3) * RND(3) + inner3) * radius;
        p = d * r;
    }else if (kind == 5){
        // Circle
        float inner2 = inner * inner;
        float r = sqrt((1.0 - inner2) * RND(3) + inner2) * radius;
        d = vec3(ca, 0.0, sa);
        p = vec3(ca * r, 0.0, sa * r);
    }else if (kind == 3){
        // Cone
        float inner2 = inner * inner;
        bool shell = emitFrom == 1 || emitFrom == 2;
        float rho = shell ? 1.0 : sqrt((1.0 - inner2) * RND(3) + inner2);
        float tilt = rho * tanAngle;
        float inv = 1.0 / sqrt(tilt * tilt + 1.0);
        d = vec3(ca * tilt * inv, inv, sa * tilt * inv);
        float h = 0.0;
        if (emitFrom == 0){
            float u = RND(4);
            if (tanAngle > 1.0e-6){
                float r0 = radius;
                float r1 = radius + len * tanAngle;
                float r03 = r0 * r0 * r0;
                h = (analyticCbrt((r1 * r1 * r1 - r03) * u + r03) - r0) / tanAngle;
            }else{
                h = u * len;
            }
        }
        float at = h * tanAngle + radius;
        p = vec3(ca * rho * at, h, sa * rho * at);
    }else if (kind == 4){
        // Box
        float ux = RND(2) * 2.0 - 1.0;
        float uy = RND(3) * 2.0 - 1.0;
        float uz = RND(4) * 2.0 - 1.0;
        vec3 halfExtent = particlesParams.analyticShape3.xyz;
        if (emitFrom == 0){
            p = vec3(ux, uy, uz) * halfExtent;
        }else{
            float pick = RND(5);
            float sgn = RND(6) < 0.5 ? -1.0 : 1.0;
            vec3 s = vec3(ux, uy, uz);
            float hx = halfExtent.x, hy = halfExtent.y, hz = halfExtent.z;
            if (emitFrom == 1){
                float ax = hy * hz, ay = hx * hz, az = hx * hy, total = ax + ay + az;
                bool onX = pick < (total > 0.0 ? ax / total : 1.0 / 3.0);
                bool onY = !onX && pick < (total > 0.0 ? (ax + ay) / total : 2.0 / 3.0);
                bool onZ = !onX && !onY;
                float depth = RND(7);
                vec3 innerRatio = particlesParams.analyticShape4.xyz;
                if (onX) s.x = sgn * ((1.0 - innerRatio.x) * depth + innerRatio.x);
                if (onY) s.y = sgn * ((1.0 - innerRatio.y) * depth + innerRatio.y);
                if (onZ) s.z = sgn * ((1.0 - innerRatio.z) * depth + innerRatio.z);
            }else{
                float total = hx + hy + hz;
                bool alongX = pick < (total > 0.0 ? hx / total : 1.0 / 3.0);
                bool alongY = !alongX && pick < (total > 0.0 ? (hx + hy) / total : 2.0 / 3.0);
                bool alongZ = !alongX && !alongY;
                float sgn2 = RND(7) < 0.5 ? -1.0 : 1.0;
                s.x = alongX ? ux : sgn;
                s.y = alongY ? uy : (alongX ? sgn : sgn2);
                s.z = alongZ ? uz : sgn2;
            }
            p = s * halfExtent;
        }
    }else if (kind == 6){
        // Donut
        float tube = RND(2) * ANALYTIC_TWO_PI;
        float st = sin(tube);
        float ct = cos(tube);
        float inner2 = inner * inner;
        float rt = sqrt((1.0 - inner2) * RND(3) + inner2) * donut;
        d = vec3(ca * ct, st, sa * ct);
        p = vec3(d.x * rt + ca * radius, d.y * rt, d.z * rt + sa * radius);
    }else if (kind == 7){
        // Edge
        p = vec3((RND(2) * 2.0 - 1.0) * radius, 0.0, 0.0);
    }
    if (particlesParams.analyticShape4.w > 0.0){
        vec3 r = analyticUnitSphere(RND(10), RND(11));
        p = p + r * (RND(12) * particlesParams.analyticShape4.w);
    }
    if (particlesParams.analyticShapeRot[0].w > 0.0){
        float l = length(p);
        vec3 s = l > 1.0e-20 ? p * (1.0 / l) : vec3(0.0);
        d = d + (s - d) * particlesParams.analyticShapeRot[0].w;
    }
    if (particlesParams.analyticShape3.w > 0.0){
        vec3 r = analyticUnitSphere(RND(13), RND(14));
        d = d + (r - d) * particlesParams.analyticShape3.w;
    }
    float dl = length(d);
    d = dl > 1.0e-20 ? d * (1.0 / dl) : vec3(0.0);
    vec3 point = p;
    vec3 dir = d;
    for (int r = 0; r < 3; r++){
        vec4 m = particlesParams.analyticShapeXform[r];
        vec4 q = particlesParams.analyticShapeRot[r];
        p[r] = m.x * point.x + m.y * point.y + m.z * point.z + m.w;
        d[r] = q.x * dir.x + q.y * dir.y + q.z * dir.z;
    }
    #undef RND
}

// Which particle the slot holds now (D3): its identity and age; false when it holds none
bool analyticSlotParticle(uint slot, out uint index, out float age){
    index = 0u;
    age = 0.0;
    uint rateSlots = uint(particlesParams.analyticSlots.x + 0.5);
    uint nLoop = uint(particlesParams.analyticSlots.z + 0.5);
    float rate = particlesParams.analyticTiming.x;
    float duration = particlesParams.analyticTiming.y;
    float phase = particlesParams.analyticFrame.x;
    float loops = particlesParams.analyticFrame.y;
    float emitEnd = particlesParams.analyticFrame.z;
    bool bornAlways = particlesParams.analyticFrame.w > 0.5;
    bool looping = nLoop > 0u;
    if (slot < rateSlots){
        if (looping){
            // pixelfx's cycle: particle m of the loop born at m / rate, a copy per loop it outlives
            uint m = slot % nLoop;
            uint copy = slot / nLoop;
            float a = phase - float(m) / rate;
            if (a < 0.0) a += duration;
            a += float(copy) * duration;
            float absolute = loops * duration + phase;
            if (!bornAlways && absolute - a < 0.0)
                return false;
            if (absolute - a >= emitEnd)
                return false;
            index = m;
            age = a;
            return true;
        }
        float tr = phase * rate;
        if (tr < float(slot))
            return false;
        int c = int(rateSlots);
        int n = int(slot) + c * int(floor((tr - float(slot)) / float(c)));
        float end = min(duration, emitEnd);
        int total = int(ceil(rate * end));
        if (n >= total)
            n -= c * ((n - total + c) / c);
        if (n < 0)
            return false;
        index = uint(n);
        age = phase - float(n) / rate;
        return age >= 0.0;
    }
    uint s = slot - rateSlots;
    for (int bi = 0; bi < 4; bi++){
        vec4 burst = particlesParams.analyticBursts[bi];
        vec4 info = particlesParams.analyticBurstSlots[bi];
        uint countMax = uint(info.y + 0.5);
        uint copies = uint(info.w + 0.5);
        uint loopCopies = max(uint(info.z + 0.5), 1u);
        uint size = countMax * copies * loopCopies;
        if (s >= size){
            s -= size;
            continue;
        }
        uint j = s % countMax;
        uint q = (s / countMax) % copies;
        uint lc = s / (countMax * copies);
        float time = burst.x;
        float interval = burst.y;
        float probability = burst.w;
        int cyclesN = burst.z > 0.5 ? int(burst.z + 0.5) : 2147483647;
        float tEff = phase + float(lc) * duration;
        if (tEff < time)
            return false;
        if (looping && !bornAlways && float(lc) > loops)
            return false;
        int cNow = min(int(floor((tEff - time) / interval)), cyclesN - 1);
        int qn = int(copies);
        int cycle = int(q) + qn * int(floor(float(cNow - int(q)) / float(qn)));
        if (cycle < 0)
            return false;
        float fired = time + float(cycle) * interval;
        if (!looping && fired >= duration)
            return false;
        float absolute = looping ? (loops - float(lc)) * duration + fired : fired;
        if (absolute >= emitEnd)
            return false;
        uint firing = uint(bi) * 65536u + uint(cycle);
        if (probability < 1.0 && analyticRandom(uint(particlesParams.analyticKeys2.z), firing) >= probability)
            return false;
        float countMin = info.x;
        float count = countMin == float(countMax) ? countMin
                    : floor(countMin + (float(countMax) - countMin) * analyticRandom(uint(particlesParams.analyticKeys2.w), firing) + 0.5);
        if (float(j) >= count)
            return false;
        index = 0x40000000u + uint(bi) * 0x01000000u + ((uint(cycle) * countMax + j) & 0x00ffffffu);
        age = tEff - fired;
        return true;
    }
    return false;
}

int analyticRampIndex(float t){
    int entries = int(particlesParams.analyticRamp.x + 0.5);
    if (particlesParams.analyticRamp.y != 1.0 && t > 0.0)
        t = pow(t, particlesParams.analyticRamp.y);
    int stepIndex = int(min(max(floor(t * float(entries)), 0.0), float(entries - 1)));
    return particlesParams.analyticRamp.z > 0.5 ? entries - 1 - stepIndex : stepIndex;
}

// The slot's particle into the record's values (i_particle*); false when the slot is empty
bool analyticParticle(float slotValue){
    uint slot = uint(slotValue + 0.5);
    uint index;
    float age;
    if (!analyticSlotParticle(slot, index, age))
        return false;
    float lifeMin = particlesParams.analyticTiming.z;
    float lifeMax = particlesParams.analyticTiming.w;
    float life = max(lifeMin == lifeMax ? lifeMin : lifeMin + (lifeMax - lifeMin) * analyticRandom(uint(particlesParams.analyticKeys0.x), index), 1.0e-4);
    if (age >= life)
        return false;
    float normAge = age / life;
    // Flicker (pixelfx EMBER): hidden on alternate half periods near the end of life
    if (particlesParams.analyticPixel.x > 0.0 && 1.0 - normAge < particlesParams.analyticPixel.x){
        float cycle = particlesParams.analyticFrame.x * particlesParams.analyticPixel.y + analyticRandom(uint(particlesParams.analyticKeys1.w), index);
        if (sin((cycle - floor(cycle)) * ANALYTIC_TWO_PI) < 0.0)
            return false;
    }

    // start values
    vec4 start = particlesParams.analyticStart;
    vec4 start2 = particlesParams.analyticStart2;
    float speed = start.x == start.y ? start.x : start.x + (start.y - start.x) * analyticRandom(uint(particlesParams.analyticKeys0.y), index);
    float size = start.z == start.w ? start.z : start.z + (start.w - start.z) * analyticRandom(uint(particlesParams.analyticKeys0.z), index);
    size = max(size, 0.0);
    float rotation = start2.x == start2.y ? start2.x : start2.x + (start2.y - start2.x) * analyticRandom(uint(particlesParams.analyticKeys0.w), index);
    if (start2.z > 0.0 && analyticRandom(uint(particlesParams.analyticKeys1.z), index) < start2.z)
        rotation = -rotation;
    rotation += start2.w * age;

    // the flight
    vec3 p0, dir;
    analyticShape(uint(particlesParams.analyticKeys1.y), index, p0, dir);
    vec3 v0 = dir * speed + particlesParams.analyticVelocity.xyz;
    vec3 position, velocity;
    analyticFly(p0, v0, particlesParams.analyticMotion.xyz, particlesParams.analyticMotion.w, age, position, velocity);

    if (particlesParams.analyticRamp.w > 0.5)
        size *= analyticCurve(normAge, false);

    // colour (sRGB rgb, as the record holds it)
    int modes = int(particlesParams.analyticVelocity.w + 0.5);
    int startKind = modes - (modes / 4) * 4;
    int lifetimeKind = modes / 4;
    int entries = int(particlesParams.analyticRamp.x + 0.5);
    float rampStep = 255.0;
    vec4 c = particlesParams.analyticColor0;
    if (startKind == 1){
        float r = analyticRandom(uint(particlesParams.analyticKeys1.x), index);
        vec4 c0 = particlesParams.analyticColor0;
        vec4 c1 = particlesParams.analyticColor1;
        for (int k = 0; k < 3; k++)
            c[k] = analyticEncode(analyticDecode(c0[k]) + (analyticDecode(c1[k]) - analyticDecode(c0[k])) * r);
        c.a = c0.a + (c1.a - c0.a) * r;
    }else if (startKind == 2){
        int s = analyticRampIndex(analyticRandom(uint(particlesParams.analyticKeys1.x) ^ 0x68e31da4u, index));
        c = particlesParams.analyticColorTable[s];
        rampStep = float(s);
    }
    if (lifetimeKind > 0){
        float t = clamp(normAge * particlesParams.analyticRemap.x + particlesParams.analyticRemap.y, 0.0, 1.0);
        vec4 e;
        bool srgb = true;
        if (particlesParams.analyticRemap.z > 0.5){
            int s = analyticRampIndex(t);
            e = particlesParams.analyticColorTable[s];
            if (particlesParams.analyticRemap.w > 1.5) rampStep = float(s);
        }else{
            float x = t * float(entries - 1);
            int i = min(int(x), entries - 2);
            float f = x - float(i);
            e = particlesParams.analyticColorTable[i] + (particlesParams.analyticColorTable[i + 1] - particlesParams.analyticColorTable[i]) * f;
            srgb = false;
        }
        bool white = c.r == 1.0 && c.g == 1.0 && c.b == 1.0;
        for (int k = 0; k < 3; k++){
            if (white && srgb)
                c[k] = e[k];
            else
                c[k] = analyticEncode(analyticDecode(c[k]) * (srgb ? analyticDecode(e[k]) : e[k]));
        }
        c.a *= e.a;
    }
    if (particlesParams.analyticPixel.w > 0.5)
        c.a *= clamp(analyticCurve(normAge, true), 0.0, 1.0);

    // the sheet's frame
    float frame01 = 0.0;
    vec4 sheet = particlesParams.analyticSheet;
    if (sheet.x > 0.5){
        float frames = sheet.y;
        float f;
        if (sheet.x > 1.5){
            f = age * sheet.z;
        }else{
            float cyc = normAge * sheet.z;
            float t = cyc >= sheet.z ? 1.0 : cyc - floor(cyc);
            f = t * frames;
        }
        f += sheet.w;
        f -= floor(f / frames) * frames;
        frame01 = (floor(f) + 0.5) / frames;
    }

    // Wobble (R7): base pixels, added before the box snaps
    vec4 wobble = particlesParams.analyticWobble;
    if (wobble.x != 0.0 || wobble.y != 0.0){
        bool randomPhase = particlesParams.analyticPixel.z > 0.5;
        float phx = randomPhase ? analyticRandom(uint(particlesParams.analyticKeys2.x), index) * ANALYTIC_TWO_PI : 0.0;
        float phy = randomPhase ? analyticRandom(uint(particlesParams.analyticKeys2.y), index) * ANALYTIC_TWO_PI : 0.0;
        analyticWobblePx = vec2(sin(age * wobble.z * ANALYTIC_TWO_PI + phx) * wobble.x, sin(age * wobble.w * ANALYTIC_TWO_PI + phy) * wobble.y);
    }

    float turns = rotation * (1.0 / ANALYTIC_TWO_PI);
    i_particlePosition = position;
    i_particleColor = clamp(c, 0.0, 1.0);
    i_particleSize = vec2(size);
    i_particleRotFrame = vec2(turns - floor(turns), frame01);
    i_particleVelocity = vec4(velocity, particlesParams.analyticExtra.x);
    float phase = floor(analyticRandom(uint(particlesParams.analyticKeys1.w), index) * 256.0);
    i_particlePixel = vec4(rampStep / 255.0, normAge, phase / 255.0, 0.0);
    return true;
}

#endif
