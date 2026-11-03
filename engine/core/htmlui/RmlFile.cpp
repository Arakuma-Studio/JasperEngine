// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlFile.h"

#include "HtmlUIDependencies.h"
#include "compat/VirtualFiles.h"
#include "debug/DebugOverlayResources.h"
#include "io/Data.h"

#include <cstdio>
#include <cstring>

using namespace jasperengine;

namespace {

    // The read position is kept here: Data::seek clamps to length - 1, so it cannot
    // stand at the end of the file the way fseek can
    struct RmlFileHandle {
        Data data;
        size_t position = 0;
    };

    RmlFileHandle* toHandle(Rml::FileHandle file){
        return reinterpret_cast<RmlFileHandle*>(file);
    }

}

Rml::FileHandle RmlFile::Open(const Rml::String& path){
    // an empty path would resolve to the asset directory itself
    if (path.empty())
        return 0;

    // the compat layer's in-memory files (phase-2 hook H3b): rewritten sheets, the
    // user-agent sheet and its SVGs, data: URIs
    if (htmlui::compat::VirtualFiles::isVirtual(path)){
        std::shared_ptr<const std::string> content = htmlui::compat::VirtualFiles::find(path);
        if (!content)
            return 0;
        RmlFileHandle* handle = new RmlFileHandle();
        handle->data.open(reinterpret_cast<unsigned char*>(const_cast<char*>(content->data())), (unsigned int)content->size(), true, true);
        return reinterpret_cast<Rml::FileHandle>(handle);
    }

    // the engine's embedded files (engine://debug/overlay.rml): never the project's
    if (EngineResources::isEnginePath(path)){
        const char* data = nullptr;
        size_t size = 0;
        if (!EngineResources::find(path, data, size))
            return 0;
        RmlFileHandle* handle = new RmlFileHandle();
        handle->data.open(reinterpret_cast<unsigned char*>(const_cast<char*>(data)), (unsigned int)size, true, true);
        return reinterpret_cast<Rml::FileHandle>(handle);
    }

    // hot reload: the files a document load reads (UIDocumentSystem's load scope), missing ones too
    HtmlUIFileLog::recordFile(path);

    RmlFileHandle* handle = new RmlFileHandle();
    if (handle->data.open(path.c_str()) != FileErrors::FILEDATA_OK){
        delete handle;
        return 0;
    }

    return reinterpret_cast<Rml::FileHandle>(handle);
}

void RmlFile::Close(Rml::FileHandle file){
    delete toHandle(file);
}

size_t RmlFile::Read(void* buffer, size_t size, Rml::FileHandle file){
    RmlFileHandle* handle = toHandle(file);
    if (!handle || !buffer)
        return 0;

    const size_t length = handle->data.length();
    if (handle->position >= length)
        return 0;

    const size_t count = (size < length - handle->position) ? size : length - handle->position;
    std::memcpy(buffer, handle->data.getMemPtr() + handle->position, count);
    handle->position += count;

    return count;
}

bool RmlFile::Seek(Rml::FileHandle file, long offset, int origin){
    RmlFileHandle* handle = toHandle(file);
    if (!handle)
        return false;

    const long long length = (long long)handle->data.length();
    long long target;
    switch (origin){
        case SEEK_SET:
            target = offset;
            break;
        case SEEK_CUR:
            target = (long long)handle->position + offset;
            break;
        case SEEK_END:
            target = length + offset;
            break;
        default:
            return false;
    }

    if (target < 0 || target > length)
        return false;

    handle->position = (size_t)target;
    return true;
}

size_t RmlFile::Tell(Rml::FileHandle file){
    RmlFileHandle* handle = toHandle(file);
    return handle ? handle->position : 0;
}

size_t RmlFile::Length(Rml::FileHandle file){
    RmlFileHandle* handle = toHandle(file);
    return handle ? handle->data.length() : 0;
}

bool RmlFile::LoadFile(const Rml::String& path, Rml::String& out_data){
    if (path.empty())
        return false;

    if (htmlui::compat::VirtualFiles::isVirtual(path)){
        std::shared_ptr<const std::string> content = htmlui::compat::VirtualFiles::find(path);
        if (!content)
            return false;
        out_data = *content;
        return true;
    }

    if (EngineResources::isEnginePath(path)){
        const char* data = nullptr;
        size_t size = 0;
        if (!EngineResources::find(path, data, size))
            return false;
        out_data.assign(data, size);
        return true;
    }

    HtmlUIFileLog::recordFile(path);

    Data data;
    if (data.open(path.c_str()) != FileErrors::FILEDATA_OK)
        return false;

    const unsigned int length = data.length();
    if (length > 0)
        out_data.assign(reinterpret_cast<const char*>(data.getMemPtr()), length);
    else
        out_data.clear();

    return true;
}
