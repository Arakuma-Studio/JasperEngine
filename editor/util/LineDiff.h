// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The lines that differ between two texts (the code editor's change markers, diff_file of an
// open buffer). Myers' diff with the clean-up passes of VS Code's line diff
// (defaultLinesDiffComputer, MIT, Copyright (c) Microsoft Corporation; NOTICE.md), so markers land
// on the lines a user expects.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor {

    // Lines that differ, with exclusive ends. An empty original range is an insertion, an empty
    // modified range a deletion.
    struct LineChange {
        enum class Kind : uint8_t { Added, Modified, Deleted };

        int originalStart = 0;
        int originalEnd = 0;
        int modifiedStart = 0;
        int modifiedEnd = 0;

        Kind kind() const {
            if (originalStart == originalEnd) return Kind::Added;
            if (modifiedStart == modifiedEnd) return Kind::Deleted;
            return Kind::Modified;
        }
        bool operator==(const LineChange& other) const = default;
    };

    const char* lineChangeKindName(LineChange::Kind kind);   // "added", "modified", "deleted"

    class LineDiff {
    public:
        // Past this many edits the differing middle is one change
        static constexpr int kMaxEdits = 1000;

        // Lines compare byte for byte: callers drop '\r' first
        static std::vector<LineChange> compute(std::span<const std::string> original, std::span<const std::string> modified);

        // A unified diff of two texts as git prints it ("--- a/<old>", "+++ b/<new>", "@@ -a,b +c,d @@",
        // 3 lines of context): lines compared byte for byte ('\r' kept), "\ No newline at end of file"
        // after a last line without one; empty when the texts are equal
        static std::string unified(std::string_view original, std::string_view modified, std::string_view oldName,
                                   std::string_view newName);

        // Text split at '\n' with '\r' dropped, as the code editor holds it
        static std::vector<std::string> splitLines(std::string_view text);
    };

}
