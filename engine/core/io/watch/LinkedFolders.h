// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_LINKEDFOLDERS_H
#define JASPERENGINE_LINKEDFOLDERS_H

#include "Export.h"

#include <cstddef>
#include <filesystem>
#include <set>
#include <system_error>
#include <vector>

namespace jasperengine::watch {

    // A link of any kind at a folder entry: a symbolic link, and an NTFS junction or mount point on
    // Windows (MSVC reports those as file_type::junction, not as symlinks). No file system call
    // where the listing told the type.
    JASPERENGINE_API bool isLink(const std::filesystem::directory_entry& entry);
    JASPERENGINE_API bool isLink(const std::filesystem::path& path);

    // Where a path is on disk, every link on the way followed; empty when it leads nowhere
    JASPERENGINE_API std::filesystem::path realPathOf(const std::filesystem::path& path);
    // `path` is `dir` or below it (both real paths)
    JASPERENGINE_API bool isAtOrBelowReal(const std::filesystem::path& path, const std::filesystem::path& dir);

    // Decides which links a walk of a project goes on through, so a folder linked in from elsewhere
    // is walked as a folder of the project, under the link's name (S68 R7). A link is entered when it
    // leads to a folder out of the root, once per folder per walk. Not entered:
    // - a link inside the root: its target is walked under its own name, and that is the file's path;
    // - a link to a folder holding the root (a cycle, and the walk would leave for the whole disk);
    // - a second link to a folder already entered: `loop` inside a linked folder leading to itself
    //   ends here, and so does any longer cycle, as every folder is entered once.
    class JASPERENGINE_API LinkFollower {
    public:
        explicit LinkFollower(const std::filesystem::path& root);
        // The real path of the folder the link at `path` leads to when the walk enters it, else empty
        std::filesystem::path enter(const std::filesystem::path& path);
        // A folder another walk entered already (the rest of an index, around a partial walk)
        void alreadyEntered(const std::filesystem::path& real) { entered.insert(real); }

    private:
        std::filesystem::path realRoot;
        std::set<std::filesystem::path> entered;
    };

    // std::filesystem::recursive_directory_iterator's interface for a walk of a project: the same
    // entries, each under its lexical path, and links LinkFollower lets through entered as folders.
    // A project without links costs what the standard iterator costs (the links' only check is the
    // entry type the listing already told). Past MAX_DEPTH levels nothing is entered.
    class JASPERENGINE_API LinkedDirectoryIterator {
    public:
        static constexpr size_t MAX_DEPTH = 256;

        LinkedDirectoryIterator() = default;
        LinkedDirectoryIterator(const std::filesystem::path& root, std::filesystem::directory_options options, std::error_code& ec);

        const std::filesystem::directory_entry& operator*() const { return *stack.back(); }
        const std::filesystem::directory_entry* operator->() const { return &*stack.back(); }
        LinkedDirectoryIterator& increment(std::error_code& ec);
        void disable_recursion_pending() { recursionPending = false; }
        int depth() const { return static_cast<int>(stack.size()) - 1; }

        // Only an iterator that ended equals another: the standard iterator's rule
        bool operator==(const LinkedDirectoryIterator& other) const { return stack.empty() && other.stack.empty(); }
        bool operator!=(const LinkedDirectoryIterator& other) const { return !(*this == other); }

    private:
        std::vector<std::filesystem::directory_iterator> stack;
        std::filesystem::directory_options options = std::filesystem::directory_options::none;
        std::filesystem::path root;
        std::vector<LinkFollower> follower;   // made at the first link: a project without links pays nothing
        bool recursionPending = false;
    };

}

#endif // JASPERENGINE_LINKEDFOLDERS_H
