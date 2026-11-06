// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/TextBaselines.h"

#include "jobs/JobSystem.h"
#include "util/LineDiff.h"

#include "Out.h"

#include <string_view>

namespace jasperengine::editor::vcs {

namespace {

constexpr std::string_view kLfsPointer = "version https://git-lfs.github.com/spec/v1";

}

const char* baselineStateName(BaselineState state) {
    switch (state) {
        case BaselineState::None: return "none";
        case BaselineState::Pending: return "pending";
        case BaselineState::Ready: return "ready";
        case BaselineState::NewFile: return "new_file";
        case BaselineState::Untracked: return "untracked";
        case BaselineState::Ignored: return "ignored";
        case BaselineState::OutsideRepository: return "outside_repository";
        case BaselineState::Binary: return "binary";
        case BaselineState::LfsPointer: return "lfs_pointer";
        case BaselineState::TooLarge: return "too_large";
        case BaselineState::Failed: return "failed";
    }
    return "none";
}

const char* baselineStateLabel(BaselineState state) {
    switch (state) {
        case BaselineState::None: return "";
        case BaselineState::Pending: return "Reading baseline...";
        case BaselineState::Ready: return "";
        case BaselineState::NewFile: return "New file";
        case BaselineState::Untracked: return "Untracked";
        case BaselineState::Ignored: return "Ignored";
        case BaselineState::OutsideRepository: return "Outside the repository";
        case BaselineState::Binary: return "Not compared: binary";
        case BaselineState::LfsPointer: return "Not compared: Git LFS file";
        case BaselineState::TooLarge: return "Not compared: over 4 MiB";
        case BaselineState::Failed: return "Not compared";
    }
    return "";
}

BaselineState TextBaselines::classify(std::string_view bytes) {
    if (bytes.size() > kMaxBytes) return BaselineState::TooLarge;
    if (bytes.rfind(kLfsPointer, 0) == 0) return BaselineState::LfsPointer;
    if (bytes.find('\0') != std::string_view::npos) return BaselineState::Binary;
    return BaselineState::Ready;
}

void TextBaselines::assign(TextBaseline& baseline, BaselineState state, std::shared_ptr<const std::vector<std::string>> lines,
                           const std::string& revision, const std::string& error) {
    // the same text read again (a refetch after a save) changes nothing on screen
    const bool sameLines = baseline.lines == lines || (baseline.lines && lines && *baseline.lines == *lines);
    if (baseline.state == state && sameLines && baseline.revision == revision && baseline.error == error) return;
    baseline.state = state;
    if (!sameLines) baseline.lines = std::move(lines);
    baseline.revision = revision;
    baseline.error = error;
    baseline.version = nextVersion++;
}

void TextBaselines::update(VcsService& service, std::span<const std::filesystem::path> openFiles, std::function<void()> wake) {
    if (!service.active()) {
        clear();
        return;
    }
    const std::string revision = service.baselineRevision();
    const SnapshotPtr& snapshot = service.snapshot();

    std::map<std::filesystem::path, Entry> kept;
    for (const std::filesystem::path& file : openFiles) {
        auto found = entries.find(file);
        Entry entry = found != entries.end() ? std::move(found->second) : Entry();

        RepoPath key;
        const VcsChange* change = nullptr;
        BaselineState fixed = BaselineState::None;
        if (!service.toKey(file, key)) {
            fixed = BaselineState::OutsideRepository;
        } else if (service.isIgnored(file)) {
            fixed = BaselineState::Ignored;
        } else if ((change = snapshot ? snapshot->find(key) : nullptr)) {
            if (change->index == FileState::Untracked) fixed = BaselineState::Untracked;
            else if (change->newFile()) fixed = BaselineState::NewFile;
        }
        if (fixed != BaselineState::None) {
            entry.read.reset();
            entry.memo.clear();
            assign(entry.baseline, fixed, nullptr, revision, std::string());
            kept[file] = std::move(entry);
            continue;
        }

        // a staged rename compares with the file it came from
        const RepoPath source = change && !change->oldPath.empty() ? change->oldPath : key;
        const std::string memo = service.contentMemoKey(key, revision);
        if (memo != entry.memo) {
            entry.memo = memo;
            entry.read = std::make_shared<Read>();
            std::shared_ptr<IVcsProvider> provider = service.providerForReads();
            if (!provider) {
                std::lock_guard<std::mutex> lock(entry.read->mutex);
                entry.read->done = true;
                entry.read->state = BaselineState::Failed;
                entry.read->error = "Version control is not active";
            } else {
                // a process: on the blocking process channel, never the main thread (S29 R24)
                std::shared_ptr<Read> read = entry.read;
                jobs::system().processChannel().submit([provider, source, revision, read, wake]() {
                    std::string text;
                    // a blob over the limit is not kept whole (only enough of it to see it is over)
                    const VcsResult result = provider->baseContent(source, revision, text, kMaxBytes);
                    BaselineState state = result.ok ? classify(text) : BaselineState::Failed;
                    std::shared_ptr<const std::vector<std::string>> lines;
                    if (state == BaselineState::Ready) {
                        lines = std::make_shared<const std::vector<std::string>>(LineDiff::splitLines(text));
                    }
                    {
                        std::lock_guard<std::mutex> lock(read->mutex);
                        read->state = state;
                        read->lines = std::move(lines);
                        read->error = result.ok ? std::string() : result.message;
                        read->done = true;
                    }
                    if (wake) wake();
                }, jobs::SubmitOptions{.name = "vcs-text-baseline"});
            }
            // the old markers stay until the new baseline arrives (no flicker to "no changes")
            if (entry.baseline.state == BaselineState::None || entry.baseline.lines == nullptr) {
                assign(entry.baseline, BaselineState::Pending, nullptr, revision, std::string());
            }
        }
        if (entry.read) {
            std::lock_guard<std::mutex> lock(entry.read->mutex);
            if (entry.read->done) {
                if (entry.read->state == BaselineState::Failed && entry.baseline.state != BaselineState::Failed) {
                    Out::warning("Version control: no baseline for %s: %s", key.c_str(), entry.read->error.c_str());
                }
                assign(entry.baseline, entry.read->state, entry.read->lines, revision, entry.read->error);
                entry.read.reset();
            }
        }
        kept[file] = std::move(entry);
    }
    entries = std::move(kept);
}

const TextBaseline& TextBaselines::get(const std::filesystem::path& absolute) const {
    static const TextBaseline none;
    auto found = entries.find(absolute);
    return found != entries.end() ? found->second.baseline : none;
}

void TextBaselines::clear() {
    entries.clear();
}

}
