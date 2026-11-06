// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AudioAnalysis.h"
#include "SoundManifest.h"

#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::editor {

    namespace fs = std::filesystem;

    struct SoundLibraryStatus {
        bool available = false;       // manifest parsed and valid
        fs::path root;
        int version = 0;
        size_t clips = 0;             // the manifest's clips
        uint64_t bytes = 0;           // sum of the manifest's clip bytes
        size_t missing = 0;           // clips whose file is absent, not a regular file, or of the wrong size
        bool analysisCurrent = false; // analysis.json's analyzer_version == AudioAnalysis::VERSION
        std::string error;            // why it is not available
    };

    // The starter library: its CC0 clips (status().clips) with
    // manifest.json, analysis.json and licenses/, installed next to the editor
    // (FileUtils::getSoundLibraryDir) and read there in place, never written. Opened on first use
    // (any thread, under a mutex): the manifest is parsed and validated (SoundManifest) and every
    // clip's path is looked at with symlink_status (a regular file of the manifest's size counts;
    // anything else is missing and that clip unavailable); no file is hashed then. read() checks
    // the size and the SHA-256 of what it returns, and is the only way a library file is read.
    // Paths are built from validated ids only (root / (id + ".ogg")). A missing or invalid
    // library leaves status().available false with the reason; everything else in the editor works.
    class SoundLibrary {
    public:
        using Json = nlohmann::json;

        static constexpr const char* PREFIX = "library:";
        static constexpr const char* ENV_DIR = "JASPERENGINE_SOUND_LIBRARY_DIR";
        // the UI's default import folder, under the assets directory
        static constexpr const char* IMPORT_FOLDER = "sounds/library";
        static constexpr const char* MANIFEST = "manifest.json";
        static constexpr const char* ANALYSIS = "analysis.json";
        static constexpr const char* LICENCES = "licenses";
        static constexpr uint64_t MAX_MANIFEST_BYTES = 8ull * 1024 * 1024;
        static constexpr uint64_t MAX_ANALYSIS_BYTES = 8ull * 1024 * 1024;
        // the whole tree (audio, manifest, analysis, licences): spec 2.3.1's hard cap, 36 MiB
        static constexpr uint64_t MAX_TREE_BYTES = 37748736;
        // a licence file's CC0 markers
        static constexpr const char* LICENCE_MARKER = "Creative Commons Zero";
        static constexpr const char* LICENCE_URL_MARKER = "publicdomain/zero/1.0";

        // The library at FileUtils::getSoundLibraryDir(), opened on first use
        static SoundLibrary& get();

        // Opens the library at root instead (tests and the CLI's --dir). What the earlier library
        // handed out stays valid: an opened library is never freed.
        void reopen(const fs::path& root);

        SoundLibraryStatus status() const;
        // The whole manifest (every clip, available or not); empty when unavailable
        const SoundManifest& manifest() const;
        // The available clips (the manifest's minus the missing), sorted by id
        const std::vector<SoundManifestClip>& all() const;
        // An available clip by id (without "library:"), nullptr otherwise
        const SoundManifestClip* find(const std::string& id) const;
        // The types (the ids' groups) of the available clips, sorted
        std::vector<std::string> types() const;

        static bool isReference(const std::string& text);
        // "library:<id>", "library:<type>/*" or "library:*" -> its available clips. Nothing found
        // fails: an unknown id or type with the types, a manifest clip whose file is missing with
        // that, an unavailable library with status().error.
        std::vector<const SoundManifestClip*> match(const std::string& ref, std::string& error) const;

        // Reads the clip's file, checks size and SHA-256 against the manifest; refuses symlinks.
        // A mismatch fails with "library:<id> does not match its manifest (SHA-256 …, expected …):
        // the installed library was modified; reinstall the editor", logged once per id.
        bool read(const SoundManifestClip& clip, std::vector<uint8_t>& bytes, std::string& error) const;
        bool read(const SoundManifestClip& clip, std::string& bytes, std::string& error) const;

        // The analysis.json item when the file is current (its analyzer_version is
        // AudioAnalysis::VERSION) and the item's sha1 is the SHA-1 of the clip's bytes (read once
        // per clip, through read()); nullptr otherwise. analysis.json is parsed on first use.
        const Json* analysis(const std::string& id) const;

        struct VerifyIssue {
            std::string path;
            std::string problem;
        };
        // Every clip's file and SHA-256; files under the root that are neither a manifest clip nor
        // manifest.json, analysis.json or licenses/* ("not in the manifest"; symbolic links
        // included); every pack's licence file with both CC0 markers (and its credits file); the
        // tree within MAX_TREE_BYTES. An unavailable library is one issue.
        std::vector<VerifyIssue> verify() const;

        // root / (id + ".ogg"), for the preview and the CLI only (reading goes through read())
        fs::path absolutePath(const SoundManifestClip& clip) const;

        // A clip's labels as the analysis shows them (type, the manifest's kind, tags, licence)
        static AudioAnalysis::Labels labels(const SoundManifestClip& clip);

        // Out::warning for read failures (on by default); the CLI turns it off, because without a
        // Console window Out prints to stdout, where its JSON goes
        void setLogWarnings(bool on);

    private:
        struct Data;

        SoundLibrary() = default;
        const Data& data() const;
        static std::unique_ptr<Data> open(const fs::path& root);
        void warnOnce(const std::string& id, const std::string& message) const;

        mutable std::mutex openMutex;
        mutable std::atomic<const Data*> current{nullptr};
        // every library opened, kept for the process's lifetime (reopen leaves references valid)
        mutable std::vector<std::unique_ptr<Data>> opened;
        std::atomic<bool> logWarnings{true};
    };

}
