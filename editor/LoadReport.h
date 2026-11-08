// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a document load could not read the way it was written (docs/data-safety.md,
// docs/specs/S03-data-safety.md R25). Each issue says what (unknown component, field, enum
// value...), where (entity, component, field, file:line) and its fate: Preserved (kept
// verbatim, written back on save), Dropped (lost by the next save: the save is gated) or Info
// (a migration ran).
//
// Loaders install a ScopedLoadReport around a decode; the decoders record into it. With no
// report installed (clipboard, duplicate, undo) nothing is recorded, but preservation still
// applies (StreamPreserved.h).

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class PreservedStore;

    enum class DegradedSavePolicy {
        // a save that would drop data writes nothing (the default everywhere)
        Refuse,
        // the user or agent accepted the loss
        SaveAnyway
    };

    enum class SaveResult {
        Saved,
        Failed,
        // nothing written: the document did not load completely (DegradedSavePolicy::Refuse)
        RefusedDegraded
    };

    enum class LoadIssueKind {
        UnknownComponent,
        UnknownField,
        UnknownEnumValue,
        InvalidValue,
        UnknownEntityType,
        UnknownRootKey,
        NewerFormat,
        Migrated,
        // a value the decoder read and resolved another way (a sheet sprite's frame name its sheet
        // lacks: the saved index is shown, S44 R48)
        Fallback
    };

    enum class LoadIssueFate {
        Preserved,
        Dropped,
        Info
    };

    struct LoadIssue {
        LoadIssueKind kind = LoadIssueKind::UnknownField;
        LoadIssueFate fate = LoadIssueFate::Preserved;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        // "Player/Arm" (names from the root down), empty for document-level issues
        std::string entityPath;
        // "Light"; "#scene" for the scene settings block; empty at entity or document root
        std::string component;
        // "type", "submeshes[0].foo"; the key for root-level issues
        std::string field;
        // "unknown value", "unknown component", "not a number"
        std::string message;
        // the authored scalar, when the issue is about one ("area")
        std::string value;
        // the nearest known name, empty when nothing is close
        std::string suggestion;
        std::string file;
        // 1-based; 0 when the node has no position (built in memory)
        int line = 0;
        int column = 0;

        // The text a person reads: `Player › Light.type = "area": unknown value (did you mean "spot"?) — Level1.scene:142`
        std::string describe() const;
        // One console line: `Level1.scene:142: Light.type: unknown value "area" (did you mean "spot"?) — kept`
        std::string consoleLine() const;
        // "Light.type", "Jumpable", "lighting2"
        std::string location() const;
    };

    const char* loadIssueKindName(LoadIssueKind kind);   // "unknown_component", ...
    const char* loadIssueFateName(LoadIssueFate fate);   // "preserved", "dropped", "info"

    struct LoadReport {
        std::string documentPath;
        int versionRead = 0;
        int versionCurrent = 0;
        std::vector<LoadIssue> issues;

        size_t dropped() const;
        size_t preserved() const;
        size_t infos() const;
        bool newerFormat() const;
        // Issues that still apply: every dropped one, and the preserved ones whose entry the
        // store still holds (a preserved value retires once the live value is written)
        std::vector<const LoadIssue*> outstanding(const PreservedStore* store) const;
        size_t outstandingDropped() const { return dropped(); }
        size_t outstandingPreserved(const PreservedStore* store) const;
        // A save without the dropped data (Save Anyway, Save As): the file no longer has it
        void clearDropped();
        void clear();
        bool empty() const;
    };

    // The report decoders on this thread record into, while the object lives. Nested scopes
    // (a prefab loaded while a scene loads) record into the innermost one.
    class ScopedLoadReport {
    public:
        ScopedLoadReport(LoadReport& report, uint32_t sceneId = 0);
        ~ScopedLoadReport();
        ScopedLoadReport(const ScopedLoadReport&) = delete;
        ScopedLoadReport& operator=(const ScopedLoadReport&) = delete;

        // The installed report, nullptr when none
        static LoadReport* current();
        static uint32_t currentSceneId();

    private:
        LoadReport* previous;
        uint32_t previousSceneId;
    };

    // Records an issue into the installed report (nothing when none). The scene id and file
    // are filled in from the scope.
    void recordLoadIssue(LoadIssue issue);

    // The console lines of a load: one per issue and a summary
    // ("Loaded Level1.scene with 8 issues (5 kept, 3 dropped).") Nothing for a clean load.
    void logLoadReport(const LoadReport& report);

}
