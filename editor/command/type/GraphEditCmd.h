// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // A batch of graph edits (graph/Graph.h ops) on a graph file, as one step of the project
    // history (S23 R24, S24 names): the file's text before and after. "Connect Speed → Blend",
    // "Edit Graph (3 Changes)". An open window or a pool of the kind learns of these edits through a
    // change listener; a file written any other way (write_file, another program) reaches it through
    // HotReloadService, kind "graph".
    class GraphEditCmd: public Command{
    private:
        std::filesystem::path file;     // absolute
        std::string before;
        std::string after;
        std::string label;
        bool creates = false;   // the step made the file: undo removes it (T2 R16)
        bool write(const std::string& text) const;

    public:
        using ChangeListener = std::function<void(const std::filesystem::path& file)>;

        GraphEditCmd(std::filesystem::path file, std::string before, std::string after, std::string label, bool createsFile = false);

        // Called after each write of execute (a redo too) and undo, with the absolute path; not when
        // the write failed. Main thread, like the history. Returns what removes it: do that before
        // what it captures goes. It runs inside the step, before the history records it, so it must
        // not add a command.
        static uint64_t addChangeListener(ChangeListener listener);
        static void removeChangeListener(uint64_t id);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return label; }
        const char* typeName() const override { return "GraphEditCmd"; }
        bool isNoOp() const override { return !creates && before == after; }
        size_t memoryBytes() const override { return sizeof(*this) + before.size() + after.size(); }
        bool releaseOffThread() const override { return true; }
        std::vector<Touched> touchedPaths(bool) const override { return {{file, false}}; }
    };

}
