// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AudioAnalysis.h"
#include "SoundManifest.h"

#include "audio/AudioImport.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // The one import planner of the built-in clips: the 40 essentials compiled into the editor
    // (`essentials:<id>`) and the starter library installed next to it (`library:<id>`). It plans
    // what an import writes: <directory>/<id>.ogg with its bytes (SHA-256 checked) and its .meta
    // (the manifest's sections and a minted GUID). Each caller then writes the plan as one undo
    // step. Any thread: the collections are read-only once opened.
    class BuiltinSoundImport {
    public:
        // clips per import, after expansion
        static constexpr size_t MAX_CLIPS = 48;

        // What to do with a clip whose file already exists
        enum class Existing {
            Replace,         // write it again (import_files overwrite: true)
            SkipIdentical,   // equal bytes: skipped; different bytes: left alone and listed (the menu)
            RefuseDifferent  // equal bytes: skipped; different bytes fail the plan (import_files)
        };

        struct Clip {
            bool library = false;
            const SoundManifestClip* entry = nullptr;

            // "essentials:<id>" or "library:<id>"
            std::string reference() const;
            // the bytes: the embedded essential, or SoundLibrary::read (size and SHA-256 checked)
            bool read(std::string& bytes, std::string& error) const;
            AudioAnalysis::Labels labels() const;
            // How an import of it plays: the manifest's fades in frames at a rate
            ClipImportSettings played(unsigned int sampleRate) const;
        };

        struct File {
            std::filesystem::path path;     // absolute
            std::string content;
        };

        struct Plan {
            std::vector<File> writes;                     // clips and metas to write
            std::vector<std::filesystem::path> imported;  // the clips written
            std::vector<std::filesystem::path> skipped;   // equal to the built-in clip already
            std::vector<std::filesystem::path> conflicts; // different files left alone
        };

        static bool isReference(const std::string& text);
        // References ("<prefix><id>", "<prefix><group>/*", "<prefix>*") to their clips, each once, in
        // the order given. Fails on one that matches nothing, and past MAX_CLIPS (with the count).
        static bool resolve(const std::vector<std::string>& refs, std::vector<Clip>& out, std::string& error);
        // The undo step's name: "Import Sound Essentials", "Import Starter Library Sounds" or "Import Built-in Sounds"
        static std::string stepName(const std::vector<Clip>& clips);
        // A clip's bytes as they are built in (the SHA-256 is checked against the manifest) and the
        // analysis through the project's cache, by the bytes' SHA-1, as files are
        static bool analyze(const Clip& clip, const std::filesystem::path& cacheDirectory, nlohmann::json& out, std::string& error,
                            AudioAnalysis::Signal* decoded = nullptr);

        // The files of an import of `clips` into `directory`. A .meta that does not parse, a clip whose
        // bytes can't be read, or two clips with one destination fail the whole plan, naming the
        // clip, before anything is written.
        static bool plan(const std::vector<Clip>& clips, const std::filesystem::path& directory, Existing existing, Plan& plan, std::string& error);
    };

}
