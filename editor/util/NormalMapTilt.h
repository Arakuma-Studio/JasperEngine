// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace jasperengine::editor {

    // How far a tangent-space normal map's average normal leans from +Z. Stochastic Tiling with
    // Rotation turns each hex cell's copy of the map by a random angle, and a map whose texels
    // lean one way on average (baked with a slope, or with light from one side) then leans a
    // different way in each cell: the surface looks tilted cell by cell. The Inspector warns
    // under a Rotation row (materials and terrain layers) and the `check` command reports it
    // (stochastic-tilted-normal-map) when the tilt is at least WARNING_DEGREES.
    //
    // The mean of the decoded texel normals (rgb * 2 - 1, each made unit length; large images
    // are sampled on a grid of at most 512 x 512), measured once per file and kept by its path,
    // modification time and size.
    //
    // Thread-safe. Nothing decodes while holding the cache's lock, and a file that changes while
    // its old version is still being measured never waits for that measure: each measure has
    // its own shared state, which the thread computing it keeps alive (and the cache drops), so
    // the UI thread never blocks on a decode, and a measure still running at exit touches no
    // static the exit destroys.
    struct NormalMapTiltResult {
        bool ready = false;     // measured (request: false while it is being measured)
        bool valid = false;     // the file could be read as an image
        float degrees = 0.0f;   // angle between the mean normal and +Z
    };

    class NormalMapTilt {
    public:
        // noticeable on rotated cells
        static constexpr float WARNING_DEGREES = 3.0f;

        // Without waiting: the cached result, else ready = false while a background thread
        // measures it (call again on a later frame)
        static NormalMapTiltResult request(const std::filesystem::path& file);
        // Waiting for the measure (cached too): measured on the calling thread, or the result of
        // the measure a request already started for this version of the file
        static NormalMapTiltResult measure(const std::filesystem::path& file);

        // the measure itself, uncached: false when the file is not an image
        static bool measureFile(const std::filesystem::path& file, float& degrees, std::string* error = nullptr);

        // e.g. "rock_normal.png's average normal is 9° off +Z"
        static std::string describe(const std::filesystem::path& file, float degrees);

        // Tests: the measure request and measure run (empty: measureFile), and forgetting every
        // cached result (measures still running finish into nothing)
        using MeasureFunction = std::function<bool(const std::filesystem::path& file, float& degrees)>;
        static void setMeasureFunction(MeasureFunction function);
        static void clearCache();
    };

}
