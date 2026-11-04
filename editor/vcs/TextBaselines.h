// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The baselines of the files open in the code editor (docs/superpowers/specs/
// 2026-11-04-code-editor-git-design.md 6.2): each file's text at the baseline revision (HEAD for git,
// BASE for svn, S29 D5), fetched off the main thread on the process channel and kept per
// VcsService::contentMemoKey, so a file is read again only when HEAD moves, its blob changes or
// it is renamed, never per poll. Files with nothing to compare against get a state saying why.

#include "vcs/VcsService.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::editor::vcs {

    enum class BaselineState : uint8_t {
        None,               // not asked for, or version control is off
        Pending,            // being read
        Ready,              // lines hold the baseline
        NewFile,            // added, not in the baseline yet
        Untracked,
        Ignored,
        OutsideRepository,
        Binary,             // the baseline has a NUL byte
        LfsPointer,         // the baseline is a Git LFS pointer
        TooLarge,           // over kMaxBytes
        Failed              // the tool failed: error says why
    };
    const char* baselineStateName(BaselineState state);   // "none", "pending", "ready", "new_file", ...
    // What the code editor's status row says for a state without markers ("New file", ...)
    const char* baselineStateLabel(BaselineState state);

    struct TextBaseline {
        BaselineState state = BaselineState::None;
        std::shared_ptr<const std::vector<std::string>> lines;   // Ready: split at '\n', '\r' dropped
        std::string revision;                                     // "HEAD" / "BASE"
        std::string error;                                        // Failed: the tool's message
        uint64_t version = 0;                                     // grows when lines or state change
    };

    class TextBaselines {
    public:
        static constexpr size_t kMaxBytes = 4u << 20;

        // Main thread, once per frame: asks for what the open files need, adopts finished reads,
        // forgets files no longer open. Nothing while the service is not active. wake runs on the
        // reading thread when a read finishes (the editor loop may be idle).
        void update(VcsService& service, std::span<const std::filesystem::path> openFiles, std::function<void()> wake = {});
        // None for a file not asked for
        const TextBaseline& get(const std::filesystem::path& absolute) const;
        void clear();

        // How a baseline's bytes read (the reading thread): Binary, LfsPointer, TooLarge or Ready
        static BaselineState classify(std::string_view bytes);

    private:
        // filled by its read job (under its own mutex: the job may outlive the cache)
        struct Read {
            std::mutex mutex;
            bool done = false;
            BaselineState state = BaselineState::Pending;
            std::shared_ptr<const std::vector<std::string>> lines;
            std::string error;
        };
        struct Entry {
            TextBaseline baseline;
            std::string memo;                 // the key the baseline (or the read) is for
            std::shared_ptr<Read> read;
        };
        // set the state, growing the version when something changed
        void assign(TextBaseline& baseline, BaselineState state, std::shared_ptr<const std::vector<std::string>> lines,
                    const std::string& revision, const std::string& error);

        std::map<std::filesystem::path, Entry> entries;
        uint64_t nextVersion = 1;
    };

}
