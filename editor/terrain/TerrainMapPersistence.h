// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "texture/Texture.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // When an edited map reaches its file (T1 R29-R32). An edit changes memory only: the map's entry
    // in TextureDataPool is authoritative, and no command encodes or copies a whole map. A dirty map's
    // path waits with the write scope root it was edited under; a second after the last edit, with no
    // stroke in progress, its pixels are copied once and handed to TerrainMapFileWriter, which stays
    // the only path to disk. Forced flushes: scene save and autosave, play start, export, closing the
    // project, and every reader that goes to disk. A crash loses at most the debounce and the encode.
    // Main thread.
    class TerrainMapPersistence{
    public:
        static constexpr double DEBOUNCE_SECONDS = 1.0;

        static TerrainMapPersistence& get();

        // A steady clock's seconds: what the forms without a time below read
        static double clockSeconds();

        // A map without a file has nothing to write
        void markDirty(Project* project, Texture& texture);
        void markDirty(Project* project, Texture& texture, double nowSeconds);

        void beginStroke();
        void endStroke();

        // Once a frame: hands over what has been quiet for the debounce
        void update();
        void update(double nowSeconds);
        // Whether update would hand something over now: an idle editor wakes a frame for it
        bool isDue() const;
        bool isDue(double nowSeconds) const;

        // Hands this map over and waits for its write (absolute path)
        void flushPath(const std::string& absolutePath);
        // Hands every dirty map over, without waiting for the writes
        void handOverAll();
        // Hands every dirty map over and waits. False when a write failed: it stays with the writer
        // and is retried at the next flush, so the caller keeps the scene modified (R31).
        bool flushAll();

        // How many maps wait for their debounce
        size_t pendingCount() const;
        // The files whose last write failed, for the map_write_failed issue (R31)
        std::vector<std::string> failedPaths() const;

        TerrainMapPersistence(const TerrainMapPersistence&) = delete;
        TerrainMapPersistence& operator=(const TerrainMapPersistence&) = delete;

    private:
        struct Dirty{
            std::filesystem::path root;
            // kept alive: the pixels of a map whose last Texture went away must still reach its file
            std::shared_ptr<std::array<TextureData, 6>> entry;
            double lastEdit = 0.0;
        };

        TerrainMapPersistence() = default;

        void handOver(const std::string& absolutePath, const Dirty& dirty);

        std::map<std::string, Dirty> dirtyMaps;
        int strokes = 0;
    };

}
