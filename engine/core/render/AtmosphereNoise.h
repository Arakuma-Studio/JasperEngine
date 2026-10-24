// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ATMOSPHERE_NOISE_H
#define ATMOSPHERE_NOISE_H

#include "Export.h"
#include "math/Vector4.h"
#include "jobs/Task.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine{

    // The volumetric clouds' noise, baked on the CPU once per process and shared by every
    // scene (no compute shaders):
    //   shape   64^3 RGBA8: R Perlin-Worley, G B A Worley FBM at rising frequencies
    //   detail  32^3 RGBA8: Worley FBM at four rising frequencies (the erosion)
    //   weather 256^2 RGBA8: R coverage, G cloud type, B a finer coverage detail, A 255 (the
    //           built-in weather map, used when a component names none)
    // Every texture tiles (its noise is periodic over the texture) and each channel is
    // stretched to the full 0..255 range. The bake is deterministic: the same bytes on every
    // run of a build. Worley cells and gradients come from an integer hash of the wrapped
    // lattice coordinate (the layering of OpenEngine's volumetric_clouds_worley.glsl, MIT).
    //
    // The work is split into items (a 3D slice or a band of weather rows) that run as
    // Background jobs (S06), or, without workers (web builds without pthreads), on the main
    // thread a time budget at a time. advance() never blocks on the workers.
    class JASPERENGINE_API AtmosphereNoise{
    public:
        static constexpr int SHAPE_SIZE = 64;
        static constexpr int DETAIL_SIZE = 32;
        static constexpr int WEATHER_SIZE = 256;
        static constexpr int WEATHER_BAND_ROWS = 4;

        AtmosphereNoise();
        ~AtmosphereNoise();
        AtmosphereNoise(const AtmosphereNoise&) = delete;
        AtmosphereNoise& operator=(const AtmosphereNoise&) = delete;

        // the process's bake, shared by every scene's renderer
        static AtmosphereNoise& get();

        // begins the bake (once; later calls do nothing): threaded as jobs, else the items
        // wait for advance()
        void start(bool threaded);
        // threaded: checks the jobs and starts the next phase; unthreaded: runs items on this
        // thread until budgetMilliseconds is spent. True once the bytes are ready.
        bool advance(double budgetMilliseconds);
        // the whole bake on the calling thread (tests, tools)
        void bakeNow();

        bool isStarted() const;
        bool isReady() const;
        // wall time from start() to the bytes being ready (ms), -1 before; how it ran
        double getBakeMilliseconds() const { return bakeMilliseconds; }
        bool isThreaded() const { return threaded; }

        // RGBA8, x fastest, then y, then z (rows top first for the weather map)
        const std::vector<uint8_t>& getShape() const;
        const std::vector<uint8_t>& getDetail() const;
        const std::vector<uint8_t>& getWeather() const;

        // the unnormalised noise functions the bake samples at texel (x, y, z) / size: a
        // period of 1 on every axis
        static Vector4 sampleShape(float x, float y, float z);
        static Vector4 sampleDetail(float x, float y, float z);
        static Vector4 sampleWeather(float x, float y);

    private:
        struct State;
        std::shared_ptr<State> state;
        bool threaded = false;
        std::vector<jobs::Task<void>> tasks;
        int nextItem = 0;   // unthreaded: the next item to run
        double startedAt = 0.0;
        double bakeMilliseconds = -1.0;

        void beginNormalise();
        bool tasksDone();
    };

}

#endif // ATMOSPHERE_NOISE_H
