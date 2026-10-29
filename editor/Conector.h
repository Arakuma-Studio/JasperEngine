// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include "Scene.h"

namespace fs = std::filesystem;

namespace jasperengine::editor{

    class Conector{
    private:
        void* libHandle;
        fs::path loadedLibCopy;

        void* loadSharedLibrary(const std::string& libPath);
        void unloadSharedLibrary(void* libHandle);

    public:
        Conector();
        virtual ~Conector();

        bool connect(const fs::path& buildPath, std::string libName, const fs::path& projectRoot = {});
        void disconnect();

        void init(Scene* sceneProject);
        void cleanup(Scene* sceneProject);

        bool isLibraryConnected() const;
    };

}
