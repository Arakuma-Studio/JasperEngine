// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "log/LogSinks.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;

MemorySink::MemorySink(size_t maxBytes, size_t maxEntries) {
    arena.resize(std::max<size_t>(maxBytes, 4096));
    slots.resize(std::max<size_t>(maxEntries, 1));
}

void MemorySink::dropOldest() {
    const Slot& oldest = slots[first];
    const size_t bytes = oldest.skipped + oldest.textLength + oldest.fileLength + oldest.functionLength + oldest.threadNameLength;
    arenaUsed -= bytes;
    arenaHead = arenaUsed == 0 ? 0 : (oldest.offset + bytes - oldest.skipped) % arena.size();
    first = (first + 1) % slots.size();
    count--;
    if (count == 0) {
        arenaHead = arenaTail = arenaUsed = 0;
    }
}

void MemorySink::write(const LogRecord& record) {
    std::lock_guard<std::mutex> lock(mutex);
    std::string_view text = record.text;
    const size_t capacity = arena.size();
    const size_t extra = record.file.size() + record.function.size() + record.threadName.size();
    // a text bigger than the whole arena keeps its start
    if (text.size() + extra > capacity) {
        text = text.substr(0, capacity - extra);
    }
    const size_t bytes = text.size() + extra;

    if (count == slots.size()) dropOldest();
    // contiguous room at the tail, else at the start (the end left unused)
    size_t offset = 0;
    size_t skipped = 0;
    while (true) {
        if (count == 0) {
            arenaHead = arenaTail = arenaUsed = 0;
        }
        if (arenaTail >= arenaHead && arenaUsed < capacity) {
            if (capacity - arenaTail >= bytes) {
                offset = arenaTail;
                skipped = 0;
                break;
            }
            if (arenaHead >= bytes) {
                offset = 0;
                skipped = capacity - arenaTail;
                break;
            }
        } else if (arenaTail < arenaHead && arenaHead - arenaTail >= bytes) {
            offset = arenaTail;
            skipped = 0;
            break;
        }
        dropOldest();
    }

    char* at = arena.data() + offset;
    std::memcpy(at, text.data(), text.size());
    at += text.size();
    if (!record.file.empty()) std::memcpy(at, record.file.data(), record.file.size());
    at += record.file.size();
    if (!record.function.empty()) std::memcpy(at, record.function.data(), record.function.size());
    at += record.function.size();
    if (!record.threadName.empty()) std::memcpy(at, record.threadName.data(), record.threadName.size());

    Slot& slot = slots[(first + count) % slots.size()];
    slot.id = record.id;
    slot.wallTime = record.wallTime;
    slot.frame = record.frame;
    slot.offset = offset;
    slot.skipped = skipped;
    slot.textLength = static_cast<uint32_t>(text.size());
    slot.line = record.line;
    slot.flags = record.flags;
    slot.fileLength = static_cast<uint16_t>(record.file.size());
    slot.functionLength = static_cast<uint16_t>(record.function.size());
    slot.threadNameLength = static_cast<uint8_t>(record.threadName.size());
    slot.level = static_cast<uint8_t>(record.level);
    slot.category = record.category->index;
    count++;
    arenaUsed += skipped + bytes;
    arenaTail = (offset + bytes) % capacity;
}

MemorySink::Entry MemorySink::toEntry(const Slot& slot) const {
    Entry entry;
    entry.id = slot.id;
    entry.level = static_cast<LogLevel>(slot.level);
    entry.category = &Logger::categoryAt(slot.category);
    entry.flags = slot.flags;
    entry.wallTime = slot.wallTime;
    entry.frame = slot.frame;
    entry.line = slot.line;
    const char* at = arena.data() + slot.offset;
    entry.text.assign(at, slot.textLength);
    at += slot.textLength;
    entry.file.assign(at, slot.fileLength);
    at += slot.fileLength;
    entry.function.assign(at, slot.functionLength);
    at += slot.functionLength;
    entry.threadName.assign(at, slot.threadNameLength);
    return entry;
}

std::vector<MemorySink::Entry> MemorySink::since(uint64_t afterId, size_t max, const std::function<bool(const View&)>& filter) const {
    std::vector<Entry> result;
    std::lock_guard<std::mutex> lock(mutex);
    // ids increase along the ring: the first one after the cursor
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (slots[(first + middle) % slots.size()].id <= afterId) low = middle + 1;
        else high = middle;
    }
    for (size_t i = low; i < count; i++) {
        const Slot& slot = slots[(first + i) % slots.size()];
        if (filter) {
            const char* at = arena.data() + slot.offset;
            const View view{slot.id, static_cast<LogLevel>(slot.level), &Logger::categoryAt(slot.category), slot.flags,
                            std::string_view(at + slot.textLength, slot.fileLength), std::string_view(at, slot.textLength)};
            if (!filter(view)) continue;
        }
        result.push_back(toEntry(slot));
        if (max > 0 && result.size() >= max) break;
    }
    return result;
}

uint64_t MemorySink::lastId() const {
    std::lock_guard<std::mutex> lock(mutex);
    return count ? slots[(first + count - 1) % slots.size()].id : 0;
}

size_t MemorySink::size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return count;
}

void MemorySink::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    first = 0;
    count = 0;
    arenaHead = arenaTail = arenaUsed = 0;
}
