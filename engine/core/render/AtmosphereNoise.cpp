// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The clouds' Worley layering (three inverted octaves per channel, persistence 0.5, the
// channel cell counts rising) and the per-channel min/max normalisation follow OpenEngine's
// cloud noise bake (Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_worley.glsl and
// volumetric_clouds_*_noise.comp, MIT, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors), itself derived from SebLague/Clouds (MIT, Copyright (c) 2019 Sebastian Lague).
// Ported to the CPU with table-driven feature points and periodic Perlin noise.

#include "AtmosphereNoise.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;

namespace {

    uint32_t hash32(uint32_t x){
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    uint32_t hashLattice(uint32_t x, uint32_t y, uint32_t z, uint32_t seed){
        return hash32(x * 0x8da6b343u ^ hash32(y * 0xd8163841u ^ hash32(z * 0xcb1ab31fu ^ seed)));
    }

    float unitFloat(uint32_t h){
        return (float)(h >> 8) * (1.0f / 16777216.0f);
    }

    int wrapCell(int cell, int cells){
        // the bake's lattice indices are at most a period outside [0, cells): no division
        if (cell < 0)
            cell += cells;
        else if (cell >= cells)
            cell -= cells;
        if (cell >= 0 && cell < cells)
            return cell;
        const int m = cell % cells;
        return m < 0 ? m + cells : m;
    }

    // the feature points of a Worley octave: one per cell of a cells^dims grid, in [0, 1)^3
    // inside its cell, from the hash of the cell and the seed
    struct WorleyTable{
        int cells = 1;
        int dims = 3;
        std::vector<float> points;

        WorleyTable(int cells, int dims, uint32_t seed): cells(cells), dims(dims){
            const int count = (dims == 3) ? cells * cells * cells : cells * cells;
            points.resize((size_t)count * 3);
            for (int i = 0; i < count; i++){
                const int x = i % cells;
                const int y = (i / cells) % cells;
                const int z = (dims == 3) ? i / (cells * cells) : 0;
                const uint32_t h = hashLattice((uint32_t)x, (uint32_t)y, (uint32_t)z, seed);
                points[(size_t)i * 3 + 0] = unitFloat(h);
                points[(size_t)i * 3 + 1] = unitFloat(hash32(h ^ 0x68e31da4u));
                points[(size_t)i * 3 + 2] = unitFloat(hash32(h ^ 0xb5297a4du));
            }
        }

        // distance to the nearest feature point in cell units (about 0..1), the grid wrapped
        float distance(float x, float y, float z) const{
            const float sx = x * (float)cells;
            const float sy = y * (float)cells;
            const float sz = (dims == 3) ? z * (float)cells : 0.0f;
            const int cx = (int)std::floor(sx);
            const int cy = (int)std::floor(sy);
            const int cz = (int)std::floor(sz);
            const float fx = sx - (float)cx;
            const float fy = sy - (float)cy;
            const float fz = sz - (float)cz;
            // the wrapped cell indices of the 3x3(x3) neighbourhood, once per axis
            int wx[3], wy[3], wz[3] = {0, 0, 0};
            for (int d = 0; d < 3; d++){
                wx[d] = wrapCell(cx + d - 1, cells);
                wy[d] = wrapCell(cy + d - 1, cells);
                if (dims == 3)
                    wz[d] = wrapCell(cz + d - 1, cells);
            }
            // the sample's own cell first: its point bounds the search, so most neighbours are
            // skipped by their box distance
            float best;
            {
                const float* point = &points[(((size_t)wz[1] * cells + (size_t)wy[1]) * cells + (size_t)wx[1]) * 3];
                const float ox = point[0] - fx;
                const float oy = point[1] - fy;
                const float oz = (dims == 3) ? point[2] - fz : 0.0f;
                best = ox * ox + oy * oy + oz * oz;
            }
            const int zRange = (dims == 3) ? 1 : 0;
            for (int dz = -zRange; dz <= zRange; dz++){
                // the squared distance from the sample to the neighbour cell's box: a cell whose
                // box is farther than the best point so far cannot hold a nearer one
                const float bz = (dz < 0) ? fz : ((dz > 0) ? 1.0f - fz : 0.0f);
                const size_t zBase = (size_t)wz[dz + 1] * cells;
                for (int dy = -1; dy <= 1; dy++){
                    const float by = (dy < 0) ? fy : ((dy > 0) ? 1.0f - fy : 0.0f);
                    const float byz = by * by + bz * bz;
                    if (byz >= best)
                        continue;
                    const size_t yBase = (zBase + (size_t)wy[dy + 1]) * cells;
                    for (int dx = -1; dx <= 1; dx++){
                        if (dx == 0 && dy == 0 && dz == 0)
                            continue;
                        const float bx = (dx < 0) ? fx : ((dx > 0) ? 1.0f - fx : 0.0f);
                        if (bx * bx + byz >= best)
                            continue;
                        const float* point = &points[(yBase + (size_t)wx[dx + 1]) * 3];
                        const float ox = (float)dx + point[0] - fx;
                        const float oy = (float)dy + point[1] - fy;
                        const float oz = (dims == 3) ? (float)dz + point[2] - fz : 0.0f;
                        best = std::min(best, ox * ox + oy * oy + oz * oz);
                    }
                }
            }
            return std::sqrt(best);
        }
    };

    // three inverted octaves folded with persistence 0.5, 0..1
    struct LayeredWorley{
        WorleyTable a;
        WorleyTable b;
        WorleyTable c;

        LayeredWorley(int cellsA, int cellsB, int cellsC, int dims, uint32_t seed):
            a(cellsA, dims, seed), b(cellsB, dims, seed + 17u), c(cellsC, dims, seed + 43u) {}

        float sample(float x, float y, float z) const{
            const float la = 1.0f - std::min(a.distance(x, y, z), 1.0f);
            const float lb = 1.0f - std::min(b.distance(x, y, z), 1.0f);
            const float lc = 1.0f - std::min(c.distance(x, y, z), 1.0f);
            return (la + 0.5f * lb + 0.25f * lc) / 1.75f;
        }
    };

    float fade(float t){
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    float lerp(float a, float b, float t){
        return a + (b - a) * t;
    }

    // improved Perlin's gradient set
    float gradient3(uint32_t h, float x, float y, float z){
        switch (h & 15u){
            case 0: return x + y;
            case 1: return -x + y;
            case 2: return x - y;
            case 3: return -x - y;
            case 4: return x + z;
            case 5: return -x + z;
            case 6: return x - z;
            case 7: return -x - z;
            case 8: return y + z;
            case 9: return -y + z;
            case 10: return y - z;
            case 11: return -y - z;
            case 12: return x + y;
            case 13: return -y + z;
            case 14: return -x + y;
            default: return -y - z;
        }
    }

    // Perlin noise of period `period` lattice cells over [0, 1) on each axis, about -1..1
    float periodicPerlin(float x, float y, float z, int period, uint32_t seed, bool threeD){
        const float sx = x * (float)period;
        const float sy = y * (float)period;
        const float sz = threeD ? z * (float)period : 0.0f;
        const int ix = (int)std::floor(sx);
        const int iy = (int)std::floor(sy);
        const int iz = (int)std::floor(sz);
        const float fx = sx - (float)ix;
        const float fy = sy - (float)iy;
        const float fz = sz - (float)iz;
        auto corner = [&](int dx, int dy, int dz){
            const uint32_t h = hashLattice((uint32_t)wrapCell(ix + dx, period), (uint32_t)wrapCell(iy + dy, period),
                                           threeD ? (uint32_t)wrapCell(iz + dz, period) : 0u, seed);
            return gradient3(h, fx - (float)dx, fy - (float)dy, fz - (float)dz);
        };
        const float u = fade(fx);
        const float v = fade(fy);
        const float x00 = lerp(corner(0, 0, 0), corner(1, 0, 0), u);
        const float x10 = lerp(corner(0, 1, 0), corner(1, 1, 0), u);
        const float y0 = lerp(x00, x10, v);
        if (!threeD)
            return y0;
        const float x01 = lerp(corner(0, 0, 1), corner(1, 0, 1), u);
        const float x11 = lerp(corner(0, 1, 1), corner(1, 1, 1), u);
        const float y1 = lerp(x01, x11, v);
        return lerp(y0, y1, fade(fz));
    }

    // octaves of periodic Perlin noise (period doubling, amplitude halving), 0..1
    float perlinFbm(float x, float y, float z, int basePeriod, int octaves, uint32_t seed, bool threeD){
        float sum = 0.0f;
        float amplitude = 1.0f;
        float total = 0.0f;
        int period = basePeriod;
        for (int o = 0; o < octaves; o++){
            sum += amplitude * periodicPerlin(x, y, z, period, seed + (uint32_t)o * 101u, threeD);
            total += amplitude;
            amplitude *= 0.5f;
            period *= 2;
        }
        return std::clamp(0.5f + 0.5f * sum / total, 0.0f, 1.0f);
    }

    // every Worley octave the bake reads, built once (thread-safe static initialisation)
    struct NoiseTables{
        LayeredWorley shapeR{4, 8, 16, 3, 1u};
        LayeredWorley shapeG{4, 8, 16, 3, 11u};
        LayeredWorley shapeB{8, 16, 24, 3, 21u};
        LayeredWorley shapeA{12, 24, 32, 3, 31u};
        LayeredWorley detailR{2, 4, 8, 3, 41u};
        LayeredWorley detailG{4, 8, 12, 3, 51u};
        LayeredWorley detailB{6, 12, 16, 3, 61u};
        LayeredWorley detailA{8, 16, 20, 3, 71u};
        LayeredWorley weatherCells{6, 12, 24, 2, 81u};
        LayeredWorley weatherDetail{16, 32, 64, 2, 91u};
    };

    const NoiseTables& tables(){
        static const NoiseTables instance;
        return instance;
    }

    // rows of a slice (3D) or of the map (2D) per work item: each item is about a millisecond of
    // work, the granularity of the time-sliced bake
    constexpr int SHAPE_ROWS_PER_ITEM = 4;
    constexpr int DETAIL_ROWS_PER_ITEM = 8;
    constexpr int SHAPE_ITEMS = AtmosphereNoise::SHAPE_SIZE * (AtmosphereNoise::SHAPE_SIZE / SHAPE_ROWS_PER_ITEM);
    constexpr int DETAIL_ITEMS = AtmosphereNoise::DETAIL_SIZE * (AtmosphereNoise::DETAIL_SIZE / DETAIL_ROWS_PER_ITEM);
    constexpr int WEATHER_ITEMS = AtmosphereNoise::WEATHER_SIZE / AtmosphereNoise::WEATHER_BAND_ROWS;
    constexpr int ITEMS = SHAPE_ITEMS + DETAIL_ITEMS + WEATHER_ITEMS;

    enum Volume { VOLUME_SHAPE = 0, VOLUME_DETAIL = 1, VOLUME_WEATHER = 2 };

    // the texels an item covers, as offsets of RGBA quadruples in its volume
    struct ItemRange{
        Volume volume;
        size_t first;
        size_t count;
    };

    ItemRange itemRange(int item){
        const int shapeRow = AtmosphereNoise::SHAPE_SIZE;
        if (item < SHAPE_ITEMS){
            return {VOLUME_SHAPE, (size_t)item * SHAPE_ROWS_PER_ITEM * shapeRow, (size_t)SHAPE_ROWS_PER_ITEM * shapeRow};
        }
        item -= SHAPE_ITEMS;
        if (item < DETAIL_ITEMS){
            const size_t rows = (size_t)DETAIL_ROWS_PER_ITEM * AtmosphereNoise::DETAIL_SIZE;
            return {VOLUME_DETAIL, (size_t)item * rows, rows};
        }
        item -= DETAIL_ITEMS;
        const size_t band = (size_t)AtmosphereNoise::WEATHER_BAND_ROWS * AtmosphereNoise::WEATHER_SIZE;
        return {VOLUME_WEATHER, (size_t)item * band, band};
    }

    int volumeSize(Volume volume){
        return volume == VOLUME_SHAPE ? AtmosphereNoise::SHAPE_SIZE
             : (volume == VOLUME_DETAIL ? AtmosphereNoise::DETAIL_SIZE : AtmosphereNoise::WEATHER_SIZE);
    }

    // channels the normalisation stretches (the weather map's alpha stays 255)
    int normalisedChannels(Volume volume){
        return volume == VOLUME_WEATHER ? 3 : 4;
    }

    double nowMilliseconds(){
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

}

struct AtmosphereNoise::State{
    std::vector<float> values[3];
    std::vector<uint8_t> bytes[3];
    // per item: min and max of each channel
    float itemMin[ITEMS][4];
    float itemMax[ITEMS][4];
    float channelMin[3][4];
    float channelScale[3][4];
    // 0 not started, 1 sampling, 2 normalising, 3 ready
    std::atomic<int> phase{0};

    State(){
        for (int v = 0; v < 3; v++){
            const size_t size = (size_t)volumeSize((Volume)v);
            const size_t texels = (v == VOLUME_WEATHER) ? size * size : size * size * size;
            values[v].resize(texels * 4);
            bytes[v].resize(texels * 4);
        }
    }

    void sampleItem(int item){
        const ItemRange range = itemRange(item);
        const int size = volumeSize(range.volume);
        float* out = values[range.volume].data();
        float lo[4] = {1e10f, 1e10f, 1e10f, 1e10f};
        float hi[4] = {-1e10f, -1e10f, -1e10f, -1e10f};
        for (size_t i = range.first; i < range.first + range.count; i++){
            const int x = (int)(i % (size_t)size);
            const int y = (int)((i / (size_t)size) % (size_t)size);
            const int z = (int)(i / ((size_t)size * (size_t)size));
            Vector4 value;
            if (range.volume == VOLUME_SHAPE){
                value = sampleShape((float)x / (float)size, (float)y / (float)size, (float)z / (float)size);
            }else if (range.volume == VOLUME_DETAIL){
                value = sampleDetail((float)x / (float)size, (float)y / (float)size, (float)z / (float)size);
            }else{
                value = sampleWeather((float)x / (float)size, (float)y / (float)size);
            }
            for (int c = 0; c < 4; c++){
                out[i * 4 + c] = value[c];
                lo[c] = std::min(lo[c], value[c]);
                hi[c] = std::max(hi[c], value[c]);
            }
        }
        for (int c = 0; c < 4; c++){
            itemMin[item][c] = lo[c];
            itemMax[item][c] = hi[c];
        }
    }

    // the channels' ranges over every item of a volume
    void reduceRanges(){
        for (int v = 0; v < 3; v++){
            for (int c = 0; c < 4; c++){
                float lo = 1e10f;
                float hi = -1e10f;
                for (int item = 0; item < ITEMS; item++){
                    if (itemRange(item).volume != (Volume)v)
                        continue;
                    lo = std::min(lo, itemMin[item][c]);
                    hi = std::max(hi, itemMax[item][c]);
                }
                channelMin[v][c] = lo;
                channelScale[v][c] = (hi - lo > 1e-6f) ? 1.0f / (hi - lo) : 0.0f;
            }
        }
    }

    void normaliseItem(int item){
        const ItemRange range = itemRange(item);
        const float* in = values[range.volume].data();
        uint8_t* out = bytes[range.volume].data();
        const int channels = normalisedChannels(range.volume);
        for (size_t i = range.first; i < range.first + range.count; i++){
            for (int c = 0; c < 4; c++){
                float unit = 1.0f;
                if (c < channels)
                    unit = std::clamp((in[i * 4 + c] - channelMin[range.volume][c]) * channelScale[range.volume][c], 0.0f, 1.0f);
                out[i * 4 + c] = (uint8_t)std::lround(unit * 255.0f);
            }
        }
    }

    void releaseValues(){
        for (int v = 0; v < 3; v++){
            std::vector<float>().swap(values[v]);
        }
    }
};

AtmosphereNoise::AtmosphereNoise(): state(std::make_shared<State>()){
}

AtmosphereNoise::~AtmosphereNoise(){
    // the jobs hold the state by shared pointer; waiting keeps them from outliving the bake
    for (jobs::Task<void>& task : tasks)
        task.wait();
}

AtmosphereNoise& AtmosphereNoise::get(){
    static AtmosphereNoise instance;
    return instance;
}

Vector4 AtmosphereNoise::sampleShape(float x, float y, float z){
    const NoiseTables& t = tables();
    const float worley = t.shapeR.sample(x, y, z);
    const float perlin = perlinFbm(x, y, z, 4, 4, 7u, true);
    // Perlin-Worley: the Perlin noise remapped into [worley, 1] (billowy, connected shapes)
    const float perlinWorley = worley + perlin * (1.0f - worley);
    return Vector4(perlinWorley, t.shapeG.sample(x, y, z), t.shapeB.sample(x, y, z), t.shapeA.sample(x, y, z));
}

Vector4 AtmosphereNoise::sampleDetail(float x, float y, float z){
    const NoiseTables& t = tables();
    return Vector4(t.detailR.sample(x, y, z), t.detailG.sample(x, y, z), t.detailB.sample(x, y, z), t.detailA.sample(x, y, z));
}

Vector4 AtmosphereNoise::sampleWeather(float x, float y){
    const NoiseTables& t = tables();
    // coverage: large Perlin patches broken into cells
    const float patches = perlinFbm(x, y, 0.0f, 3, 4, 101u, false);
    const float cells = t.weatherCells.sample(x, y, 0.0f);
    const float coverage = patches * 0.65f + cells * 0.35f;
    // cloud type: a slow variation from stratus to towering
    const float type = perlinFbm(x, y, 0.0f, 2, 3, 211u, false);
    return Vector4(coverage, type, t.weatherDetail.sample(x, y, 0.0f), 1.0f);
}

void AtmosphereNoise::start(bool threaded){
    int expected = 0;
    if (!state->phase.compare_exchange_strong(expected, 1))
        return;
    this->threaded = threaded;
    nextItem = 0;
    if (threaded){
        std::shared_ptr<State> shared = state;
        tasks.reserve(ITEMS);
        for (int item = 0; item < ITEMS; item++){
            tasks.push_back(jobs::submit([shared, item](){ shared->sampleItem(item); }, {jobs::Lane::Background, jobs::GateClass::Default, "atmosphere-noise"}));
        }
    }
}

bool AtmosphereNoise::tasksDone(){
    for (const jobs::Task<void>& task : tasks){
        if (!task.isReady())
            return false;
    }
    tasks.clear();
    return true;
}

void AtmosphereNoise::beginNormalise(){
    state->reduceRanges();
    state->phase = 2;
    nextItem = 0;
    if (threaded){
        std::shared_ptr<State> shared = state;
        tasks.reserve(ITEMS);
        for (int item = 0; item < ITEMS; item++){
            tasks.push_back(jobs::submit([shared, item](){ shared->normaliseItem(item); }, {jobs::Lane::Background, jobs::GateClass::Default, "atmosphere-noise"}));
        }
    }
}

bool AtmosphereNoise::advance(double budgetMilliseconds){
    const int phase = state->phase;
    if (phase == 0)
        return false;
    if (phase == 3)
        return true;

    if (threaded){
        if (!tasksDone())
            return false;
        if (phase == 1){
            beginNormalise();
            return false;
        }
        state->releaseValues();
        state->phase = 3;
        return true;
    }

    const double start = nowMilliseconds();
    while (nowMilliseconds() - start < budgetMilliseconds){
        if (state->phase == 1){
            state->sampleItem(nextItem++);
            if (nextItem == ITEMS)
                beginNormalise();
        }else{
            state->normaliseItem(nextItem++);
            if (nextItem == ITEMS){
                state->releaseValues();
                state->phase = 3;
                return true;
            }
        }
    }
    return false;
}

void AtmosphereNoise::bakeNow(){
    start(false);
    while (!advance(1e9)){
    }
}

bool AtmosphereNoise::isStarted() const{
    return state->phase != 0;
}

bool AtmosphereNoise::isReady() const{
    return state->phase == 3;
}

const std::vector<uint8_t>& AtmosphereNoise::getShape() const{
    return state->bytes[VOLUME_SHAPE];
}

const std::vector<uint8_t>& AtmosphereNoise::getDetail() const{
    return state->bytes[VOLUME_DETAIL];
}

const std::vector<uint8_t>& AtmosphereNoise::getWeather() const{
    return state->bytes[VOLUME_WEATHER];
}
