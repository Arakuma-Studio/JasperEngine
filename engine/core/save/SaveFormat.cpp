// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "save/SaveFormat.h"

#include "util/ContentHash.h"

#include <cstring>
#include <set>

using namespace jasperengine;

namespace jasperengine::savegame {

namespace {

    const char MAGIC[8] = {'J', 'S', 'A', 'V', '\r', '\n', '\x1A', '\n'};
    const char END_MARKER[8] = {'J', 'S', 'A', 'V', 'E', 'N', 'D', '\n'};

    void saveFormatPutLE(std::string& out, uint64_t v, int bytes){
        for (int i = 0; i < bytes; i++) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }

    uint64_t saveFormatGetLE(std::string_view bytes, size_t offset, int count){
        uint64_t v = 0;
        for (int i = 0; i < count; i++)
            v |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
        return v;
    }

    bool fail(SaveError& error, const std::string& what, size_t offset){
        error = makeError(SaveErrorCode::Corrupt, what + " at offset " + std::to_string(offset));
        return false;
    }

    // Magic, format and the fixed header fields with the metadata; `limit` is where the sections
    // end (the footer's start), or the bytes given for a header-only read
    bool parseHeader(std::string_view bytes, size_t limit, SaveFile& out, size_t& headerSize, SaveError& error){
        if (bytes.size() < 12) return fail(error, "file of " + std::to_string(bytes.size()) + " bytes is too short", 0);
        if (std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
            return fail(error, "bad magic (not a save file, or a copy that changed its line ends)", 0);
        const uint16_t format = static_cast<uint16_t>(saveFormatGetLE(bytes, 8, 2));
        if (format > FORMAT_VERSION){
            error = makeError(SaveErrorCode::NewerFormat, "written in save format " + std::to_string(format) +
                              " by a newer engine; this one reads format " + std::to_string(FORMAT_VERSION));
            return false;
        }
        if (format == 0) return fail(error, "bad format version 0", 8);
        if (bytes.size() < HEADER_FIXED_BYTES) return fail(error, "header cut short", bytes.size());
        const uint16_t flags = static_cast<uint16_t>(saveFormatGetLE(bytes, 10, 2));
        if (flags != 0) return fail(error, "unknown flags " + std::to_string(flags), 10);
        headerSize = static_cast<size_t>(saveFormatGetLE(bytes, 12, 4));
        if (headerSize < HEADER_FIXED_BYTES || headerSize > MAX_HEADER_BYTES)
            return fail(error, "bad header size " + std::to_string(headerSize), 12);
        if (headerSize > limit) return fail(error, "header size " + std::to_string(headerSize) + " beyond the file", 12);
        out.format = format;
        out.generation = saveFormatGetLE(bytes, 16, 8);
        out.savedAtMs = static_cast<int64_t>(saveFormatGetLE(bytes, 24, 8));
        out.dataVersion = static_cast<uint32_t>(saveFormatGetLE(bytes, 32, 4));
        const uint32_t sectionCount = static_cast<uint32_t>(saveFormatGetLE(bytes, 36, 4));
        if (sectionCount > MAX_SECTIONS) return fail(error, "bad section count " + std::to_string(sectionCount), 36);
        const size_t metadataSize = static_cast<size_t>(saveFormatGetLE(bytes, 40, 4));
        if (metadataSize > MAX_METADATA_BYTES || HEADER_FIXED_BYTES + metadataSize != headerSize)
            return fail(error, "bad metadata size " + std::to_string(metadataSize), 40);
        const std::string_view metadata = bytes.substr(HEADER_FIXED_BYTES, metadataSize);
        SaveError valueError;
        if (!validatePayload(metadata, valueError))
            return fail(error, "bad metadata (" + valueError.message + ")", HEADER_FIXED_BYTES);
        out.metadata.assign(metadata.data(), metadata.size());
        out.sections.clear();
        out.sections.resize(sectionCount);
        return true;
    }

}

std::string buildMetadata(const SaveMetadata& metadata){
    // the keys in byte order: engine, meta, platform, requires
    const std::pair<const char*, const std::string*> fields[] = {
        {"engine", &metadata.engine}, {"meta", &metadata.meta}, {"platform", &metadata.platform}, {"requires", &metadata.requirements}};
    uint64_t count = 0;
    for (const auto& field : fields) if (!field.second->empty()) count++;
    std::string out;
    beginPairTable(out, count);
    for (const auto& field : fields){
        if (field.second->empty()) continue;
        appendString(out, field.first);
        out += *field.second;
    }
    return out;
}

bool splitMetadata(std::string_view bytes, SaveMetadata& out){
    out = SaveMetadata();
    SaveError error;
    if (!validatePayload(bytes, error) || bytes.empty() || bytes[0] != 0x05) return false;
    std::string_view value;
    if (findField(bytes, "meta", value)) out.meta.assign(value.data(), value.size());
    if (findField(bytes, "requires", value)) out.requirements.assign(value.data(), value.size());
    if (findField(bytes, "engine", value)) out.engine.assign(value.data(), value.size());
    if (findField(bytes, "platform", value)) out.platform.assign(value.data(), value.size());
    return true;
}

std::string buildSaveFile(const SaveFile& file){
    size_t total = HEADER_FIXED_BYTES + file.metadata.size() + FOOTER_BYTES;
    for (const SaveSection& section : file.sections)
        total += 6 + section.name.size() + (section.payload ? section.payload->size() : 0);
    std::string out;
    out.reserve(total);
    out.append(MAGIC, sizeof(MAGIC));
    saveFormatPutLE(out, file.format, 2);
    saveFormatPutLE(out, 0, 2);
    saveFormatPutLE(out, HEADER_FIXED_BYTES + file.metadata.size(), 4);
    saveFormatPutLE(out, file.generation, 8);
    saveFormatPutLE(out, static_cast<uint64_t>(file.savedAtMs), 8);
    saveFormatPutLE(out, file.dataVersion, 4);
    saveFormatPutLE(out, file.sections.size(), 4);
    saveFormatPutLE(out, file.metadata.size(), 4);
    out += file.metadata;
    for (const SaveSection& section : file.sections){
        saveFormatPutLE(out, section.name.size(), 2);
        out += section.name;
        const size_t size = section.payload ? section.payload->size() : 0;
        saveFormatPutLE(out, size, 4);
        if (section.payload) out += *section.payload;
    }
    saveFormatPutLE(out, hashContent64(out.data(), out.size(), 0), 8);
    out.append(END_MARKER, sizeof(END_MARKER));
    return out;
}

bool parseSaveHeader(std::string_view bytes, SaveFile& out, SaveError& error){
    size_t headerSize = 0;
    return parseHeader(bytes, bytes.size(), out, headerSize, error);
}

bool parseSaveFile(std::string_view bytes, SaveFile& out, SaveError& error){
    if (bytes.size() > MAX_FILE_BYTES)
        return fail(error, "file of " + std::to_string(bytes.size()) + " bytes is above the 64 MiB cap", 0);
    // the footer first (R18), also for a format above this engine's: every format keeps it, so a
    // format field damaged in a format-1 file reads as damage, not as a newer engine's file
    if (bytes.size() < HEADER_FIXED_BYTES + FOOTER_BYTES)
        return fail(error, "file of " + std::to_string(bytes.size()) + " bytes is too short", 0);
    if (std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
        return fail(error, "bad magic (not a save file, or a copy that changed its line ends)", 0);
    const size_t body = bytes.size() - FOOTER_BYTES;
    if (std::memcmp(bytes.data() + body + 8, END_MARKER, sizeof(END_MARKER)) != 0)
        return fail(error, "no end marker (the file is cut short)", body + 8);
    if (saveFormatGetLE(bytes, body, 8) != hashContent64(bytes.data(), body, 0)){
        error = makeError(SaveErrorCode::Corrupt, "checksum mismatch");
        return false;
    }
    size_t pos = 0;
    if (!parseHeader(bytes, body, out, pos, error)) return false;
    std::set<std::string_view> names;
    for (SaveSection& section : out.sections){
        if (body - pos < 2) return fail(error, "section cut short", pos);
        const size_t nameLength = static_cast<size_t>(saveFormatGetLE(bytes, pos, 2));
        if (nameLength == 0 || nameLength > MAX_SECTION_NAME || nameLength > body - pos - 2)
            return fail(error, "bad section name length " + std::to_string(nameLength), pos);
        const std::string_view name = bytes.substr(pos + 2, nameLength);
        if (!isValidName(name, MAX_SECTION_NAME)) return fail(error, "bad section name", pos + 2);
        if (!names.insert(name).second) return fail(error, "section \"" + std::string(name) + "\" given twice", pos + 2);
        pos += 2 + nameLength;
        if (body - pos < 4) return fail(error, "section cut short", pos);
        const uint64_t payloadLength = saveFormatGetLE(bytes, pos, 4);
        if (payloadLength > body - pos - 4) return fail(error, "bad section length " + std::to_string(payloadLength), pos);
        const std::string_view payload = bytes.substr(pos + 4, static_cast<size_t>(payloadLength));
        SaveError valueError;
        if (!validatePayload(payload, valueError))
            return fail(error, "bad section \"" + std::string(name) + "\" (" + valueError.message + ")", pos + 4);
        section.name.assign(name.data(), name.size());
        section.payload = std::make_shared<const std::string>(payload);
        pos += 4 + static_cast<size_t>(payloadLength);
    }
    if (pos != body) return fail(error, std::to_string(body - pos) + " bytes after the sections", pos);
    return true;
}

bool isValidName(std::string_view name, size_t max){
    if (name.empty() || name.size() > max) return false;
    for (char c : name){
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

std::string suggestName(std::string_view name, size_t max){
    std::string out;
    for (char c : name){
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(ok ? c : '-');
        if (out.size() >= max) break;
    }
    if (out.empty()) out = "slot1";
    return out;
}

}  // namespace jasperengine::savegame
