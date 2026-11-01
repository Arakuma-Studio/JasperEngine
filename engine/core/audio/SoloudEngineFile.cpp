// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SoloudEngineFile.h"

#include "audio/AudioFileCheck.h"
#include "io/FileData.h"

#include <cstdint>
#include <vector>

using namespace jasperengine;

SoloudEngineFile::SoloudEngineFile(const std::string& path) {
    opened = file.open(path.c_str()) == FileErrors::FILEDATA_OK && file.getFilePtr() != nullptr;
}

int SoloudEngineFile::eof() {
    return opened ? file.eof() : 1;
}

unsigned int SoloudEngineFile::read(unsigned char* aDst, unsigned int aBytes) {
    return opened ? file.read(aDst, aBytes) : 0;
}

unsigned int SoloudEngineFile::length() {
    return opened ? file.length() : 0;
}

void SoloudEngineFile::seek(int aOffset) {
    if (opened) file.seek(aOffset);
}

unsigned int SoloudEngineFile::pos() {
    return opened ? file.pos() : 0;
}

FILE* SoloudEngineFile::getFilePtr() {
    return opened ? file.getFilePtr() : nullptr;
}

EngineWavStream::~EngineWavStream() {
    // the voices go first: they may still read their own files
    stop();
    mStreamFile = nullptr;
    delete parsed;
}

bool EngineWavStream::open(const std::string& filePath, SoLoud::result* result, std::string* problem) {
    stop();
    mStreamFile = nullptr;
    delete parsed;
    parsed = nullptr;
    path = filePath;

    auto* file = new SoloudEngineFile(path);
    if (!file->isOpen()) {
        delete file;
        if (result) *result = SoLoud::FILE_NOT_FOUND;
        return false;
    }

    // AudioFileCheck reads the whole file once (Ogg pages, the last FLAC frame, ADPCM blocks): what
    // SoLoud's decoders would trust and must not is refused before they open it
    std::vector<unsigned char> bytes(file->length());
    const bool read = bytes.empty() || file->read(bytes.data(), (unsigned int)bytes.size()) == bytes.size();
    file->seek(0);
    AudioFileFormat format;
    std::string reason = read ? "" : "it can't be read";
    if (!read || !AudioFileCheck::prepare(bytes.data(), bytes.size(), AudioDecode::Stream, format, reason)) {
        delete file;
        if (result) *result = SoLoud::FILE_LOAD_FAILED;
        if (problem) *problem = reason;
        return false;
    }

    SoLoud::result res = loadFile(file);
    if (res != SoLoud::SO_NO_ERROR) {
        mStreamFile = nullptr;
        delete file;
        if (result) *result = res;
        return false;
    }
    // a WAV written to a pipe says 0xFFFFFFFF for its sizes, which dr_wav takes for the length (a
    // 1 s file streamed for 12 hours): the stream is the frames the file holds (prepare clamped
    // the copy's sizes; the voices read the file itself, whose reads end there)
    if ((uint64_t)mSampleCount > format.frames) mSampleCount = (unsigned int)format.frames;
    parsed = file;
    if (result) *result = SoLoud::SO_NO_ERROR;
    return true;
}

namespace {

    // a voice of a stream whose file went away: silent, and over at once
    class EndedInstance : public SoLoud::AudioSourceInstance {
    public:
        unsigned int getAudio(float* buffer, unsigned int, unsigned int bufferSize) override {
            for (unsigned int i = 0; i < bufferSize * mChannels; i++) buffer[i] = 0.0f;
            return 0;
        }
        bool hasEnded() override { return true; }
    };

}

SoLoud::AudioSourceInstance* EngineWavStream::createInstance() {
    // every voice reads a file of its own (openInstanceFile; the parsed file is never shared, so
    // no voice reads over another's position and none deletes another's file)
    auto* own = new SoloudEngineFile(path);
    if (!own->isOpen()) {
        delete own;
        return new EndedInstance();
    }
    pendingFile = own;
    auto* instance = new SoLoud::WavStreamInstance(this);
    // taken by the instance (or deleted by it when its decoder did not open)
    pendingFile = nullptr;
    return instance;
}

SoLoud::File* EngineWavStream::openInstanceFile() {
    SoLoud::File* file = pendingFile;
    pendingFile = nullptr;
    return file;
}
