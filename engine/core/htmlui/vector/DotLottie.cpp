// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DotLottie.h"

#include "stb_image.h"
#include "json.hpp"

#include <algorithm>
#include <cstring>

using namespace jasperengine;

namespace {

    const uint32_t localHeaderSignature = 0x04034b50;
    const uint32_t centralHeaderSignature = 0x02014b50;
    const uint32_t endOfCentralDirectorySignature = 0x06054b50;
    const uint32_t zip64LocatorSignature = 0x07064b50;
    const size_t endOfCentralDirectorySize = 22;
    const size_t centralHeaderSize = 46;
    const size_t localHeaderSize = 30;

    uint16_t read16(const uint8_t* p){
        return (uint16_t)(p[0] | (p[1] << 8));
    }

    uint32_t read32(const uint8_t* p){
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    uint32_t crc32(const uint8_t* data, size_t size){
        static uint32_t table[256];
        static bool tableReady = false;
        if (!tableReady){
            for (uint32_t i = 0; i < 256; i++){
                uint32_t c = i;
                for (int k = 0; k < 8; k++)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            tableReady = true;
        }
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; i++)
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    std::string normalizePath(std::string path){
        std::replace(path.begin(), path.end(), '\\', '/');
        // collapse "//" (Lottie builds "<dir>/<u><p>") and drop "./" and leading "/"
        std::string out;
        out.reserve(path.size());
        for (char c : path){
            if (c == '/' && !out.empty() && out.back() == '/')
                continue;
            out.push_back(c);
        }
        while (true){
            if (out.compare(0, 2, "./") == 0){
                out.erase(0, 2);
            }else if (!out.empty() && out[0] == '/'){
                out.erase(0, 1);
            }else{
                break;
            }
        }
        return out;
    }

    void readSettings(const nlohmann::json& object, DotLottie::Settings& settings){
        if (!object.is_object())
            return;
        auto it = object.find("autoplay");
        if (it != object.end() && it->is_boolean())
            settings.autoplay = it->get<bool>();
        it = object.find("loop");
        if (it != object.end()){
            if (it->is_boolean()){
                settings.loop = it->get<bool>();
            }else if (it->is_number_integer() || it->is_number_unsigned()){
                settings.loop = true;
                settings.loopCount = it->get<int>();
            }
        }
        it = object.find("speed");
        if (it != object.end() && it->is_number())
            settings.speed = it->get<float>();
        it = object.find("direction");
        if (it != object.end() && it->is_number())
            settings.direction = it->get<int>();
        it = object.find("playMode");
        if (it != object.end() && it->is_string())
            settings.playMode = it->get<std::string>();
    }

}

// ---------------------------------------------------------------- ZipArchive

bool ZipArchive::open(const uint8_t* data, size_t size, std::string& error){
    bytes.clear();
    entries.clear();

    if (!data || size < endOfCentralDirectorySize){
        error = "not a ZIP archive (too small)";
        return false;
    }

    // the end-of-central-directory record: the last one within the comment's reach
    size_t eocd = SIZE_MAX;
    const size_t lowest = size > endOfCentralDirectorySize + 0xFFFF ? size - endOfCentralDirectorySize - 0xFFFF : 0;
    for (size_t pos = size - endOfCentralDirectorySize + 1; pos-- > lowest;){
        if (read32(data + pos) == endOfCentralDirectorySignature){
            eocd = pos;
            break;
        }
    }
    if (eocd == SIZE_MAX){
        error = "not a ZIP archive (no end of central directory)";
        return false;
    }

    if (eocd >= 20 && read32(data + eocd - 20) == zip64LocatorSignature){
        error = "ZIP64 archives are not supported";
        return false;
    }

    const uint16_t diskNumber = read16(data + eocd + 4);
    const uint16_t directoryDisk = read16(data + eocd + 6);
    const uint16_t diskEntries = read16(data + eocd + 8);
    const uint16_t totalEntries = read16(data + eocd + 10);
    const uint32_t directorySize = read32(data + eocd + 12);
    const uint32_t directoryOffset = read32(data + eocd + 16);

    if (totalEntries == 0xFFFF || directorySize == 0xFFFFFFFFu || directoryOffset == 0xFFFFFFFFu){
        error = "ZIP64 archives are not supported";
        return false;
    }
    if (diskNumber != 0 || directoryDisk != 0 || diskEntries != totalEntries){
        error = "multi-disk ZIP archives are not supported";
        return false;
    }
    if ((size_t)directoryOffset + directorySize > eocd){
        error = "corrupt ZIP archive (central directory out of range)";
        return false;
    }

    size_t pos = directoryOffset;
    for (uint16_t i = 0; i < totalEntries; i++){
        if (pos + centralHeaderSize > eocd || read32(data + pos) != centralHeaderSignature){
            error = "corrupt ZIP archive (central directory entry)";
            return false;
        }
        const uint16_t flags = read16(data + pos + 8);
        Entry entry;
        entry.method = read16(data + pos + 10);
        entry.crc = read32(data + pos + 16);
        entry.compressedSize = read32(data + pos + 20);
        entry.uncompressedSize = read32(data + pos + 24);
        const uint16_t nameLength = read16(data + pos + 28);
        const uint16_t extraLength = read16(data + pos + 30);
        const uint16_t commentLength = read16(data + pos + 32);
        entry.localHeaderOffset = read32(data + pos + 42);

        if (pos + centralHeaderSize + nameLength > eocd){
            error = "corrupt ZIP archive (entry name)";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(data + pos + centralHeaderSize), nameLength);

        if (flags & 0x0001){
            error = "encrypted ZIP archives are not supported ('" + name + "')";
            return false;
        }
        if (entry.compressedSize == 0xFFFFFFFFu || entry.uncompressedSize == 0xFFFFFFFFu || entry.localHeaderOffset == 0xFFFFFFFFu){
            error = "ZIP64 archives are not supported ('" + name + "')";
            return false;
        }

        if (!name.empty() && name.back() != '/')
            entries[normalizePath(name)] = entry;

        pos += centralHeaderSize + nameLength + extraLength + commentLength;
    }

    bytes.assign(data, data + size);
    return true;
}

bool ZipArchive::has(const std::string& name) const{
    return entries.count(normalizePath(name)) != 0;
}

std::vector<std::string> ZipArchive::names() const{
    std::vector<std::string> result;
    for (const auto& entry : entries)
        result.push_back(entry.first);
    return result;
}

bool ZipArchive::read(const std::string& name, std::string& out, std::string& error) const{
    auto it = entries.find(normalizePath(name));
    if (it == entries.end()){
        error = "'" + name + "' is not in the archive";
        return false;
    }
    const Entry& entry = it->second;

    const size_t local = entry.localHeaderOffset;
    if (local + localHeaderSize > bytes.size() || read32(bytes.data() + local) != localHeaderSignature){
        error = "corrupt ZIP archive (local header of '" + name + "')";
        return false;
    }
    const size_t dataOffset = local + localHeaderSize + read16(bytes.data() + local + 26) + read16(bytes.data() + local + 28);
    if (dataOffset + entry.compressedSize > bytes.size()){
        error = "corrupt ZIP archive (data of '" + name + "' out of range)";
        return false;
    }
    const uint8_t* compressed = bytes.data() + dataOffset;

    if (entry.method == 0){
        if (entry.compressedSize != entry.uncompressedSize){
            error = "corrupt ZIP archive (stored entry '" + name + "' sizes differ)";
            return false;
        }
        out.assign(reinterpret_cast<const char*>(compressed), entry.compressedSize);
    }else if (entry.method == 8){
        // one spare byte: stb needs a non-empty buffer for empty entries
        out.resize((size_t)entry.uncompressedSize + 1);
        const int written = stbi_zlib_decode_noheader_buffer(&out[0], (int)entry.uncompressedSize + 1,
                                                             reinterpret_cast<const char*>(compressed), (int)entry.compressedSize);
        if (written < 0 || (uint32_t)written != entry.uncompressedSize){
            error = "corrupt ZIP archive (could not inflate '" + name + "')";
            return false;
        }
        out.resize(entry.uncompressedSize);
    }else{
        error = "ZIP compression method " + std::to_string(entry.method) + " of '" + name + "' is not supported (stored and deflate only)";
        return false;
    }

    if (crc32(reinterpret_cast<const uint8_t*>(out.data()), out.size()) != entry.crc){
        error = "corrupt ZIP archive (CRC mismatch in '" + name + "')";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- DotLottie

bool DotLottie::looksLikeZip(const uint8_t* data, size_t size){
    return data && size >= 4 && read32(data) == localHeaderSignature;
}

bool DotLottie::open(const uint8_t* data, size_t size, std::string& error){
    animations.clear();
    topLevel = Settings();
    activeAnimationId.clear();
    version = 1;

    if (!archive.open(data, size, error))
        return false;

    std::string manifestText;
    if (!archive.read("manifest.json", manifestText, error)){
        error = "no manifest.json in the .lottie archive (" + error + ")";
        return false;
    }

    const nlohmann::json manifest = nlohmann::json::parse(manifestText, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()){
        error = "manifest.json of the .lottie archive is not a JSON object";
        return false;
    }

    auto versionIt = manifest.find("version");
    if (versionIt != manifest.end()){
        std::string text = versionIt->is_string() ? versionIt->get<std::string>() :
                           versionIt->is_number() ? std::to_string(versionIt->get<int>()) : "";
        if (!text.empty() && text[0] == '2')
            version = 2;
    }

    readSettings(manifest, topLevel);

    auto list = manifest.find("animations");
    if (list == manifest.end() || !list->is_array() || list->empty()){
        error = "manifest.json of the .lottie archive lists no animations";
        return false;
    }
    for (const nlohmann::json& item : *list){
        if (!item.is_object())
            continue;
        auto id = item.find("id");
        if (id == item.end() || !id->is_string())
            continue;
        Animation animation;
        animation.id = id->get<std::string>();
        readSettings(item, animation.settings);
        animations.push_back(std::move(animation));
    }
    if (animations.empty()){
        error = "manifest.json of the .lottie archive has no animation with an id";
        return false;
    }

    auto active = manifest.find("activeAnimationId");
    if (active != manifest.end() && active->is_string()){
        activeAnimationId = active->get<std::string>();
    }
    auto initial = manifest.find("initial");
    if (activeAnimationId.empty() && initial != manifest.end() && initial->is_object()){
        auto animation = initial->find("animation");
        if (animation != initial->end() && animation->is_string())
            activeAnimationId = animation->get<std::string>();
    }

    // dotLottie v2 moved the folders; some v1 tools already wrote a/ and i/
    if (version == 1 && !archive.has("animations/" + animations[0].id + ".json") && archive.has("a/" + animations[0].id + ".json"))
        version = 2;

    return true;
}

int DotLottie::getVersion() const{
    return version;
}

const std::vector<DotLottie::Animation>& DotLottie::getAnimations() const{
    return animations;
}

std::string DotLottie::getDefaultAnimationId() const{
    if (!activeAnimationId.empty() && findAnimation(activeAnimationId))
        return activeAnimationId;
    return animations.empty() ? std::string() : animations[0].id;
}

const DotLottie::Animation* DotLottie::findAnimation(const std::string& id) const{
    for (const Animation& animation : animations){
        if (animation.id == id)
            return &animation;
    }
    return nullptr;
}

DotLottie::Settings DotLottie::getSettings(const std::string& id) const{
    Settings result = topLevel;
    if (const Animation* animation = findAnimation(id)){
        const Settings& own = animation->settings;
        if (own.autoplay) result.autoplay = own.autoplay;
        if (own.loop){
            result.loop = own.loop;
            result.loopCount = own.loopCount;
        }
        if (own.speed) result.speed = own.speed;
        if (own.direction) result.direction = own.direction;
        if (own.playMode) result.playMode = own.playMode;
    }
    return result;
}

bool DotLottie::readAnimation(const std::string& id, std::string& json, std::string& error) const{
    const std::string primary = (version == 2 ? "a/" : "animations/") + id + ".json";
    const std::string secondary = (version == 2 ? "animations/" : "a/") + id + ".json";
    if (archive.has(primary))
        return archive.read(primary, json, error);
    if (archive.has(secondary))
        return archive.read(secondary, json, error);
    error = "animation '" + id + "' is not in the .lottie archive (" + primary + ")";
    return false;
}

bool DotLottie::readImage(const std::string& path, std::string& data) const{
    std::string error;
    const std::string normalized = normalizePath(path);
    if (archive.has(normalized))
        return archive.read(normalized, data, error);

    const size_t slash = normalized.find_last_of('/');
    const std::string file = slash == std::string::npos ? normalized : normalized.substr(slash + 1);
    for (const char* folder : {"images/", "i/"}){
        const std::string candidate = folder + file;
        if (archive.has(candidate))
            return archive.read(candidate, data, error);
    }
    return false;
}
