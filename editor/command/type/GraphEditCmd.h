// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor{

    // A batch of graph edits (graph/Graph.h ops) on a graph file, as one step of the project
    // history (S23 R24, S24 names): the file's text before and after. "Connect Speed → Blend",
    // "Edit Graph (3 Changes)".
    class GraphEditCmd: public Command{
    private:
        std::filesystem::path file;     // absolute
        std::string before;
        std::string after;
        std::string label;
        bool creates = false;   // the step made the file: undo removes it (T2 R16)
        bool write(const std::string& text) const;

    public:
        GraphEditCmd(std::filesystem::path file, std::string before, std::string after, std::string label, bool createsFile = false);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return label; }
        const char* typeName() const override { return "GraphEditCmd"; }
        bool isNoOp() const override { return !creates && before == after; }
        size_t memoryBytes() const override { return sizeof(*this) + before.size() + after.size(); }
        bool releaseOffThread() const override { return true; }
    };

}
