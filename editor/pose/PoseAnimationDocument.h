// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's working copy of one .poseanim file (spec: pose animation phase 2, section 5),
// shared per path by everything that edits or shows it: the MCP pose tools now, the Pose
// Animation window and the viewport later. Every edit goes through commit(): the new data is
// clamped to the value ranges (PoseRanges, reported), written as the deterministic file text
// through the project command history (WriteFilesCmd: one undo step, Ctrl+Z and MCP `undo`
// with scope project both undo it) and read back, so data() is always what the file holds.
// Undo, redo and changes made outside the editor (McpAutoSync, the Project window's poll) reach
// the document through filesChanged() / checkDiskChanges(), which reload it and notify the
// listeners.
//
// Main thread only.

#pragma once

#include "pose/PoseAnimationIO.h"
#include "pose/PoseMetrics.h"
#include "pose/PoseRanges.h"
#include "pose/PoseRig.h"
#include "pose/PoseSampler.h"
#include "pose/PoseTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // A pose animation realised on one rig: what previews, metrics, baking and auto timing use
    struct PoseRealisation{
        std::shared_ptr<pose::PoseRig> rig;
        uint64_t revision = 0;                  // the document revision it was made from
        std::vector<pose::HumanPose> keys;      // per key pose (joint limits first when humanize.joint_limits)
        std::vector<pose::PoseMetrics> metrics; // per key pose; limit hits from the key's own angles
        std::vector<pose::HumanPose> frames;    // pose::frames at frame_rate (f = 0..count)
        pose::PoseTimeline timeline;
    };

    class PoseAnimationDocument{
    public:
        enum class Change { Edited, Reloaded, Removed };
        using Listener = std::function<void(PoseAnimationDocument&, Change)>;

        // An extra file written in the same undo step as the animation (the .humanoid sidecar)
        struct ExtraFile {
            std::filesystem::path path;     // absolute
            std::string content;
        };

        // The document of a project-relative (or absolute, inside the project) .poseanim path,
        // shared per path; loads it on first use. Null + error when the file is missing,
        // outside the project or not a pose animation (wrong format, newer version).
        static std::shared_ptr<PoseAnimationDocument> open(Project* project, const std::string& path, std::string* error);
        // Creates the file (fails if it exists) with `initial` as one undo step and opens it
        static std::shared_ptr<PoseAnimationDocument> create(Project* project, const std::string& path,
                                                             const pose::PoseAnimationData& initial, const std::string& undoName,
                                                             std::vector<pose::ClampReport>* clamped, std::string* error,
                                                             const std::vector<ExtraFile>& extraFiles = {});

        // Files changed on disk (absolute paths; written by a command, its undo/redo, a tool or
        // another program): the documents of those files reload
        static void filesChanged(const std::vector<std::filesystem::path>& paths);
        // Stats every open document's file and reloads the changed ones (the editor's poll)
        static void checkDiskChanges();
        // Called after a WriteFilesCmd of a document ran, was undone or redone (the MCP server
        // sets it to its file sync: asset database, Project window, McpAutoSync stamps)
        static void setFilesWrittenHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);

        const std::string& path() const { return relativePath; }            // project-relative, generic
        const std::filesystem::path& absolutePath() const { return filePath; }
        const pose::PoseAnimationData& data() const { return current; }
        // Increases on every change of data() (edit, undo, redo, reload)
        uint64_t revision() const { return rev; }
        // Warnings of the last load (unknown keys, clamped values in the file)
        const std::vector<std::string>& loadWarnings() const { return warnings; }
        // False after the file was deleted (an undone creation): commit() fails until it is back
        bool exists() const { return onDisk; }

        // The single edit path: clamps `newData` to the value ranges (reports appended to
        // `clamped`), writes the file through the project command history as one undo step and
        // reloads. A non-empty `mergeKey` merges with the previous edit of the same key (drags).
        // An edit that changes nothing writes nothing (true, no undo step). `extraFiles` are
        // written in the same step.
        bool commit(const pose::PoseAnimationData& newData, const std::string& undoName, const std::string& mergeKey,
                    std::vector<pose::ClampReport>* clamped, std::string* error,
                    const std::vector<ExtraFile>& extraFiles = {});

        // The animation realised on `rig`, cached by revision and rig (null rig: null)
        std::shared_ptr<const PoseRealisation> realise(const std::shared_ptr<pose::PoseRig>& rig);
        // The same for any data (not cached): auto timing and edits before their commit
        static std::shared_ptr<PoseRealisation> realiseData(const pose::PoseAnimationData& data, const std::shared_ptr<pose::PoseRig>& rig);

        // Reads the file again (when its text changed) and notifies the listeners
        void reloadFromDisk();

        int addListener(Listener listener);
        void removeListener(int id);

        // The text commit() would write for `data` (clamped, deterministic)
        static std::string fileText(const pose::PoseAnimationData& data);

    private:
        Project* project = nullptr;
        std::string relativePath;
        std::filesystem::path filePath;
        pose::PoseAnimationData current;
        std::string text;                   // the file text data() was read from
        std::vector<std::string> warnings;
        uint64_t rev = 0;
        bool onDisk = false;
        bool committing = false;            // inside commit(): reloads are this document's own edit
        int64_t stampTime = 0;              // file mtime (ns) and size when last read
        uint64_t stampSize = 0;
        std::map<int, Listener> listeners;
        std::shared_ptr<const PoseRealisation> realised;   // the last realise()
        int nextListener = 1;

        PoseAnimationDocument() = default;
        bool load(std::string* error);
        void notify(Change change);
        void takeStamp();
        bool stampChanged() const;
        bool writeUndoable(const std::string& newText, const std::string& mergeKey,
                           const std::vector<ExtraFile>& extraFiles, std::string* error);
    };

}
