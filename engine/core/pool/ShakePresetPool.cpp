// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShakePresetPool.h"

#include "io/Data.h"
#include "io/FileData.h"

#include <filesystem>
#include <map>

using namespace jasperengine;

namespace{

    struct Stamp{
        bool known = false;
        uintmax_t size = 0;
        std::filesystem::file_time_type time;

        bool operator==(const Stamp& other) const{
            return known == other.known && (!known || (size == other.size && time == other.time));
        }
    };

    struct Entry{
        std::shared_ptr<const camera::ShakePresetFile> file;
        Stamp stamp;
    };

    // never destroyed: shakers may release presets during static destruction
    std::map<std::string, Entry>& entries(){
        static std::map<std::string, Entry>* map = new std::map<std::string, Entry>();
        return *map;
    }

    Stamp stampOf(const std::string& path){
        Stamp stamp;
        std::error_code ec;
        const std::filesystem::path file = std::filesystem::path(FileData::getSystemPath(path));
        if (!std::filesystem::is_regular_file(file, ec)) return stamp;
        stamp.size = std::filesystem::file_size(file, ec);
        if (ec) return Stamp();
        stamp.time = std::filesystem::last_write_time(file, ec);
        if (ec) return Stamp();
        stamp.known = true;
        return stamp;
    }

}

std::string ShakePresetPool::getKey(const std::string& path){
    std::string text = path;
    if (text.compare(0, 8, "asset://") == 0) text = text.substr(8);
    std::string key = std::filesystem::path(text).lexically_normal().generic_string();
    while (key.size() > 2 && key.compare(0, 2, "./") == 0) key = key.substr(2);
    return key;
}

std::shared_ptr<const camera::ShakePresetFile> ShakePresetPool::get(const std::string& path, std::string* error){
    if (path.empty()){
        if (error) *error = "no path";
        return nullptr;
    }
    const std::string key = getKey(path);
    const Stamp stamp = stampOf(key);
    auto it = entries().find(key);
    if (it != entries().end() && it->second.stamp == stamp){
        return it->second.file;
    }

    Data data;
    if (data.open(key.c_str()) != FileErrors::FILEDATA_OK){
        entries().erase(key);
        if (error) *error = "cannot read " + key;
        return nullptr;
    }
    std::string text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
    camera::ShakeParseResult parsed = camera::parseShakePreset(text);
    if (!parsed.ok()){
        entries().erase(key);
        if (error) *error = key + ": " + parsed.errorText();
        return nullptr;
    }
    auto file = std::make_shared<const camera::ShakePresetFile>(std::move(*parsed.file));
    entries()[key] = Entry{file, stamp};
    return file;
}

void ShakePresetPool::invalidate(const std::string& path){
    entries().erase(getKey(path));
}

void ShakePresetPool::clear(){
    entries().clear();
}

size_t ShakePresetPool::size(){
    return entries().size();
}
