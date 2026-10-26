// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "Scene.h"
#include "script/ToolchainFingerprint.h"

namespace fs = std::filesystem;

namespace jasperengine::editor{

    // The C++ script library of Play: a uniquely named copy of the build output, checked
    // against the editor's toolchain fingerprint before it is loaded (its marker, read from
    // the file) and after (its export). A reload during Play loads the next generation next to
    // the ones before: superseded images are never unmapped while Play runs (callbacks and
    // statics may still point into them); disconnect() at Stop unloads every generation.
    class Conector{
    public:
        // At most this many generations per Play (the first and 15 reloads)
        static constexpr size_t MAX_GENERATIONS = 16;

        struct LoadCheck {
            bool ok = false;
            bool refused = false;                      // the fingerprint said no
            std::string message;                       // why not, or the soft warning
            std::vector<FingerprintMismatch> mismatches;
        };

    private:
        struct Library {
            void* handle = nullptr;
            fs::path copy;
        };
        std::vector<Library> libraries;                // in load order
        size_t current = 0;                            // the one in use
        LoadCheck lastCheck;

        void* loadSharedLibrary(const std::string& libPath);
        void unloadSharedLibrary(void* libHandle);
        void* findSymbol(void* handle, const char* name) const;
        // stages, checks and loads one library; false when it is refused or fails
        bool loadGeneration(const fs::path& buildPath, const std::string& libName, Library& out);

    public:
        Conector();
        virtual ~Conector();

        // Play: the first generation (unloads what was loaded)
        bool connect(const fs::path& buildPath, std::string libName);
        // A C++ reload: the rebuilt library next to the current one, which stays current until
        // makeLatestCurrent(). False (lastLoadCheck says why) when refused or not loadable.
        bool connectNext(const fs::path& buildPath, const std::string& libName);
        // The library loaded last becomes the one ObjectManager's hooks, init() and cleanup() use
        void makeLatestCurrent();
        void disconnect();

        void init(Scene* sceneProject);
        void cleanup(Scene* sceneProject);

        bool isLibraryConnected() const;
        size_t generationCount() const { return libraries.size(); }
        size_t currentGeneration() const { return current; }
        // A symbol of a generation (0 = the first, generationCount() - 1 = the newest)
        void* symbol(size_t generation, const char* name) const;
        const LoadCheck& lastLoadCheck() const { return lastCheck; }
    };

}
