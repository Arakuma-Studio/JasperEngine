// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FileChangeCoalescer.h"

#include <algorithm>
#include <unordered_map>

using namespace jasperengine;

namespace fs = std::filesystem;

FileChangeCoalescer::FileChangeCoalescer(Environment& environment)
    : env(environment) {
}

FileChangeCoalescer::FileChangeCoalescer(Environment& environment, const Settings& settings)
    : env(environment), settings(settings) {
}

fs::path FileChangeCoalescer::pathOf(const std::string& key) {
    return watch::pathFromUtf8(key);
}

bool FileChangeCoalescer::isTemporary(const std::string& key) {
    const size_t slash = key.find_last_of('/');
    return watch::isTemporaryName(slash == std::string::npos ? key : key.substr(slash + 1));
}

void FileChangeCoalescer::setQuietWindow(std::chrono::milliseconds quiet) {
    settings.quiet = std::clamp(quiet, std::chrono::milliseconds(50), std::chrono::milliseconds(2000));
}

size_t FileChangeCoalescer::pendingCount() const {
    return entries.size() + rescans.size();
}

void FileChangeCoalescer::clear() {
    entries.clear();
    rescans.clear();
}

FileChangeCoalescer::Entry& FileChangeCoalescer::fresh(const std::string& key, State state, Clock::time_point now) {
    Entry& entry = entries[key];
    entry = Entry{};
    entry.state = state;
    entry.order = nextOrder++;
    entry.first = now;
    entry.last = now;
    return entry;
}

void FileChangeCoalescer::touch(Entry& entry, Clock::time_point now) {
    entry.last = now;
    entry.sampled = false;
    coalesced++;
}

void FileChangeCoalescer::add(const watch::RawEvent& event, Clock::time_point now) {
    const std::string key = watch::pathToUtf8(event.path.lexically_normal());
    if (key.empty()) return;
    switch (event.type) {
        case watch::RawEvent::Type::Rescan:
            if (std::find(rescans.begin(), rescans.end(), key) == rescans.end()) rescans.push_back(key);
            return;
        case watch::RawEvent::Type::Created:
            created(key, event.directory, now);
            return;
        case watch::RawEvent::Type::Modified:
            modified(key, event.directory, now);
            return;
        case watch::RawEvent::Type::Removed:
            removed(key, event.directory, now);
            return;
        case watch::RawEvent::Type::Moved: {
            const std::string from = watch::pathToUtf8(event.oldPath.lexically_normal());
            if (from.empty()) created(key, event.directory, now);
            else moved(from, key, event.directory, now);
            return;
        }
        case watch::RawEvent::Type::Changed: {
            // FSEvents flags are cumulative: what is on disk and in the index decides
            watch::PathStat disk, index;
            const bool exists = env.stat(event.path, disk);
            const bool isKnown = env.known(event.path, index) || entries.count(key) > 0;
            if (exists) {
                // a folder's own change is its entries changing, and those come per file
                if (disk.directory && isKnown) return;
                if (isKnown) modified(key, disk.directory, now);
                else created(key, disk.directory, now);
            } else if (isKnown) {
                removed(key, index.directory, now);
            }
            return;
        }
    }
}

void FileChangeCoalescer::created(const std::string& key, bool directory, Clock::time_point now) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        watch::PathStat index;
        const bool isKnown = env.known(pathOf(key), index);
        Entry& entry = fresh(key, isKnown ? State::Modified : State::Added, now);
        entry.existedBefore = isKnown;
        entry.before = index;
        entry.directory = directory || (isKnown && index.directory);
        return;
    }
    Entry& entry = it->second;
    touch(entry, now);
    entry.directory = entry.directory || directory;
    switch (entry.state) {
        case State::Added:
        case State::Modified:
        case State::Moved:
            return;   // still there with new bytes: the settle decides
        case State::Removed:
            entry.state = entry.existedBefore ? State::Modified : State::Added;
            return;
        case State::MovedFrom: {
            // renamed away, then recreated: this path changed, and the rename's target is a
            // file of its own now (it holds the old bytes)
            const std::string to = entry.link;
            entry.state = entry.existedBefore ? State::Modified : State::Added;
            entry.link.clear();
            auto partner = entries.find(to);
            if (partner != entries.end() && partner->second.state == State::Moved && partner->second.link == key) {
                watch::PathStat index;
                const bool targetKnown = env.known(pathOf(to), index);
                partner->second.state = targetKnown ? State::Modified : State::Added;
                partner->second.link.clear();
                partner->second.existedBefore = targetKnown;
                partner->second.before = index;
                partner->second.modifiedAfterMove = false;
            }
            return;
        }
    }
}

void FileChangeCoalescer::modified(const std::string& key, bool directory, Clock::time_point now) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        // a change to a path never seen: it is new (written before the watch saw it appear)
        created(key, directory, now);
        return;
    }
    Entry& entry = it->second;
    switch (entry.state) {
        case State::Added:
        case State::Modified:
            touch(entry, now);
            return;
        case State::Moved:
            touch(entry, now);
            entry.modifiedAfterMove = true;
            return;
        case State::Removed:
        case State::MovedFrom:
            // written again after it went: back
            created(key, directory, now);
            return;
    }
}

void FileChangeCoalescer::dropMoved(const std::string& key) {
    auto it = entries.find(key);
    if (it == entries.end() || it->second.state != State::Moved) return;
    auto source = entries.find(it->second.link);
    if (source != entries.end() && source->second.state == State::MovedFrom && source->second.link == key) {
        source->second.state = State::Removed;
        source->second.link.clear();
    }
}

void FileChangeCoalescer::removed(const std::string& key, bool directory, Clock::time_point now) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        watch::PathStat index;
        if (!env.known(pathOf(key), index)) return;   // never delivered: nothing to remove
        Entry& entry = fresh(key, State::Removed, now);
        entry.existedBefore = true;
        entry.before = index;
        entry.directory = directory || index.directory;
        return;
    }
    Entry& entry = it->second;
    touch(entry, now);
    switch (entry.state) {
        case State::Added:
            entries.erase(it);   // came and went
            return;
        case State::Modified:
            entry.state = State::Removed;
            return;
        case State::Moved: {
            // what the rename brought left again: its source is gone, and so is what was here
            dropMoved(key);
            if (entry.existedBefore) {
                entry.state = State::Removed;
                entry.link.clear();
                entry.modifiedAfterMove = false;
            } else {
                entries.erase(it);
            }
            return;
        }
        case State::Removed:
        case State::MovedFrom:
            return;
    }
}

void FileChangeCoalescer::rekeyUnder(const std::string& from, const std::string& to) {
    const std::string prefix = from + "/";
    std::vector<std::pair<std::string, Entry>> moved;
    for (auto it = entries.lower_bound(prefix); it != entries.end() && it->first.compare(0, prefix.size(), prefix) == 0;) {
        Entry entry = it->second;
        entry.order = nextOrder++;   // after their folder's Moved
        moved.emplace_back(to + "/" + it->first.substr(prefix.size()), entry);
        it = entries.erase(it);
    }
    for (auto& [key, entry] : moved) entries[key] = entry;
}

void FileChangeCoalescer::moved(const std::string& from, const std::string& to, bool directory, Clock::time_point now) {
    if (from == to) {
        modified(to, directory, now);
        return;
    }

    std::string origin = from;          // where the bytes at `to` were when last delivered
    bool modifiedFlag = false;
    bool originDirectory = directory;
    auto source = entries.find(from);
    if (source == entries.end()) {
        watch::PathStat index;
        if (!env.known(pathOf(from), index)) {
            // from somewhere never delivered (a temporary, a folder outside the watch):
            // something new arrived at `to`
            created(to, directory, now);
            if (directory) rekeyUnder(from, to);
            return;
        }
        Entry& entry = fresh(from, State::MovedFrom, now);
        entry.existedBefore = true;
        entry.before = index;
        entry.directory = directory || index.directory;
        entry.link = to;
        originDirectory = entry.directory;
    } else {
        Entry& entry = source->second;
        switch (entry.state) {
            case State::Added:
                // the temp file of a safe-save renamed over its target: the target changed
                originDirectory = entry.directory || directory;
                entries.erase(source);
                created(to, originDirectory, now);
                if (originDirectory) rekeyUnder(from, to);
                return;
            case State::Removed:
            case State::MovedFrom:
                created(to, directory, now);
                return;
            case State::Modified:
                touch(entry, now);
                entry.state = State::MovedFrom;
                entry.link = to;
                modifiedFlag = true;
                originDirectory = entry.directory || directory;
                break;
            case State::Moved: {
                // Moved(A->B) + Moved(B->C) = Moved(A->C)
                origin = entry.link;
                modifiedFlag = entry.modifiedAfterMove;
                originDirectory = entry.directory || directory;
                const bool existedBefore = entry.existedBefore;
                const watch::PathStat before = entry.before;
                entries.erase(source);
                if (existedBefore) {
                    // `from` had bytes of its own before the first rename landed on it
                    Entry& gone = fresh(from, State::Removed, now);
                    gone.existedBefore = true;
                    gone.before = before;
                    gone.directory = before.directory;
                }
                auto start = entries.find(origin);
                if (origin == to) {
                    // renamed back where it came from
                    if (start != entries.end() && start->second.state == State::MovedFrom) {
                        if (modifiedFlag) {
                            start->second.state = State::Modified;
                            start->second.link.clear();
                            touch(start->second, now);
                        } else {
                            entries.erase(start);
                        }
                    } else {
                        modified(to, originDirectory, now);
                    }
                    if (originDirectory) rekeyUnder(from, to);
                    return;
                }
                if (start != entries.end()) start->second.link = to;
                break;
            }
        }
    }

    // Whatever was at `to` is replaced
    watch::PathStat targetBefore;
    bool targetKnown = false;
    auto target = entries.find(to);
    if (target != entries.end()) {
        Entry& entry = target->second;
        targetKnown = entry.existedBefore;
        targetBefore = entry.before;
        if (entry.state == State::Moved) dropMoved(to);
    } else {
        targetKnown = env.known(pathOf(to), targetBefore);
    }
    Entry& entry = fresh(to, State::Moved, now);
    entry.link = origin;
    entry.existedBefore = targetKnown;
    entry.before = targetBefore;
    entry.directory = originDirectory;
    entry.modifiedAfterMove = modifiedFlag;
    if (originDirectory) rekeyUnder(from, to);
}

void FileChangeCoalescer::pairByIdentity(Clock::time_point now) {
    std::unordered_map<FileIdentity, std::string> removedByIdentity;
    bool anyAdded = false;
    for (const auto& [key, entry] : entries) {
        if (entry.state == State::Removed && entry.before.identity.valid && !isTemporary(key)) removedByIdentity[entry.before.identity] = key;
        if (entry.state == State::Added) anyAdded = true;
    }
    if (removedByIdentity.empty() || !anyAdded) return;

    for (auto& [key, entry] : entries) {
        if (entry.state != State::Added) continue;
        if (!entry.sampled) {
            watch::PathStat disk;
            if (!env.stat(pathOf(key), disk)) continue;
            entry.sampled = true;
            entry.sample = disk;
            entry.sampledAt = now;
        }
        auto it = removedByIdentity.find(entry.sample.identity);
        if (!entry.sample.identity.valid || it == removedByIdentity.end()) continue;
        Entry& gone = entries[it->second];
        if (gone.directory != entry.sample.directory) continue;
        // the polling backend and FSEvents see a rename as a removal plus an addition
        entry.state = State::Moved;
        entry.link = it->second;
        entry.modifiedAfterMove = !gone.before.sameContent(entry.sample);
        entry.directory = entry.sample.directory;
        gone.state = State::MovedFrom;
        gone.link = key;
        removedByIdentity.erase(it);
        if (removedByIdentity.empty()) return;
    }
}

FileChangeCoalescer::Clock::time_point FileChangeCoalescer::readyAt(const Entry& entry) const {
    if (entry.state == State::Removed) return entry.last + std::max(settings.quiet, settings.deleteHold);
    const Clock::time_point quiet = entry.last + settings.quiet;
    if (entry.sampled && !entry.directory) return std::max(quiet, entry.sampledAt + settings.settleGap);
    return quiet;
}

FileChangeCoalescer::Clock::time_point FileChangeCoalescer::nextDeadline() const {
    if (!rescans.empty()) return Clock::time_point::min();
    Clock::time_point next = Clock::time_point::max();
    for (const auto& [key, entry] : entries) {
        if (entry.state == State::MovedFrom) continue;
        next = std::min(next, readyAt(entry));
    }
    return next;
}

FileChange FileChangeCoalescer::makeChange(FileChangeKind kind, const std::string& key, const watch::PathStat& stat, bool directory) const {
    FileChange change;
    change.kind = kind;
    change.path = pathOf(key);
    change.directory = directory;
    change.size = stat.size;
    change.mtimeNs = stat.mtimeNs;
    change.identity = stat.identity;
    return change;
}

std::vector<FileChange> FileChangeCoalescer::take(Clock::time_point now, bool force) {
    std::vector<FileChange> out;
    for (const std::string& key : rescans) {
        FileChange change;
        change.kind = FileChangeKind::Rescan;
        change.path = pathOf(key);
        change.directory = true;
        out.push_back(change);
    }
    rescans.clear();
    if (entries.empty()) return out;

    pairByIdentity(now);

    std::vector<std::pair<uint64_t, std::string>> order;
    order.reserve(entries.size());
    for (const auto& [key, entry] : entries) order.emplace_back(entry.order, key);
    std::sort(order.begin(), order.end());

    for (const auto& item : order) {
        const std::string& key = item.second;
        auto it = entries.find(key);
        if (it == entries.end()) continue;
        Entry& entry = it->second;
        if (entry.state == State::MovedFrom) continue;   // comes out with its partner
        if (!force && now < readyAt(entry)) continue;

        if (entry.state == State::Removed) {
            if (!isTemporary(key)) out.push_back(makeChange(FileChangeKind::Removed, key, entry.before, entry.directory));
            entries.erase(it);
            continue;
        }

        watch::PathStat disk;
        if (!env.stat(pathOf(key), disk)) {
            // gone again before it settled
            if (entry.state == State::Added) {
                entries.erase(it);
            } else if (entry.state == State::Modified) {
                entry.state = State::Removed;
                entry.last = now;
                if (force) {
                    out.push_back(makeChange(FileChangeKind::Removed, key, entry.before, entry.directory));
                    entries.erase(it);
                }
            } else {
                dropMoved(key);
                if (entry.existedBefore) {
                    entry.state = State::Removed;
                    entry.link.clear();
                    entry.last = now;
                } else {
                    entries.erase(it);
                }
                if (force) {
                    // the source's removal and this one, now
                    auto again = entries.find(key);
                    if (again != entries.end()) {
                        out.push_back(makeChange(FileChangeKind::Removed, key, again->second.before, again->second.directory));
                        entries.erase(again);
                    }
                }
            }
            continue;
        }

        if (!force && !entry.directory && !disk.directory && now - entry.first < settings.growCap) {
            // settled: two stats settleGap apart agree
            if (!entry.sampled || !entry.sample.sameContent(disk)) {
                entry.sampled = true;
                entry.sample = disk;
                entry.sampledAt = now;
                continue;
            }
            if (now - entry.sampledAt < settings.settleGap) continue;
        }

        const bool directory = entry.directory || disk.directory;
        switch (entry.state) {
            case State::Added:
                if (!isTemporary(key)) out.push_back(makeChange(FileChangeKind::Added, key, disk, directory));
                break;
            case State::Modified:
                if (!isTemporary(key)) out.push_back(makeChange(FileChangeKind::Modified, key, disk, directory));
                break;
            case State::Moved: {
                const std::string from = entry.link;
                watch::PathStat sourceBefore;
                auto source = entries.find(from);
                if (source != entries.end() && source->second.state == State::MovedFrom && source->second.link == key) {
                    sourceBefore = source->second.before;
                    entries.erase(source);
                }
                const bool temporaryFrom = isTemporary(from), temporaryTo = isTemporary(key);
                if (temporaryTo && !temporaryFrom) {
                    // renamed to a backup name and not replaced: it went
                    out.push_back(makeChange(FileChangeKind::Removed, from, sourceBefore, directory));
                } else if (temporaryFrom && !temporaryTo) {
                    out.push_back(makeChange(entry.existedBefore ? FileChangeKind::Modified : FileChangeKind::Added, key, disk, directory));
                } else if (!temporaryFrom) {
                    FileChange change = makeChange(FileChangeKind::Moved, key, disk, directory);
                    change.oldPath = pathOf(from);
                    out.push_back(change);
                    if (entry.modifiedAfterMove && !directory) out.push_back(makeChange(FileChangeKind::Modified, key, disk, false));
                }
                break;
            }
            case State::Removed:
            case State::MovedFrom:
                break;
        }
        entries.erase(key);
    }

    if (force && !entries.empty()) {
        // what a pass turned into a removal (a source whose rename target vanished) comes
        // out too; a source left without its partner is a removal
        for (auto& [key, entry] : entries) {
            if (entry.state != State::MovedFrom) continue;
            auto partner = entries.find(entry.link);
            if (partner == entries.end() || partner->second.state != State::Moved || partner->second.link != key) {
                entry.state = State::Removed;
                entry.link.clear();
            }
        }
        std::vector<FileChange> rest = take(now, true);
        out.insert(out.end(), rest.begin(), rest.end());
    }
    return out;
}
