// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Format versions and migrations of the editor's YAML documents (docs/data-safety.md,
// docs/specs/S03-data-safety.md R20-R23).
//
// Every document kind has an integer format version written as the first key of its root
// map: `formatVersion` for scenes, project.yaml and .material files, `version` for prefabs.
// A file without the key is version 0 (prefabs: 1, the files written before the header).
//
// Loaders call migrate() on the parsed YAML before any decode, so decoders only ever see the
// current shape. Migrations are pure YAML -> YAML functions, with no editor state, so the CLI
// and tests run them too. A migrated document is not marked modified; it is written in the
// current format on its next save.
//
// Bump rule (every spec): adding an optional component or field does NOT bump a version
// (defaults and unknown-data preservation cover it). Renaming, moving or re-typing a key, or
// changing what a value means, bumps the kind's version and adds a migration here, with a
// fixture under editor/tests/fixtures/migrations/ and a test.
//
// Adding a migration:
//   1. raise the kind's current version in currentVersion();
//   2. write `void sceneV1ToV2(YAML::Node& root, MigrationLog& log)` in StreamMigrations.cpp;
//   3. append {1, 2, "what it changes", sceneV1ToV2} to the kind's table.

#include "yaml-cpp/yaml.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    enum class DocumentKind {
        Scene,
        Project,
        Material,
        Prefab
    };

    // What the migrations of one load did, for the load report and the console
    struct MigrationLog {
        std::vector<std::string> lines;
        void info(std::string line) { lines.push_back(std::move(line)); }
    };

    using MigrationFunction = void (*)(YAML::Node& root, MigrationLog& log);

    struct Migration {
        int from;
        int to;
        const char* name;
        MigrationFunction apply;
    };

    struct MigrationResult {
        int versionRead = 0;
        int versionCurrent = 0;
        // The names of the migrations that ran, in order
        std::vector<std::string> applied;
        MigrationLog log;

        bool migrated() const { return !applied.empty(); }
        // Written by a newer editor: loaded best-effort, nothing migrated, saves gated
        bool newer() const { return versionRead > versionCurrent; }
    };

    class StreamMigrations {
    public:
        // "formatVersion", or "version" for prefabs
        static const char* versionKey(DocumentKind kind);
        // The version this editor writes
        static int currentVersion(DocumentKind kind);
        // The version of a file without the key
        static int absentVersion(DocumentKind kind);
        // "scene", "project", "material", "prefab"
        static const char* kindName(DocumentKind kind);

        // The root's version (absentVersion() when it has none or it is not a number)
        static int readVersion(DocumentKind kind, const YAML::Node& root);

        // The ordered migrations of a kind (from < to, contiguous)
        static const std::vector<Migration>& table(DocumentKind kind);

        // Runs every migration from the root's version up to the current one and stamps the
        // current version. A newer file is left untouched. Prefabs older than the first entry
        // of their table are upgraded by the prefab loader itself (Prefab::loadRegistry).
        static MigrationResult migrate(DocumentKind kind, YAML::Node& root);

        // Sets the version key to `version` as the first key of the root map
        static void stampVersion(DocumentKind kind, YAML::Node& root, int version);
        static void stampCurrentVersion(DocumentKind kind, YAML::Node& root);

        // "Written by a newer JasperEngine (scene format 2; this editor writes 1)."
        static std::string newerFormatMessage(DocumentKind kind, int versionRead);
    };

}
