// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VirtualFiles.h"

#include "io/Data.h"
#include "io/FileData.h"
#include "io/ResourcePack.h"
#include "System.h"

#include <cstdio>

#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Entry {
        std::shared_ptr<const std::string> content;
        uint64_t owner = 0;
    };

    struct Store {
        std::mutex mutex;
        std::map<std::string, Entry, std::less<>> files;
    };

    Store& store(){
        static Store instance;
        return instance;
    }

}

bool VirtualFiles::isVirtual(std::string_view path){
    if (!path.empty() && path[0] == '/')
        path.remove_prefix(1);
    return path.size() >= 8 && path.substr(0, 8) == kPrefix;
}

std::string VirtualFiles::normalize(std::string_view path){
    while (!path.empty() && path[0] == '/')
        path.remove_prefix(1);
    return std::string(path);
}

void VirtualFiles::add(std::string_view path, std::string content, uint64_t owner){
    add(path, std::make_shared<const std::string>(std::move(content)), owner);
}

void VirtualFiles::add(std::string_view path, std::shared_ptr<const std::string> content, uint64_t owner){
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    Entry& entry = s.files[normalize(path)];
    entry.content = std::move(content);
    entry.owner = owner;
}

bool VirtualFiles::remove(std::string_view path){
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.files.erase(normalize(path)) > 0;
}

size_t VirtualFiles::removeOwner(uint64_t owner){
    if (owner == 0)
        return 0;
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    size_t removed = 0;
    for (auto it = s.files.begin(); it != s.files.end();){
        if (it->second.owner == owner){
            it = s.files.erase(it);
            removed++;
        }else{
            ++it;
        }
    }
    return removed;
}

std::shared_ptr<const std::string> VirtualFiles::find(std::string_view path){
    if (!isVirtual(path))
        return nullptr;
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto it = s.files.find(normalize(path));
    return it != s.files.end() ? it->second.content : nullptr;
}

bool VirtualFiles::exists(std::string_view path){
    return find(path) != nullptr;
}

std::vector<std::string> VirtualFiles::list(std::string_view prefix){
    const std::string normalized = normalize(prefix);
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<std::string> result;
    for (const auto& file : s.files){
        if (file.first.compare(0, normalized.size(), normalized) == 0)
            result.push_back(file.first);
    }
    return result;
}

size_t VirtualFiles::count(){
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.files.size();
}

void VirtualFiles::clear(){
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.files.clear();
}

bool jasperengine::htmlui::compat::readAssetFile(const FileAccess& access, const std::string& path, std::string& content){
    if (VirtualFiles::isVirtual(path)){
        std::shared_ptr<const std::string> file = VirtualFiles::find(path);
        if (!file)
            return false;
        content = *file;
        return true;
    }
    if (access.read)
        return access.read(path, content);

    if (path.empty())
        return false;
    Data data;
    if (data.open(path.c_str()) != FileErrors::FILEDATA_OK)
        return false;
    const unsigned int length = data.length();
    if (length > 0)
        content.assign(reinterpret_cast<const char*>(data.getMemPtr()), length);
    else
        content.clear();
    return true;
}

bool jasperengine::htmlui::compat::assetFileExists(const FileAccess& access, const std::string& path){
    if (VirtualFiles::isVirtual(path))
        return VirtualFiles::exists(path);
    if (access.exists)
        return access.exists(path);
    if (access.read){
        std::string ignored;
        return access.read(path, ignored);
    }
    if (path.empty())
        return false;
    // without reading the file (images can be large): the resource pack's index, else an open
    // through the platform (Android assets and the Web preload included)
    if (ResourcePack::contains(path))
        return true;
    FILE* file = System::instance().platformFopen(FileData::getSystemPath(path).c_str(), "rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}
