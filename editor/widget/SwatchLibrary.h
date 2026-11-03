// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Color window's swatch libraries (docs/specs/S27-editor-tools.md R5, §6.5): JSON files
// `*.colors`, per user in <settings dir>/swatches/ and per project in
// <project>/EditorSettings/Swatches/ (in version control). A swatch keeps its linear value and
// whether it is HDR; a library also keeps gradient presets for the Gradient Editor. A file of an
// unknown version is read-only. The palette model is OpenEngine's (Apps/Editor/Source/Panels/
// ColorPicker.cpp: rows of 19, at most 4 rows, a spectrum row by default).

#include "math/Curve.h"
#include "math/Vector4.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct Swatch {
        std::string name;
        Vector4 linear = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        bool hdr = false;

        bool operator==(const Swatch& other) const { return name == other.name && linear == other.linear && hdr == other.hdr; }
    };

    struct GradientPreset {
        std::string name;
        Gradient gradient;
    };

    enum class LibraryScope : uint8_t { User, Project };

    struct SwatchLibrary {
        static constexpr int kVersion = 1;
        static constexpr size_t kRowLength = 19;
        static constexpr size_t kMaxSwatches = 4 * kRowLength;

        std::string name;
        LibraryScope scope = LibraryScope::User;
        std::filesystem::path path;      // the .colors file
        bool readOnly = false;           // an unknown version: shown, never written
        std::string warning;             // why it is read-only
        std::vector<Swatch> swatches;
        std::vector<GradientPreset> gradients;
    };

    namespace swatches {

        constexpr const char* kExtension = ".colors";

        // <settings dir>/swatches and <project>/EditorSettings/Swatches
        std::filesystem::path userDirectory(const std::filesystem::path& settingsDirectory);
        std::filesystem::path projectDirectory(const std::filesystem::path& projectPath);

        // the library "Default": black, white, grey and a 16-hue spectrum; the gradients White,
        // Fade Out, Fade In, Black to White, White to Black and Rainbow
        SwatchLibrary defaultLibrary();

        std::string toJson(const SwatchLibrary& library);
        // false with `error` on a malformed file; an unknown version reads as read-only
        bool fromJson(const std::string& text, SwatchLibrary& library, std::string& error);

        bool read(const std::filesystem::path& file, LibraryScope scope, SwatchLibrary& library, std::string& error);
        // atomic (S03); refuses a read-only library
        bool write(const SwatchLibrary& library, std::string& error);

        // Every library of both directories, user first, each sorted by name. When the user
        // directory has no library, "Default" is written there first. Unreadable files are
        // skipped with a line in `errors`.
        std::vector<SwatchLibrary> loadAll(const std::filesystem::path& userDir, const std::filesystem::path& projectDir,
                                           std::vector<std::string>* errors = nullptr);

        // the file of a library named `name` in `directory` (the name with the characters a
        // file name cannot hold replaced by '_')
        std::filesystem::path libraryPath(const std::filesystem::path& directory, const std::string& name);

        // ---- edits (false with `error` when not allowed: read-only, full, a bad index)
        bool add(SwatchLibrary& library, const Swatch& swatch, std::string& error);
        bool remove(SwatchLibrary& library, size_t index, std::string& error);
        bool replace(SwatchLibrary& library, size_t index, const Vector4& linear, bool hdr, std::string& error);
        bool rename(SwatchLibrary& library, size_t index, const std::string& name, std::string& error);
        // moves swatch `index` one place left (-1) or right (+1)
        bool move(SwatchLibrary& library, size_t index, int direction, std::string& error);

        // creates an empty library file (false when one with that name exists); renames a
        // library's file; deletes it
        bool createLibrary(const std::filesystem::path& directory, LibraryScope scope, const std::string& name,
                           SwatchLibrary& library, std::string& error);
        bool renameLibrary(SwatchLibrary& library, const std::string& name, std::string& error);
        bool deleteLibrary(const SwatchLibrary& library, std::string& error);

    }

}
