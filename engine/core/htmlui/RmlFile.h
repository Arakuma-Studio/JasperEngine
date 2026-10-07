// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLFILE_H
#define RMLFILE_H

#include "Export.h"

#include <RmlUi/Core/FileInterface.h>

namespace jasperengine {

    // RmlUi's file interface over the engine's Data: ResourcePack, asset://, data://,
    // project-relative paths, Android assets and the Web preloaded FS, with no platform
    // code here. A file is read whole on Open.
    class JASPERENGINE_INTERNAL RmlFile : public Rml::FileInterface {
    public:
        Rml::FileHandle Open(const Rml::String& path) override;  // 0 when Data::open fails
        void Close(Rml::FileHandle file) override;
        size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
        bool Seek(Rml::FileHandle file, long offset, int origin) override;   // SEEK_SET/CUR/END, false if out of range
        size_t Tell(Rml::FileHandle file) override;
        size_t Length(Rml::FileHandle file) override;
        bool LoadFile(const Rml::String& path, Rml::String& out_data) override;  // one Data::open + copy
    };

}

#endif //RMLFILE_H
