// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Curve presets (docs/specs/S27-editor-tools.md R11, §6.5): the built-in shapes and the user and
// project `.curves` libraries (<settings dir>/curves/, <project>/EditorSettings/Curves/). Presets
// are stored normalised to [0,1]x[0,1] and mapped onto the field's range when applied
// (curveops::remap), as OpenEngine's CurvePresets do (Apps/Editor/Source/UI/Controls/
// CurvePresets.cpp, MIT, NOTICE.md), with its "My Preset N" naming.

#include "math/Curve.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct CurvePreset {
        std::string name;
        Curve curve;   // over [0,1]x[0,1]
    };

    enum class CurveLibraryScope : uint8_t { BuiltIn, User, Project };

    struct CurvePresetLibrary {
        static constexpr int kVersion = 1;

        std::string name;
        CurveLibraryScope scope = CurveLibraryScope::User;
        std::filesystem::path path;   // empty for the built-in one
        bool readOnly = false;        // built in, or a file of an unknown version
        std::string warning;
        std::vector<CurvePreset> presets;
    };

    namespace curvepresets {

        constexpr const char* kExtension = ".curves";

        // Constant, Linear, Linear Down, Ease In, Ease Out, Ease In Out, Ease In Out Down, Bell
        const CurvePresetLibrary& builtIn();

        std::filesystem::path userDirectory(const std::filesystem::path& settingsDirectory);
        std::filesystem::path projectDirectory(const std::filesystem::path& projectPath);

        std::string toJson(const CurvePresetLibrary& library);
        bool fromJson(const std::string& text, CurvePresetLibrary& library, std::string& error);
        bool read(const std::filesystem::path& file, CurveLibraryScope scope, CurvePresetLibrary& library, std::string& error);
        bool write(const CurvePresetLibrary& library, std::string& error);

        // built-in first, then the user libraries, then the project's (each sorted by name); when
        // the user directory has none, an empty "My Presets" is made there
        std::vector<CurvePresetLibrary> loadAll(const std::filesystem::path& userDir, const std::filesystem::path& projectDir,
                                                std::vector<std::string>* errors = nullptr);

        // "My Preset N" with the first N no preset of the library has
        std::string nextPresetName(const CurvePresetLibrary& library);
        // a preset added to a writable library and the file written
        bool addPreset(CurvePresetLibrary& library, const std::string& name, const Curve& normalised, std::string& error);
        bool removePreset(CurvePresetLibrary& library, size_t index, std::string& error);

    }

}
