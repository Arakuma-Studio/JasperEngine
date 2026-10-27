// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What the S20 migration converted (docs/specs/S20-particles.md R32): each old Particle System a
// document read since the project opened, with the line its migration logged (the target it
// moved to, the approximations it made). get_particle_migration_report lists them.

#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class ParticleMigrationReport {
    public:
        struct Entry {
            std::string document;   // the file read (scene or prefab)
            std::string line;       // "Particle System of Fire: converted to an inline effect; ..."
        };

        // the lines of one document's migration that are particle conversions
        static void record(const std::string& document, const std::vector<std::string>& lines);
        static std::vector<Entry> entries();
        static void clear();

    private:
        static std::mutex& mutex();
        static std::vector<Entry>& list();
    };

}
