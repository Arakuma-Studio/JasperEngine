// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Streaming audio from the engine's files (audio-mixer spec A8, 11): SoLoud reads through the
// engine File (asset paths, Android APK assets through AAssetManager, UTF-8 paths on Windows).
// Engine-internal: the public headers do not include SoLoud.
#ifndef SoloudEngineFile_h
#define SoloudEngineFile_h

#include "io/File.h"

#include "soloud.h"
#include "soloud_file.h"
#include "soloud_wavstream.h"

#include <string>

namespace jasperengine {

    // A SoLoud::File over an engine File (owned)
    class SoloudEngineFile : public SoLoud::File {
        jasperengine::File file;
        bool opened = false;
    public:
        explicit SoloudEngineFile(const std::string& path);
        bool isOpen() const { return opened; }

        int eof() override;
        unsigned int read(unsigned char* aDst, unsigned int aBytes) override;
        unsigned int length() override;
        void seek(int aOffset) override;
        unsigned int pos() override;
        FILE* getFilePtr() override;
    };

    // A WavStream over an engine file: every voice opens its own SoloudEngineFile (SoLoud's
    // loadFile shares one file between all instances, so two voices of one clip would read over
    // each other)
    class EngineWavStream : public SoLoud::WavStream {
        std::string path;
        SoloudEngineFile* parsed = nullptr;   // the file parse read (SoLoud's mStreamFile)
    public:
        ~EngineWavStream() override;
        // false when the file does not open or is not audio (SoLoud's result in `result`)
        bool open(const std::string& path, SoLoud::result* result = nullptr);
        SoLoud::AudioSourceInstance* createInstance() override;
    };

}

#endif /* SoloudEngineFile_h */
